/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: CC BY-NC-SA 4.0 (https://creativecommons.org/licenses/by-nc-sa/4.0/)
*/
#pragma once

#include "common.h"
#include "app_state.h"
#include "chat_log.h"
#include "protocol.h"
#include "room.h"
#include "storage.h"
#include "tls.h"
#include "transport.h"
#include "utils.h"

class ChatServer;

class ClientSession
    : public std::enable_shared_from_this<ClientSession>
{
public:
    ClientSession(tcp::socket socket, ChatServer& server,
        boost::asio::ssl::context* tls_context)
        : stream_(std::move(socket), tls_context), server_(server),
        buffer_(MAX_WIRE_LINE_LENGTH)
    {}

    void start();
    void deliver(const chatbox::protocol::ServerMessage& msg);
    void deliver_text(const std::string& text);
    void close();

    std::string nickname() const
    {
        std::lock_guard lock(nickname_mutex_);
        return nickname_;
    }
    chatbox::protocol::RoomId active_room_id() const
    {
        return active_room_id_.load();
    }

private:
    friend class ChatServer;

    void read_loop();
    void write_next();
    bool consume_message_quota();
    void set_active_room_id(chatbox::protocol::RoomId id)
    {
        active_room_id_ = id;
    }
    void set_nickname(const std::string& nickname)
    {
        std::lock_guard lock(nickname_mutex_);
        nickname_ = nickname;
    }

    TransportStream stream_;
    ChatServer& server_;
    boost::asio::streambuf buffer_;
    std::deque<std::string> write_queue_;
    std::map<chatbox::protocol::RoomId,
        std::deque<std::chrono::steady_clock::time_point>> recent_messages_by_room_;
    mutable std::mutex nickname_mutex_;
    std::string nickname_;
    std::string pending_nickname_;
    std::string pending_public_key_hex_;
    std::string pending_challenge_;
    std::atomic<chatbox::protocol::RoomId> active_room_id_{ 0 };
    bool authenticated_ = false;
    bool protocol_negotiated_ = false;
    bool close_after_write_ = false;
    bool suppress_leave_notice_ = false;
};

class ChatServer
{
public:
    using RoomId = chatbox::protocol::RoomId;
    static constexpr RoomId DEFAULT_ROOM_ID = 1;

    ChatServer(boost::asio::io_context& io, uint16_t port,
        const std::string& password = "", ChatLog* log = nullptr,
        const std::string& local_nickname = "",
        std::shared_ptr<chatbox::storage::ServerStorage> storage = nullptr,
        chatbox::tls::ServerConfig tls_config = {},
        std::size_t history_limit = SERVER_HISTORY_LIMIT)
        : io_(io), acceptor_(io), password_(password), log_(log),
        local_nickname_(local_nickname), storage_(std::move(storage)),
        tls_config_(std::move(tls_config)),
        history_limit_(std::max<std::size_t>(1, history_limit))
    {
        if (tls_config_.enabled)
        {
            tls_context_ = std::make_unique<boost::asio::ssl::context>(
                boost::asio::ssl::context::tls_server);
            chatbox::tls::configure_server_context(*tls_context_, tls_config_);
        }

        load_persistent_state();

        tcp::endpoint endpoint(tcp::v6(), port);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(boost::asio::ip::v6_only(false));
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();
        accept_loop();
    }

    uint16_t port() const { return acceptor_.local_endpoint().port(); }
    bool uses_tls() const { return tls_config_.enabled; }
    bool has_password() const { return !password_.empty(); }
    bool check_password(const std::string& pw) const { return pw == password_; }

    bool is_banned(const std::string& nick) const
    {
        std::lock_guard lock(ban_mutex_);
        return banned_nicks_.count(nick) > 0;
    }

    bool nickname_taken(const std::string& nick) const
    {
        if (!local_nickname_.empty() && local_nickname_ == nick)
            return true;

        std::lock_guard lock(sessions_mutex_);
        return std::any_of(sessions_.begin(), sessions_.end(),
            [&](const auto& session) { return session->nickname() == nick; });
    }

    bool identity_available_for(const std::string& nick,
        const std::string& public_key_hex,
        std::string& error) const
    {
        if (!is_valid_nickname(nick))
        {
            error = "Invalid nickname";
            return false;
        }

        if (!is_hex_of_len(public_key_hex, crypto_sign_PUBLICKEYBYTES))
        {
            error = "Invalid identity public key";
            return false;
        }

        std::lock_guard lock(identity_mutex_);
        auto it = identities_.find(nick);
        if (it != identities_.end() && it->second != public_key_hex)
        {
            error = "Nickname belongs to a different identity key";
            return false;
        }
        return true;
    }

    bool bind_identity(const std::string& nick, const std::string& public_key_hex)
    {
        std::lock_guard lock(identity_mutex_);
        auto it = identities_.find(nick);
        if (it != identities_.end())
            return it->second == public_key_hex;

        identities_[nick] = public_key_hex;
        if (storage_)
            storage_->store_identity(nick, public_key_hex);
        else
            save_identities_locked();
        if (log_)
            log_->write("[identity] Registered " + nick
                + " as " + identity_fingerprint(public_key_hex));
        return true;
    }

    void load_identities(const std::string& path)
    {
        if (path.empty())
            return;
        if (storage_)
        {
            storage_->import_legacy_files("", path);
            std::lock_guard lock(identity_mutex_);
            for (const auto& [nickname, key] : storage_->load_identities())
                identities_[nickname] = key;
            return;
        }
        identities_file_ = path;
        std::ifstream file(path);
        if (!file.is_open())
            return;

        std::string line;
        size_t count = 0;
        {
            std::lock_guard lock(identity_mutex_);
            while (std::getline(file, line))
            {
                auto parts = split(line, '|');
                if (parts.size() == 2 && is_valid_nickname(parts[0]) &&
                    is_hex_of_len(parts[1], crypto_sign_PUBLICKEYBYTES))
                {
                    identities_[parts[0]] = parts[1];
                    ++count;
                }
            }
        }

        if (log_)
            log_->write("[identity] Loaded " + std::to_string(count)
                + " identity binding(s) from " + path);
    }

    void import_legacy_files(const std::string& bans_path,
        const std::string& identities_path)
    {
        if (!storage_)
        {
            load_identities(identities_path);
            load_bans(bans_path);
            return;
        }

        const auto imported = storage_->import_legacy_files(
            bans_path, identities_path);
        {
            std::lock_guard lock(identity_mutex_);
            identities_.clear();
            for (const auto& [nickname, key] : storage_->load_identities())
                identities_[nickname] = key;
        }
        {
            std::lock_guard lock(ban_mutex_);
            banned_nicks_.clear();
            for (const auto& ban : storage_->load_bans(current_unix_seconds()))
                banned_nicks_.insert(ban.nickname);
        }
        if (log_ && (imported.bans || imported.identities))
            log_->write("[storage] Imported " + std::to_string(imported.bans)
                + " ban(s) and " + std::to_string(imported.identities)
                + " identity binding(s) into SQLite");
    }

    bool register_local_identity(const std::string& nick,
        const std::string& public_key_hex)
    {
        std::string error;
        return identity_available_for(nick, public_key_hex, error)
            && bind_identity(nick, public_key_hex);
    }

    void start_local_user()
    {
        if (local_nickname_.empty())
            return;

        chatbox::protocol::RoomJoined room;
        {
            std::lock_guard lock(rooms_mutex_);
            local_room_id_ = DEFAULT_ROOM_ID;
            const auto& lobby = rooms_.at(DEFAULT_ROOM_ID);
            room = { lobby.id, lobby.name, lobby.topic };
        }
        activate_room(room.id, room.name, room.topic);
        broadcast_users(room.id);
        broadcast_room_lists();
    }

    void join(std::shared_ptr<ClientSession> session)
    {
        std::lock_guard lock(sessions_mutex_);
        sessions_.insert(std::move(session));
    }

    void disconnect(std::shared_ptr<ClientSession> session, bool announce = true)
    {
        {
            std::lock_guard lock(sessions_mutex_);
            sessions_.erase(session);
        }

        const RoomId old_room_id = session->active_room_id();
        session->set_active_room_id(0);
        if (old_room_id == 0)
            return;

        bool removed = false;
        {
            std::lock_guard lock(rooms_mutex_);
            auto room = rooms_.find(old_room_id);
            if (room != rooms_.end())
                removed = room->second.members.erase(session) > 0;
        }

        if (removed && announce && !session->nickname().empty())
            broadcast_room(old_room_id,
                "[system] " + session->nickname() + " left");
        if (removed)
            broadcast_users(old_room_id);
        if (removed)
            broadcast_room_lists();
    }

    void admit_to_default_room(std::shared_ptr<ClientSession> session)
    {
        move_session_to_room(std::move(session), DEFAULT_ROOM_ID, false);
    }

    bool join_room(std::shared_ptr<ClientSession> session,
        const std::string& name,
        std::string& error)
    {
        RoomId room_id = 0;
        {
            std::lock_guard lock(rooms_mutex_);
            auto room = find_room_by_name_locked(name);
            if (room == rooms_.end())
            {
                error = "Room '" + name + "' does not exist";
                return false;
            }
            room_id = room->first;
        }

        if (session->active_room_id() == room_id)
        {
            error = "Already in room '" + name + "'";
            return false;
        }
        move_session_to_room(std::move(session), room_id, true);
        return true;
    }

    bool leave_room(std::shared_ptr<ClientSession> session, std::string& error)
    {
        if (session->active_room_id() == DEFAULT_ROOM_ID)
        {
            error = "Already in the lobby";
            return false;
        }
        move_session_to_room(std::move(session), DEFAULT_ROOM_ID, true);
        return true;
    }

    bool create_room(std::shared_ptr<ClientSession> session,
        const std::string& name,
        std::string& error)
    {
        if (!chatbox::protocol::is_valid_room_name(name))
        {
            error = "Room names use 1-32 letters, numbers, '-' or '_'";
            return false;
        }

        RoomId id = 0;
        {
            std::lock_guard lock(rooms_mutex_);
            if (rooms_.size() >= chatbox::protocol::MAX_ROOMS)
            {
                error = "This server has reached its room limit";
                return false;
            }
            if (find_room_by_name_locked(name) != rooms_.end())
            {
                error = "A room named '" + name + "' already exists";
                return false;
            }

            id = next_room_id_++;
            Room room;
            room.id = id;
            room.name = name;
            room.owner_nickname = session->nickname();
            rooms_.emplace(id, std::move(room));
        }

        persist_room(id);

        move_session_to_room(std::move(session), id, true);
        return true;
    }

    void send_room_list(const std::shared_ptr<ClientSession>& session) const
    {
        session->deliver(chatbox::protocol::ServerMessage{ room_list() });
    }

    void broadcast_room_lists() const
    {
        const auto list = room_list();
        std::vector<std::shared_ptr<ClientSession>> sessions;
        {
            std::lock_guard lock(sessions_mutex_);
            for (const auto& session : sessions_)
                if (!session->nickname().empty()) sessions.push_back(session);
        }
        for (auto& session : sessions)
            session->deliver(chatbox::protocol::ServerMessage{ list });
        if (!local_nickname_.empty() && local_room_id() != 0)
            update_room_list(list.rooms);
    }

    chatbox::protocol::RoomList room_list() const
    {
        chatbox::protocol::RoomList list;
        std::lock_guard lock(rooms_mutex_);
        for (const auto& [id, room] : rooms_)
        {
            std::size_t count = room.members.size();
            if (!local_nickname_.empty() && local_room_id_ == id)
                ++count;
            list.rooms.push_back({ id, room.name, count });
        }
        return list;
    }

    void show_room_list_local() const
    {
        const auto list = room_list();
        update_room_list(list.rooms);
        push_message("[system] Available rooms:");
        const RoomId current = local_room_id();
        for (const auto& room : list.rooms)
            push_message("[system]   "
                + std::string(room.id == current ? "* " : "- ")
                + room.name + " (" + std::to_string(room.user_count) + ")");
    }

    bool create_local_room(const std::string& name, std::string& error)
    {
        if (local_nickname_.empty())
        {
            error = "No interactive host is configured";
            return false;
        }
        if (!chatbox::protocol::is_valid_room_name(name))
        {
            error = "Room names use 1-32 letters, numbers, '-' or '_'";
            return false;
        }

        RoomId id = 0;
        {
            std::lock_guard lock(rooms_mutex_);
            if (rooms_.size() >= chatbox::protocol::MAX_ROOMS)
            {
                error = "This server has reached its room limit";
                return false;
            }
            if (find_room_by_name_locked(name) != rooms_.end())
            {
                error = "A room named '" + name + "' already exists";
                return false;
            }
            id = next_room_id_++;
            Room room;
            room.id = id;
            room.name = name;
            room.owner_nickname = local_nickname_;
            rooms_.emplace(id, std::move(room));
        }
        persist_room(id);
        move_local_to_room(id, true);
        return true;
    }

    bool create_admin_room(const std::string& name, std::string& error)
    {
        if (!chatbox::protocol::is_valid_room_name(name))
        {
            error = "Room names use 1-32 letters, numbers, '-' or '_'";
            return false;
        }
        {
            std::lock_guard lock(rooms_mutex_);
            if (rooms_.size() >= chatbox::protocol::MAX_ROOMS)
            {
                error = "This server has reached its room limit";
                return false;
            }
            if (find_room_by_name_locked(name) != rooms_.end())
            {
                error = "A room named '" + name + "' already exists";
                return false;
            }
            Room room;
            room.id = next_room_id_++;
            room.name = name;
            rooms_.emplace(room.id, std::move(room));
        }
        RoomId created_id = 0;
        {
            std::lock_guard lock(rooms_mutex_);
            auto room = find_room_by_name_locked(name);
            if (room != rooms_.end())
                created_id = room->first;
        }
        if (created_id != 0)
            persist_room(created_id);
        broadcast_room_lists();
        return true;
    }

    std::vector<std::string> users_in_room(
        const std::string& name, std::string& error) const
    {
        std::vector<std::string> users;
        std::lock_guard lock(rooms_mutex_);
        auto room = find_room_by_name_locked(name);
        if (room == rooms_.end())
        {
            error = "Room '" + name + "' does not exist";
            return users;
        }
        for (const auto& member : room->second.members)
            if (!member->nickname().empty()) users.push_back(member->nickname());
        if (!local_nickname_.empty() && local_room_id_ == room->first)
            users.push_back(local_nickname_);
        std::sort(users.begin(), users.end());
        return users;
    }

    bool set_admin_topic(const std::string& room_name,
        const std::string& topic,
        std::string& error)
    {
        RoomId id = 0;
        {
            std::lock_guard lock(rooms_mutex_);
            auto room = find_room_by_name_locked(room_name);
            if (room == rooms_.end())
            {
                error = "Room '" + room_name + "' does not exist";
                return false;
            }
            id = room->first;
        }
        return set_topic_for_room(id, "server", true, topic, error);
    }

    bool kick_user_in_room(const std::string& room_name,
        const std::string& nick,
        const std::string& reason,
        std::string& error)
    {
        RoomId id = 0;
        {
            std::lock_guard lock(rooms_mutex_);
            auto room = find_room_by_name_locked(room_name);
            if (room == rooms_.end())
            {
                error = "Room '" + room_name + "' does not exist";
                return false;
            }
            id = room->first;
        }
        if (!kick_user_in_room(id, nick, reason))
        {
            error = "User '" + nick + "' is not in #" + room_name;
            return false;
        }
        return true;
    }

    bool join_local_room(const std::string& name, std::string& error)
    {
        RoomId id = 0;
        {
            std::lock_guard lock(rooms_mutex_);
            auto room = find_room_by_name_locked(name);
            if (room == rooms_.end())
            {
                error = "Room '" + name + "' does not exist";
                return false;
            }
            id = room->first;
        }
        if (local_room_id() == id)
        {
            error = "Already in room '" + name + "'";
            return false;
        }
        move_local_to_room(id, true);
        return true;
    }

    bool leave_local_room(std::string& error)
    {
        if (local_room_id() == DEFAULT_ROOM_ID)
        {
            error = "Already in the lobby";
            return false;
        }
        move_local_to_room(DEFAULT_ROOM_ID, true);
        return true;
    }

    RoomId local_room_id() const
    {
        std::lock_guard lock(rooms_mutex_);
        return local_room_id_;
    }

    void broadcast(const std::string& msg)
    {
        RoomId id = local_room_id();
        if (id == 0)
            id = DEFAULT_ROOM_ID;
        broadcast_room(id, msg);
    }

    void broadcast_room(RoomId room_id, const std::string& msg,
        const std::string& nickname = "")
    {
        std::vector<std::shared_ptr<ClientSession>> members;
        bool show_locally = false;
        std::string room_name;
        {
            std::lock_guard lock(rooms_mutex_);
            auto room = rooms_.find(room_id);
            if (room == rooms_.end())
                return;

            room->second.recent_messages.push_back(msg);
            if (room->second.recent_messages.size() > history_limit_)
                room->second.recent_messages.pop_front();
            members.assign(room->second.members.begin(), room->second.members.end());
            show_locally = !local_nickname_.empty() && local_room_id_ == room_id;
            room_name = room->second.name;
        }

        if (show_locally)
            push_message(msg);
        for (auto& member : members)
            member->deliver(chatbox::protocol::ServerMessage{
                chatbox::protocol::RoomText{ room_id, msg } });
        if (log_)
            log_->write("[#" + room_name + "] " + msg);
        if (storage_)
        {
            chatbox::storage::StoredMessage stored;
            stored.id = random_hex(16);
            stored.room_id = room_id;
            stored.nickname = nickname;
            stored.body = msg;
            stored.sent_at = next_message_timestamp();
            storage_->store_message(stored);
        }
    }

    void broadcast_server(const std::string& msg)
    {
        std::vector<RoomId> ids;
        {
            std::lock_guard lock(rooms_mutex_);
            for (const auto& [id, room] : rooms_)
                ids.push_back(id);
        }
        for (RoomId id : ids)
            broadcast_room(id, "[SERVER] " + msg);
    }

    void broadcast_users(RoomId room_id)
    {
        chatbox::protocol::UserList users;
        users.room_id = room_id;
        std::vector<std::shared_ptr<ClientSession>> members;
        bool update_local = false;
        {
            std::lock_guard lock(rooms_mutex_);
            auto room = rooms_.find(room_id);
            if (room == rooms_.end())
                return;
            for (const auto& member : room->second.members)
            {
                if (!member->nickname().empty())
                    users.nicknames.push_back(member->nickname());
            }
            if (!local_nickname_.empty() && local_room_id_ == room_id)
            {
                users.nicknames.push_back(local_nickname_);
                update_local = true;
            }
            members.assign(room->second.members.begin(), room->second.members.end());
        }
        std::sort(users.nicknames.begin(), users.nicknames.end());

        if (update_local)
        {
            std::lock_guard lock(g_mutex);
            g_users = users.nicknames;
        }
        for (auto& member : members)
            member->deliver(chatbox::protocol::ServerMessage{ users });
    }

    bool send_private(const std::string& sender,
        const std::string& target,
        const std::string& message,
        bool mirror_to_local = false)
    {
        RoomId room_id = local_room_id();
        if (room_id == 0)
        {
            std::lock_guard lock(sessions_mutex_);
            for (const auto& session : sessions_)
            {
                if (session->nickname() == sender)
                {
                    room_id = session->active_room_id();
                    break;
                }
            }
        }
        return send_private_in_room(sender, target, message, room_id, mirror_to_local);
    }

    bool send_private_from(const std::shared_ptr<ClientSession>& sender,
        const std::string& target,
        const std::string& message)
    {
        return send_private_in_room(sender->nickname(), target, message,
            sender->active_room_id(), false);
    }

    void send_topic(const std::shared_ptr<ClientSession>& session) const
    {
        chatbox::protocol::RoomTopic topic;
        {
            std::lock_guard lock(rooms_mutex_);
            auto room = rooms_.find(session->active_room_id());
            if (room == rooms_.end())
                return;
            topic = { room->first, room->second.topic };
        }
        session->deliver(chatbox::protocol::ServerMessage{ topic });
    }

    bool set_topic(const std::shared_ptr<ClientSession>& session,
        const std::string& topic,
        std::string& error)
    {
        return set_topic_for_room(session->active_room_id(), session->nickname(),
            false, topic, error);
    }

    bool set_local_topic(const std::string& topic, std::string& error)
    {
        return set_topic_for_room(local_room_id(), local_nickname_, true,
            topic, error);
    }

    bool kick_from(const std::shared_ptr<ClientSession>& requester,
        const std::string& nick,
        const std::string& reason,
        std::string& error)
    {
        const RoomId room_id = requester->active_room_id();
        {
            std::lock_guard lock(rooms_mutex_);
            auto room = rooms_.find(room_id);
            if (room == rooms_.end() ||
                room->second.owner_nickname != requester->nickname())
            {
                error = "Only this room's creator can kick members";
                return false;
            }
        }
        if (nick == requester->nickname())
        {
            error = "You cannot kick yourself";
            return false;
        }
        if (!kick_user_in_room(room_id, nick, reason))
        {
            error = "User '" + nick + "' is not in this room";
            return false;
        }
        return true;
    }

    bool kick_user(const std::string& nick, const std::string& reason = "")
    {
        RoomId room_id = local_room_id();
        if (room_id == 0)
        {
            std::lock_guard lock(sessions_mutex_);
            for (const auto& session : sessions_)
            {
                if (session->nickname() == nick)
                {
                    room_id = session->active_room_id();
                    break;
                }
            }
        }
        return room_id != 0 && kick_user_in_room(room_id, nick, reason);
    }

    bool kick_user_in_room(RoomId room_id,
        const std::string& nick,
        const std::string& reason = "")
    {
        std::shared_ptr<ClientSession> target;
        {
            std::lock_guard lock(rooms_mutex_);
            auto room = rooms_.find(room_id);
            if (room == rooms_.end())
                return false;
            for (const auto& member : room->second.members)
            {
                if (member->nickname() == nick)
                {
                    target = member;
                    break;
                }
            }
        }
        if (!target)
            return false;

        target->deliver(chatbox::protocol::ServerMessage{
            chatbox::protocol::Kick{ nick, reason } });
        target->close();
        disconnect(target, false);
        broadcast_room(room_id, "[system] " + nick + " was kicked"
            + (reason.empty() ? "" : ": " + reason));
        broadcast_users(room_id);
        return true;
    }

    bool ban_user(const std::string& nick, const std::string& reason = "")
    {
        {
            std::lock_guard lock(ban_mutex_);
            banned_nicks_.insert(nick);
        }
        if (storage_)
        {
            storage_->store_ban({ nick, reason, current_unix_seconds(), std::nullopt });
        }
        else
        {
            save_bans();
        }

        std::shared_ptr<ClientSession> target;
        {
            std::lock_guard lock(sessions_mutex_);
            for (const auto& session : sessions_)
            {
                if (session->nickname() == nick)
                {
                    target = session;
                    break;
                }
            }
        }
        if (target)
        {
            const RoomId room_id = target->active_room_id();
            target->deliver(chatbox::protocol::ServerMessage{
                chatbox::protocol::Kick{ nick,
                    reason.empty() ? "banned" : reason } });
            target->close();
            disconnect(target, false);
            broadcast_room(room_id, "[system] " + nick + " has been banned");
            broadcast_users(room_id);
        }
        if (log_)
            log_->write("[admin] Banned: " + nick
                + (reason.empty() ? "" : " (" + reason + ")"));
        return true;
    }

    bool unban_user(const std::string& nick)
    {
        bool removed = false;
        {
            std::lock_guard lock(ban_mutex_);
            removed = banned_nicks_.erase(nick) > 0;
        }
        if (removed)
        {
            if (storage_)
                storage_->remove_ban(nick);
            else
                save_bans();
            if (log_)
                log_->write("[admin] Unbanned: " + nick);
        }
        return removed;
    }

    void load_bans(const std::string& path)
    {
        if (path.empty())
            return;
        if (storage_)
        {
            storage_->import_legacy_files(path, "");
            std::lock_guard lock(ban_mutex_);
            for (const auto& ban : storage_->load_bans(current_unix_seconds()))
                banned_nicks_.insert(ban.nickname);
            return;
        }
        bans_file_ = path;
        std::ifstream file(path);
        if (!file.is_open())
            return;

        std::string nick;
        {
            std::lock_guard lock(ban_mutex_);
            while (std::getline(file, nick))
                if (!nick.empty()) banned_nicks_.insert(nick);
        }
        if (log_)
            log_->write("[admin] Loaded " + std::to_string(ban_list().size())
                + " ban(s) from " + path);
    }

    std::set<std::string> ban_list() const
    {
        std::lock_guard lock(ban_mutex_);
        return banned_nicks_;
    }

    std::vector<std::string> connected_users() const
    {
        std::vector<std::string> users;
        if (!local_nickname_.empty())
            users.push_back(local_nickname_);
        {
            std::lock_guard lock(sessions_mutex_);
            for (const auto& session : sessions_)
                if (!session->nickname().empty()) users.push_back(session->nickname());
        }
        std::sort(users.begin(), users.end());
        return users;
    }

    void stop()
    {
        boost::asio::post(io_, [this]
            {
                boost::system::error_code ignored;
                acceptor_.close(ignored);
            });

        std::vector<std::shared_ptr<ClientSession>> sessions;
        {
            std::lock_guard lock(sessions_mutex_);
            sessions.assign(sessions_.begin(), sessions_.end());
            sessions_.clear();
        }
        {
            std::lock_guard lock(rooms_mutex_);
            for (auto& [id, room] : rooms_)
                room.members.clear();
        }
        for (auto& session : sessions)
            session->close();
        if (storage_)
            storage_->flush();
    }

private:
    using RoomMap = std::map<RoomId, Room>;

    static std::string normalized_room_name(std::string name)
    {
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return name;
    }

    RoomMap::iterator find_room_by_name_locked(const std::string& name)
    {
        const std::string wanted = normalized_room_name(name);
        return std::find_if(rooms_.begin(), rooms_.end(), [&](const auto& item)
            {
                return normalized_room_name(item.second.name) == wanted;
            });
    }

    RoomMap::const_iterator find_room_by_name_locked(const std::string& name) const
    {
        const std::string wanted = normalized_room_name(name);
        return std::find_if(rooms_.begin(), rooms_.end(), [&](const auto& item)
            {
                return normalized_room_name(item.second.name) == wanted;
            });
    }

    void move_session_to_room(std::shared_ptr<ClientSession> session,
        RoomId target_id,
        bool announce_move)
    {
        const RoomId old_id = session->active_room_id();
        chatbox::protocol::RoomJoined target;
        std::vector<std::string> history;
        {
            std::lock_guard lock(rooms_mutex_);
            auto target_room = rooms_.find(target_id);
            if (target_room == rooms_.end())
                return;

            auto old_room = rooms_.find(old_id);
            if (old_room != rooms_.end())
                old_room->second.members.erase(session);
            target_room->second.members.insert(session);
            session->set_active_room_id(target_id);
            target = { target_room->second.id,
                target_room->second.name,
                target_room->second.topic };
            history.assign(target_room->second.recent_messages.begin(),
                target_room->second.recent_messages.end());
        }

        if (announce_move && old_id != 0)
        {
            broadcast_room(old_id,
                "[system] " + session->nickname() + " left for #" + target.name);
            broadcast_users(old_id);
        }

        session->deliver(chatbox::protocol::ServerMessage{ target });
        if (!history.empty())
        {
            session->deliver_text("[system] Recent messages in #" + target.name + ":");
            for (const auto& message : history)
                session->deliver(chatbox::protocol::ServerMessage{
                    chatbox::protocol::RoomText{ target.id, message } });
        }
        broadcast_room(target.id,
            "[system] " + session->nickname() + " joined #" + target.name);
        broadcast_users(target.id);
        broadcast_room_lists();
    }

    void move_local_to_room(RoomId target_id, bool announce_move)
    {
        RoomId old_id = 0;
        chatbox::protocol::RoomJoined target;
        std::vector<std::string> history;
        {
            std::lock_guard lock(rooms_mutex_);
            auto target_room = rooms_.find(target_id);
            if (target_room == rooms_.end())
                return;
            old_id = local_room_id_;
            local_room_id_ = target_id;
            target = { target_room->second.id,
                target_room->second.name,
                target_room->second.topic };
            history.assign(target_room->second.recent_messages.begin(),
                target_room->second.recent_messages.end());
        }

        if (announce_move && old_id != 0)
        {
            broadcast_room(old_id,
                "[system] " + local_nickname_ + " left for #" + target.name);
            broadcast_users(old_id);
        }

        activate_room(target.id, target.name, target.topic);
        if (!history.empty())
        {
            push_message("[system] Recent messages in #" + target.name + ":");
            for (const auto& message : history)
                push_message(message);
        }
        broadcast_room(target.id,
            "[system] " + local_nickname_ + " joined #" + target.name);
        broadcast_users(target.id);
        broadcast_room_lists();
    }

    bool send_private_in_room(const std::string& sender,
        const std::string& target,
        const std::string& message,
        RoomId room_id,
        bool mirror_to_local)
    {
        std::shared_ptr<ClientSession> sender_session;
        std::shared_ptr<ClientSession> target_session;
        {
            std::lock_guard lock(rooms_mutex_);
            auto room = rooms_.find(room_id);
            if (room == rooms_.end())
                return false;
            for (const auto& member : room->second.members)
            {
                if (member->nickname() == sender) sender_session = member;
                if (member->nickname() == target) target_session = member;
            }
        }

        const bool target_is_local = !local_nickname_.empty() &&
            local_room_id() == room_id && target == local_nickname_;
        if (!target_session && !target_is_local)
        {
            const std::string error = "[system] User '" + target
                + "' is not in this room";
            if (sender_session) sender_session->deliver_text(error);
            if (mirror_to_local) push_message(error);
            return false;
        }

        const std::string line = "[PRIVATE " + timestamp() + "] "
            + sender + " -> " + target + ": " + message;
        if (target_is_local || mirror_to_local) push_message(line);
        if (target_session) target_session->deliver_text(line);
        if (sender_session && sender != target) sender_session->deliver_text(line);
        if (log_)
            log_->write("[private #" + std::to_string(room_id) + "] "
                + sender + " -> " + target + ": " + message);
        return true;
    }

    bool set_topic_for_room(RoomId room_id,
        const std::string& actor,
        bool privileged,
        const std::string& topic,
        std::string& error)
    {
        if (!chatbox::protocol::is_valid_room_topic(topic))
        {
            error = "Topics may contain at most 200 printable characters";
            return false;
        }

        std::vector<std::shared_ptr<ClientSession>> members;
        bool update_local = false;
        {
            std::lock_guard lock(rooms_mutex_);
            auto room = rooms_.find(room_id);
            if (room == rooms_.end())
            {
                error = "The active room no longer exists";
                return false;
            }
            if (!privileged && room->second.owner_nickname != actor)
            {
                error = "Only this room's creator can change its topic";
                return false;
            }
            room->second.topic = topic;
            members.assign(room->second.members.begin(), room->second.members.end());
            update_local = !local_nickname_.empty() && local_room_id_ == room_id;
        }

        if (storage_)
            storage_->update_room_topic(room_id, topic);

        const chatbox::protocol::ServerMessage update{
            chatbox::protocol::RoomTopic{ room_id, topic } };
        for (auto& member : members) member->deliver(update);
        if (update_local) update_room_topic(room_id, topic);
        broadcast_room(room_id, "[system] " + actor + " changed the topic to: " + topic);
        return true;
    }

    void accept_loop()
    {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket)
            {
                if (!ec)
                {
                    auto session = std::make_shared<ClientSession>(
                        std::move(socket), *this, tls_context_.get());
                    join(session);
                    session->start();
                }
                if (acceptor_.is_open()) accept_loop();
            });
    }

    void save_bans()
    {
        if (bans_file_.empty()) return;
        auto bans = ban_list();
        std::ofstream file(bans_file_, std::ios::trunc);
        for (const auto& nick : bans) file << nick << "\n";
    }

    void save_identities_locked()
    {
        if (identities_file_.empty()) return;
        std::ofstream file(identities_file_, std::ios::trunc);
        for (const auto& [nick, public_key_hex] : identities_)
            file << nick << "|" << public_key_hex << "\n";
    }

    static std::int64_t current_unix_seconds()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    static std::int64_t next_message_timestamp()
    {
        static std::atomic<std::int64_t> last{ 0 };
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto observed = last.load(std::memory_order_relaxed);
        while (true)
        {
            const auto candidate = std::max(now, observed + 1);
            if (last.compare_exchange_weak(observed, candidate,
                std::memory_order_relaxed))
                return candidate;
        }
    }

    void load_persistent_state()
    {
        if (storage_)
        {
            const auto stored_rooms = storage_->load_rooms();
            for (const auto& stored : stored_rooms)
            {
                Room room;
                room.id = stored.id;
                room.name = stored.name;
                room.topic = stored.topic;
                room.owner_nickname = stored.owner_nickname;
                auto history = storage_->load_history(room.id, history_limit_);
                std::reverse(history.messages.begin(), history.messages.end());
                for (const auto& message : history.messages)
                    room.recent_messages.push_back(message.body);
                next_room_id_ = std::max(next_room_id_, room.id + 1);
                rooms_.emplace(room.id, std::move(room));
            }
            for (const auto& [nickname, key] : storage_->load_identities())
                identities_[nickname] = key;
            for (const auto& ban : storage_->load_bans(current_unix_seconds()))
                banned_nicks_.insert(ban.nickname);
        }

        if (rooms_.find(DEFAULT_ROOM_ID) == rooms_.end())
        {
            Room lobby;
            lobby.id = DEFAULT_ROOM_ID;
            lobby.name = "lobby";
            lobby.topic = "Welcome to the lobby";
            lobby.owner_nickname = local_nickname_;
            rooms_.emplace(lobby.id, std::move(lobby));
            persist_room(DEFAULT_ROOM_ID);
        }
    }

    void persist_room(RoomId room_id)
    {
        if (!storage_)
            return;

        chatbox::storage::StoredRoom stored;
        {
            std::lock_guard lock(rooms_mutex_);
            const auto room = rooms_.find(room_id);
            if (room == rooms_.end())
                return;
            stored.id = room->second.id;
            stored.name = room->second.name;
            stored.topic = room->second.topic;
            stored.owner_nickname = room->second.owner_nickname;
            stored.created_at = current_unix_seconds();
        }
        storage_->store_room(stored);
    }

    boost::asio::io_context& io_;
    tcp::acceptor acceptor_;
    std::string password_;
    ChatLog* log_ = nullptr;
    std::string bans_file_;
    std::string identities_file_ = "identities.txt";
    std::string local_nickname_;
    std::shared_ptr<chatbox::storage::ServerStorage> storage_;
    chatbox::tls::ServerConfig tls_config_;
    std::unique_ptr<boost::asio::ssl::context> tls_context_;
    std::size_t history_limit_ = SERVER_HISTORY_LIMIT;

    mutable std::mutex sessions_mutex_;
    std::set<std::shared_ptr<ClientSession>> sessions_;

    mutable std::mutex rooms_mutex_;
    RoomMap rooms_;
    RoomId next_room_id_ = DEFAULT_ROOM_ID + 1;
    RoomId local_room_id_ = 0;

    mutable std::mutex ban_mutex_;
    std::set<std::string> banned_nicks_;

    mutable std::mutex identity_mutex_;
    std::map<std::string, std::string> identities_;
};
