/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: CC BY-NC-SA 4.0 (https://creativecommons.org/licenses/by-nc-sa/4.0/)
*/
#pragma once

#include "common.h"
#include "app_state.h"
#include "chat_log.h"
#include "utils.h"
class ChatServer;

class ClientSession
    : public std::enable_shared_from_this<ClientSession>
{
public:
    ClientSession(tcp::socket socket, ChatServer& server)
        : socket_(std::move(socket)), server_(server),
        buffer_(MAX_WIRE_LINE_LENGTH)
    {}

    void start();
    void deliver(const std::string& msg);
    void close();

    // The nickname is set once identity proof is accepted
    const std::string& nickname() const { return nickname_; }

private:
    void read_loop();
    void write_next();
    bool consume_message_quota();

    tcp::socket   socket_;
    ChatServer& server_;
    boost::asio::streambuf buffer_;
    std::deque<std::string> write_queue_;
    std::deque<std::chrono::steady_clock::time_point> recent_messages_;
    std::string   nickname_;
    std::string   pending_nickname_;
    std::string   pending_public_key_hex_;
    std::string   pending_challenge_;
    bool          authenticated_ = false;   // true after password accepted
    bool          close_after_write_ = false;
    bool          suppress_leave_notice_ = false;
};

// =====================================================
// Server
// =====================================================

class ChatServer
{
public:
    ChatServer(boost::asio::io_context& io, uint16_t port,
        const std::string& password = "", ChatLog* log = nullptr,
        const std::string& local_nickname = "")
        : io_(io), acceptor_(io), password_(password), log_(log),
        local_nickname_(local_nickname)
    {
        tcp::endpoint endpoint(tcp::v6(), port);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(boost::asio::ip::v6_only(false));
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();
        accept_loop();
    }

    // Returns true when the server requires a password
    bool has_password() const { return !password_.empty(); }

    // Returns true when the supplied password matches
    bool check_password(const std::string& pw) const { return pw == password_; }

    // Returns true if the nickname is on the ban list
    bool is_banned(const std::string& nick) const
    {
        std::lock_guard lock(ban_mutex_);
        return banned_nicks_.count(nick) > 0;
    }

    bool nickname_taken(const std::string& nick) const
    {
        auto users = ::connected_users();
        return std::find(users.begin(), users.end(), nick) != users.end();
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
        save_identities_locked();
        if (log_)
            log_->write("[identity] Registered " + nick
                + " as " + identity_fingerprint(public_key_hex));
        return true;
    }

    void load_identities(const std::string& path)
    {
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
                if (parts.size() == 2 &&
                    is_valid_nickname(parts[0]) &&
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

    bool register_local_identity(const std::string& nick, const std::string& public_key_hex)
    {
        std::string error;
        return identity_available_for(nick, public_key_hex, error)
            && bind_identity(nick, public_key_hex);
    }

    void join(std::shared_ptr<ClientSession> session)
    {
        std::lock_guard lock(sessions_mutex_);
        sessions_.insert(session);
    }

    void leave(std::shared_ptr<ClientSession> session)
    {
        std::lock_guard lock(sessions_mutex_);
        sessions_.erase(session);
    }

    void broadcast(const std::string& msg)
    {
        push_message(msg);
        remember_history(msg);
        if (log_)
            log_->write(msg);

        std::vector<std::shared_ptr<ClientSession>> sessions;
        {
            std::lock_guard lock(sessions_mutex_);
            sessions.assign(sessions_.begin(), sessions_.end());
        }

        for (auto& s : sessions)
            s->deliver(msg);
    }

    void broadcast_server(const std::string& msg)
    {
        broadcast("[SERVER] " + msg);
    }

    bool send_private(const std::string& sender,
        const std::string& target,
        const std::string& message,
        bool mirror_to_local = false)
    {
        std::shared_ptr<ClientSession> sender_session;
        std::shared_ptr<ClientSession> target_session;
        {
            std::lock_guard lock(sessions_mutex_);
            for (auto& s : sessions_)
            {
                if (s->nickname() == sender)
                    sender_session = s;
                if (s->nickname() == target)
                    target_session = s;
            }
        }

        const bool target_is_local =
            !local_nickname_.empty() && target == local_nickname_;

        if (!target_session && !target_is_local)
        {
            std::string error = "[system] User '" + target + "' not found";
            if (sender_session)
                sender_session->deliver(error);
            if (mirror_to_local)
                push_message(error);
            return false;
        }

        std::string line = "[PRIVATE " + timestamp() + "] "
            + sender + " -> " + target + ": " + message;

        if (target_is_local || mirror_to_local)
            push_message(line);

        if (target_session)
            target_session->deliver(line);

        if (sender_session && sender != target)
            sender_session->deliver(line);

        if (log_)
            log_->write("[private] " + sender + " -> " + target + ": " + message);

        return true;
    }

    void deliver_history(std::shared_ptr<ClientSession> session) const
    {
        std::vector<std::string> history;
        {
            std::lock_guard lock(history_mutex_);
            history.assign(history_.begin(), history_.end());
        }

        if (history.empty())
            return;

        session->deliver("[system] Recent messages:");
        for (const auto& msg : history)
            session->deliver(msg);
    }

    void broadcast_users()
    {
        std::string payload = "USERS|";
        {
            std::lock_guard lock(g_mutex);
            bool first = true;
            for (const auto& u : g_users)
            {
                if (!first) payload += ',';
                payload += u;
                first = false;
            }
        }

        std::vector<std::shared_ptr<ClientSession>> sessions;
        {
            std::lock_guard lock(sessions_mutex_);
            sessions.assign(sessions_.begin(), sessions_.end());
        }

        for (auto& s : sessions)
            s->deliver(payload);
    }

    void stop()
    {
        boost::asio::post(io_,
            [this]
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

        for (auto& s : sessions)
            s->close();
    }

    // Kick a user by nickname: send KICK message to that session then drop it.
    // Returns true if the user was found.
    bool kick_user(const std::string& nick, const std::string& reason = "")
    {
        std::shared_ptr<ClientSession> target;
        {
            std::lock_guard lock(sessions_mutex_);
            for (auto& s : sessions_)
            {
                if (s->nickname() == nick)
                {
                    target = s;
                    break;
                }
            }
        }

        if (!target)
            return false;

        std::string kick_msg = "KICK|" + nick;
        if (!reason.empty())
            kick_msg += "|" + reason;

        target->deliver(kick_msg);
        target->close();
        leave(target);
        remove_user(nick);
        broadcast("[system] " + nick + " was kicked"
            + (reason.empty() ? "" : ": " + reason));
        broadcast_users();
        return true;
    }

    // Ban a user by nickname: kicks them and adds to the ban list.
    bool ban_user(const std::string& nick, const std::string& reason = "")
    {
        {
            std::lock_guard lock(ban_mutex_);
            banned_nicks_.insert(nick);
        }
        save_bans();
        // If they're currently connected, kick them too
        kick_user(nick, reason.empty() ? "banned" : reason);
        broadcast("[system] " + nick + " has been banned");
        if (log_)
            log_->write("[admin] Banned: " + nick + (reason.empty() ? "" : " (" + reason + ")"));
        return true;
    }

    // Unban a user
    bool unban_user(const std::string& nick)
    {
        bool removed = false;
        {
            std::lock_guard lock(ban_mutex_);
            removed = banned_nicks_.erase(nick) > 0;
        }

        if (removed)
        {
            save_bans();
            if (log_)
                log_->write("[admin] Unbanned: " + nick);
        }

        return removed;
    }

    void load_bans(const std::string& path)
    {
        bans_file_ = path;

        std::ifstream file(path);
        if (!file.is_open())
            return;

        std::string nick;
        {
            std::lock_guard lock(ban_mutex_);
            while (std::getline(file, nick))
            {
                if (!nick.empty())
                    banned_nicks_.insert(nick);
            }
        }

        if (log_)
            log_->write("[admin] Loaded " + std::to_string(ban_list().size()) + " ban(s) from " + path);
    }

    std::set<std::string> ban_list() const
    {
        std::lock_guard lock(ban_mutex_);
        return banned_nicks_;
    }

    std::vector<std::string> connected_users() const
    {
        return ::connected_users();
    }

private:
    void remember_history(const std::string& msg)
    {
        std::lock_guard lock(history_mutex_);
        history_.push_back(msg);
        if (history_.size() > SERVER_HISTORY_LIMIT)
            history_.pop_front();
    }

    void accept_loop()
    {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket)
            {
                if (!ec)
                {
                    auto session = std::make_shared<ClientSession>(
                        std::move(socket), *this);
                    join(session);
                    session->start();
                }

                if (acceptor_.is_open())
                    accept_loop();
            });
    }

    void save_bans()
    {
        if (bans_file_.empty())
            return;

        auto bans = ban_list();
        std::ofstream file(bans_file_, std::ios::trunc);
        for (const auto& nick : bans)
            file << nick << "\n";
    }

    void save_identities_locked()
    {
        if (identities_file_.empty())
            return;

        std::ofstream file(identities_file_, std::ios::trunc);
        for (const auto& [nick, public_key_hex] : identities_)
            file << nick << "|" << public_key_hex << "\n";
    }

    boost::asio::io_context& io_;
    tcp::acceptor acceptor_;
    std::string   password_;
    ChatLog*      log_ = nullptr;
    std::string   bans_file_;
    std::string   identities_file_ = "identities.txt";
    std::string   local_nickname_;

    mutable std::mutex sessions_mutex_;
    std::set<std::shared_ptr<ClientSession>> sessions_;

    mutable std::mutex ban_mutex_;
    std::set<std::string> banned_nicks_;

    mutable std::mutex identity_mutex_;
    std::map<std::string, std::string> identities_;

    mutable std::mutex history_mutex_;
    std::deque<std::string> history_;
};
