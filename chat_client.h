/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: CC BY-NC-SA 4.0 (https://creativecommons.org/licenses/by-nc-sa/4.0/)
*/
#pragma once

#include "common.h"
#include "app_state.h"
#include "protocol.h"
#include "tls.h"
#include "transport.h"
#include "utils.h"
class ChatClient
{
public:
    explicit ChatClient(boost::asio::io_context& io,
        chatbox::tls::ClientConfig tls_config = {})
        : tls_config_(std::move(tls_config)),
        tls_context_(boost::asio::ssl::context::tls_client),
        stream_(io, tls_config_.enabled ? &tls_context_ : nullptr),
        buffer_(MAX_WIRE_LINE_LENGTH)
    {
        chatbox::tls::configure_client_context(tls_context_, tls_config_);
    }

    // connect() returns false on a synchronous resolve or connection failure.
    bool connect(const std::string& host, uint16_t port)
    {
        try
        {
            tcp::resolver resolver(stream_.get_executor());
            auto endpoints = resolver.resolve(host, std::to_string(port));
            boost::asio::connect(stream_.lowest_layer(), endpoints);
            if (tls_config_.enabled)
            {
                chatbox::tls::set_server_name(stream_.native_tls_handle(), host);
                boost::system::error_code handshake_error;
                stream_.client_handshake(handshake_error);
                if (handshake_error)
                {
                    connect_error_ = "TLS handshake failed: " + handshake_error.message();
                    stream_.close();
                    return false;
                }
                std::string notice;
                if (!chatbox::tls::verify_peer(stream_.native_tls_handle(), host, port,
                    tls_config_, notice, connect_error_))
                {
                    stream_.close();
                    return false;
                }
                if (!notice.empty())
                    push_message("[security] " + notice);
            }
            send(chatbox::protocol::ClientMessage{
                chatbox::protocol::Hello{ chatbox::protocol::VERSION } });
            read_loop();
            return true;
        }
        catch (const std::exception& ex)
        {
            connect_error_ = ex.what();
            return false;
        }
    }

    void send(const chatbox::protocol::ClientMessage& msg)
    {
        auto encoded = chatbox::protocol::encode_client_frame(msg);
        if (!encoded)
        {
            push_message("[system] Could not encode message: " + encoded.detail);
            return;
        }

        boost::asio::post(
            stream_.get_executor(),
            [this, wire = std::move(*encoded.value)]
            {
                const bool writing = !write_queue_.empty();
                write_queue_.push_back(wire);
                if (!writing)
                    write_next();
            });
    }

    void set_identity(const ClientIdentity& identity)
    {
        identity_ = identity;
    }

    void identify(const std::string& nick)
    {
        if (!identity_)
        {
            join_resolved_ = true;
            join_failed_ = true;
            join_failure_reason_ = "No local identity key loaded";
            return;
        }

        send(chatbox::protocol::ClientMessage{
            chatbox::protocol::Identify{ nick, identity_->public_key_hex } });
    }

    void request_rooms()
    {
        room_list_requested_ = true;
        send(chatbox::protocol::ClientMessage{ chatbox::protocol::ListRooms{} });
    }

    void close()
    {
        closing_ = true;
        boost::asio::post(
            stream_.get_executor(),
            [this]
            {
                if (!write_queue_.empty())
                {
                    close_after_write_ = true;
                    return;
                }

                stream_.close();
            });
    }

    // AUTH state accessors (polled by main thread during login)
    bool auth_required()       const { return auth_required_; }
    bool auth_resolved()       const { return auth_resolved_; }
    bool auth_ok()             const { return auth_ok_; }
    bool join_resolved()       const { return join_resolved_; }
    bool was_kicked()          const { return kicked_; }
    bool was_banned()          const { return banned_; }
    bool nickname_taken()      const { return nickname_taken_; }
    bool join_failed()         const { return join_failed_; }
    const std::string& join_failure_reason() const { return join_failure_reason_; }
    const std::string& kick_reason() const { return kick_reason_; }
    const std::string& connect_error() const { return connect_error_; }

private:
    void write_next()
    {
        stream_.async_write(
            boost::asio::buffer(write_queue_.front()),
            [this](boost::system::error_code ec, std::size_t)
            {
                if (ec)
                {
                    if (ec != boost::asio::error::operation_aborted && !closing_)
                        std::cerr << "Send error: " << ec.message() << "\n";
                    return;
                }

                write_queue_.pop_front();
                if (!write_queue_.empty())
                {
                    write_next();
                    return;
                }

                if (close_after_write_)
                {
                    stream_.close();
                }
            });
    }

    void read_loop()
    {
        stream_.async_read_until(
            buffer_, '\n',
            [this](boost::system::error_code ec, std::size_t)
            {
                if (ec)
                {
                    if (!closing_ && !auth_resolved_)
                    {
                        auth_resolved_ = true;
                        auth_ok_ = false;
                        join_resolved_ = true;
                        join_failed_ = true;
                        join_failure_reason_ = "Connection closed during protocol handshake";
                    }
                    if (!closing_ && !kicked_ && !banned_ && auth_ok_)
                        push_message("[system] disconnected");
                    return;
                }

                std::istream is(&buffer_);
                std::string  line;
                std::getline(is, line);
                auto parsed = chatbox::protocol::decode_server_frame(line);
                if (!parsed)
                {
                    push_message("[system] Ignored invalid server frame: " + parsed.detail);
                    if (parsed.error == chatbox::protocol::Error::UnsupportedVersion)
                    {
                        auth_resolved_ = true;
                        auth_ok_ = false;
                        join_resolved_ = true;
                        join_failed_ = true;
                        join_failure_reason_ = parsed.detail;
                        close();
                        return;
                    }
                    read_loop();
                    return;
                }

                const auto& message = *parsed.value;
                if (const auto* hello = std::get_if<chatbox::protocol::Hello>(&message))
                {
                    protocol_negotiated_ =
                        hello->version == chatbox::protocol::VERSION;
                }
                else if (!protocol_negotiated_)
                {
                    auth_resolved_ = true;
                    auth_ok_ = false;
                    join_resolved_ = true;
                    join_failed_ = true;
                    join_failure_reason_ = "Server did not complete the protocol handshake";
                    push_message("[system] " + join_failure_reason_);
                    close();
                    return;
                }
                // Server signals
                else if (std::holds_alternative<chatbox::protocol::AuthRequired>(message))
                {
                    auth_required_ = true;
                }
                else if (std::holds_alternative<chatbox::protocol::AuthAccepted>(message))
                {
                    auth_resolved_ = true;
                    auth_ok_ = true;
                }
                else if (std::holds_alternative<chatbox::protocol::AuthRejected>(message))
                {
                    auth_resolved_ = true;
                    auth_ok_ = false;
                    push_message("[system] Wrong password - disconnected");
                }
                else if (std::holds_alternative<chatbox::protocol::JoinAccepted>(message))
                {
                    join_resolved_ = true;
                }
                else if (const auto* challenge =
                    std::get_if<chatbox::protocol::Challenge>(&message))
                {
                    if (identity_)
                    {
                        send(chatbox::protocol::ClientMessage{
                            chatbox::protocol::IdentityProof{
                                sign_identity_challenge(*identity_, challenge->challenge_hex) } });
                    }
                    else
                    {
                        join_resolved_ = true;
                        join_failed_ = true;
                        join_failure_reason_ = "Identity challenge could not be answered";
                    }
                }
                else if (const auto* accepted =
                    std::get_if<chatbox::protocol::IdentityAccepted>(&message))
                {
                    push_message("[system] Identity verified: " + accepted->fingerprint);
                }
                else if (std::holds_alternative<chatbox::protocol::Banned>(message))
                {
                    join_resolved_ = true;
                    banned_ = true;
                    push_message("[system] You are banned from this server");
                }
                else if (std::holds_alternative<chatbox::protocol::NicknameTaken>(message))
                {
                    join_resolved_ = true;
                    nickname_taken_ = true;
                    push_message("[system] Nickname is already in use");
                }
                else if (const auto* rejected =
                    std::get_if<chatbox::protocol::JoinRejected>(&message))
                {
                    join_resolved_ = true;
                    join_failed_ = true;
                    join_failure_reason_ = rejected->reason;
                    push_message("[system] Join failed: " + join_failure_reason_);
                }
                else if (const auto* kick = std::get_if<chatbox::protocol::Kick>(&message))
                {
                    kicked_ = true;
                    kick_reason_ = kick->reason;
                    push_message("[system] You have been kicked"
                        + (kick_reason_.empty() ? "" : ": " + kick_reason_));
                    g_kicked = true;   // signal main loop
                }
                else if (const auto* users =
                    std::get_if<chatbox::protocol::UserList>(&message))
                {
                    if (users->room_id == active_room_id())
                    {
                        std::lock_guard lock(g_mutex);
                        g_users = users->nicknames;
                    }
                }
                else if (const auto* rooms =
                    std::get_if<chatbox::protocol::RoomList>(&message))
                {
                    update_room_list(rooms->rooms);
                    if (room_list_requested_.exchange(false))
                    {
                        const auto current = active_room_id();
                        push_message("[system] Available rooms:");
                        for (const auto& room : rooms->rooms)
                            push_message("[system]   "
                                + std::string(room.id == current ? "* " : "- ")
                                + room.name + " (" + std::to_string(room.user_count) + ")");
                    }
                }
                else if (const auto* room =
                    std::get_if<chatbox::protocol::RoomJoined>(&message))
                {
                    activate_room(room->id, room->name, room->topic);
                    push_message("[system] Entered #" + room->name
                        + (room->topic.empty() ? "" : " - " + room->topic));
                }
                else if (const auto* topic =
                    std::get_if<chatbox::protocol::RoomTopic>(&message))
                {
                    update_room_topic(topic->room_id, topic->topic);
                    if (topic->room_id == active_room_id())
                        push_message("[system] Topic: "
                            + (topic->topic.empty() ? "(none)" : topic->topic));
                }
                else if (const auto* room_text =
                    std::get_if<chatbox::protocol::RoomText>(&message))
                {
                    if (room_text->room_id == active_room_id())
                        push_message(room_text->body);
                }
                else if (const auto* text = std::get_if<chatbox::protocol::Text>(&message))
                {
                    push_message(text->body);
                }
                else if (const auto* error =
                    std::get_if<chatbox::protocol::ServerError>(&message))
                {
                    push_message("[system] " + error->reason);
                }

                read_loop();
            });
    }

    chatbox::tls::ClientConfig tls_config_;
    boost::asio::ssl::context tls_context_;
    TransportStream stream_;
    boost::asio::streambuf buffer_;
    std::deque<std::string> write_queue_;

    std::atomic<bool> auth_required_{ false };
    std::atomic<bool> auth_resolved_{ false };
    std::atomic<bool> auth_ok_{ false };
    std::atomic<bool> join_resolved_{ false };
    std::atomic<bool> kicked_{ false };
    std::atomic<bool> banned_{ false };
    std::atomic<bool> nickname_taken_{ false };
    std::atomic<bool> join_failed_{ false };
    std::atomic<bool> closing_{ false };
    std::atomic<bool> protocol_negotiated_{ false };
    std::atomic<bool> room_list_requested_{ false };
    std::optional<ClientIdentity> identity_;
    std::string       kick_reason_;
    std::string       join_failure_reason_;
    std::string       connect_error_;
    bool              close_after_write_ = false;
};
