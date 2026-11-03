/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: GNU GPLv3 (https://www.gnu.org/licenses/gpl-3.0.html)
*/
#pragma once

#include "common/common.h"
#include "app/app_state.h"
#include "protocol/protocol.h"
#include "network/tls.h"
#include "network/transport.h"
#include "common/utils.h"
enum class ConnectionFailure { None, Dns, Refused, Timeout, Tls, Cancelled, Network };

class ChatClient
{
public:
    explicit ChatClient(boost::asio::io_context& io,
        chatbox::tls::ClientConfig tls_config = {})
        : io_(io), resolver_(io), deadline_(io), tls_config_(std::move(tls_config)),
        tls_context_(boost::asio::ssl::context::tls_client),
        stream_(io, tls_config_.enabled ? &tls_context_ : nullptr),
        buffer_(MAX_WIRE_LINE_LENGTH)
    {
        chatbox::tls::configure_client_context(tls_context_, tls_config_);
    }

    // Start and poll on the same io_context thread. Keep the client alive until
    // cancelled handlers have drained, as with the established read/write API.
    void async_connect(const std::string& host, uint16_t port,
        std::function<void(bool)> completed,
        std::chrono::milliseconds phase_timeout = std::chrono::seconds(10))
    {
        attempt_lifetime_ = std::make_shared<int>(0);
        const std::weak_ptr<int> lifetime = attempt_lifetime_;
        connect_error_.clear();
        connection_failure_ = ConnectionFailure::None;
        connect_completed_ = std::move(completed);
        connecting_ = true;
        phase_timeout_ = phase_timeout;
        connection_phase_ = "Resolving address";
        arm_deadline();
        resolver_.async_resolve(host, std::to_string(port),
            [this, host, port, lifetime](boost::system::error_code ec, tcp::resolver::results_type endpoints) {
                if (lifetime.expired() || !connecting_) return;
                if (ec) { finish_connect(ConnectionFailure::Dns, ec.message()); return; }
                connection_phase_ = "Connecting TCP";
                arm_deadline();
                boost::asio::async_connect(stream_.lowest_layer(), endpoints,
                    [this, host, port, lifetime](boost::system::error_code ec, const tcp::endpoint&) {
                        if (lifetime.expired() || !connecting_) return;
                        if (ec) {
                            finish_connect(ec == boost::asio::error::connection_refused
                                ? ConnectionFailure::Refused : ConnectionFailure::Network, ec.message());
                            return;
                        }
                        if (!tls_config_.enabled) { finish_connect(); return; }
                        connection_phase_ = "Negotiating TLS";
                        arm_deadline();
                        try { chatbox::tls::set_server_name(stream_.native_tls_handle(), host); }
                        catch (const std::exception& error) {
                            finish_connect(ConnectionFailure::Tls, error.what()); return;
                        }
                        stream_.async_client_handshake([this, host, port, lifetime](boost::system::error_code ec) {
                            if (lifetime.expired() || !connecting_) return;
                            if (ec) { finish_connect(ConnectionFailure::Tls, ec.message()); return; }
                            std::string notice, error;
                            if (!chatbox::tls::verify_peer(stream_.native_tls_handle(), host, port,
                                tls_config_, notice, error)) {
                                finish_connect(ConnectionFailure::Tls, error); return;
                            }
                            if (!notice.empty()) push_message("[security] " + notice);
                            finish_connect();
                        });
                    });
            });
    }

    bool connect(const std::string& host, uint16_t port)
    {
        bool done = false, success = false;
        io_.restart();
        async_connect(host, port, [&](bool result) { success = result; done = true; });
        while (!done) io_.run_one();
        return success;
    }

    void cancel_connect()
    {
        if (connecting_) finish_connect(ConnectionFailure::Cancelled, "Connection cancelled.");
    }
    ConnectionFailure connection_failure() const { return connection_failure_; }
    const std::string& connection_phase() const { return connection_phase_; }

    bool connected() const { return connected_; }
    std::string disconnect_reason() const {
        std::lock_guard lock(disconnect_mutex_); return disconnect_reason_;
    }

    void send(const chatbox::protocol::ClientMessage& msg)
    {
        if (!connected_ || closing_) return;
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
                if (!connected_ || closing_) return;
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
            join_failed_ = true;
            join_failure_reason_ = "No local identity key loaded";
            join_resolved_ = true;
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
                connected_ = false;
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
    void arm_deadline()
    {
        const std::weak_ptr<int> lifetime = attempt_lifetime_;
        const auto generation = ++deadline_generation_;
        deadline_.expires_after(phase_timeout_);
        deadline_.async_wait([this, lifetime, generation](boost::system::error_code ec) {
            if (!lifetime.expired() && !ec && connecting_ && generation == deadline_generation_)
                finish_connect(ConnectionFailure::Timeout, connection_phase_ + " timed out.");
        });
    }

    void finish_connect(ConnectionFailure failure = ConnectionFailure::None,
        std::string error = {})
    {
        if (!connecting_) return;
        connecting_ = false;
        attempt_lifetime_.reset();
        deadline_.cancel();
        resolver_.cancel();
        connection_failure_ = failure;
        connect_error_ = std::move(error);
        if (failure != ConnectionFailure::None) stream_.close();
        else {
            connected_ = true;
            send(chatbox::protocol::ClientMessage{
                chatbox::protocol::Hello{ chatbox::protocol::VERSION } });
            read_loop();
        }
        auto completed = std::move(connect_completed_);
        completed(failure == ConnectionFailure::None);
    }

    void write_next()
    {
        stream_.async_write(
            boost::asio::buffer(write_queue_.front()),
            [this](boost::system::error_code ec, std::size_t)
            {
                if (ec)
                {
                    { std::lock_guard lock(disconnect_mutex_); disconnect_reason_ = ec.message(); }
                    connected_ = false;
                    stream_.close();
                    if (ec != boost::asio::error::operation_aborted && !closing_)
                        push_message("[system] Send failed: " + ec.message() + ". Delivery may be uncertain.");
                    return;
                }

                write_queue_.pop_front();
                if (!write_queue_.empty())
                {
                    write_next();
                    return;
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
                    { std::lock_guard lock(disconnect_mutex_); disconnect_reason_ = ec.message(); }
                    connected_ = false;
                    stream_.close();
                    if (!closing_ && !auth_resolved_)
                    {
                        auth_ok_ = false;
                        auth_resolved_ = true;
                        join_failed_ = true;
                        join_failure_reason_ = "Connection closed during protocol handshake";
                        join_resolved_ = true;
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
                        auth_ok_ = false;
                        auth_resolved_ = true;
                        join_failed_ = true;
                        join_failure_reason_ = parsed.detail;
                        join_resolved_ = true;
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
                    auth_ok_ = false;
                    auth_resolved_ = true;
                    join_failed_ = true;
                    join_failure_reason_ = "Server did not complete the protocol handshake";
                    join_resolved_ = true;
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
                    auth_ok_ = true;
                    auth_resolved_ = true;
                }
                else if (std::holds_alternative<chatbox::protocol::AuthRejected>(message))
                {
                    auth_ok_ = false;
                    auth_resolved_ = true;
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
                        join_failed_ = true;
                        join_failure_reason_ = "Identity challenge could not be answered";
                        join_resolved_ = true;
                    }
                }
                else if (const auto* accepted =
                    std::get_if<chatbox::protocol::IdentityAccepted>(&message))
                {
                    push_message("[system] Identity verified: " + accepted->fingerprint);
                }
                else if (std::holds_alternative<chatbox::protocol::Banned>(message))
                {
                    banned_ = true;
                    join_resolved_ = true;
                    push_message("[system] You are banned from this server");
                }
                else if (std::holds_alternative<chatbox::protocol::NicknameTaken>(message))
                {
                    nickname_taken_ = true;
                    join_resolved_ = true;
                    push_message("[system] Nickname is already in use");
                }
                else if (const auto* rejected =
                    std::get_if<chatbox::protocol::JoinRejected>(&message))
                {
                    join_failed_ = true;
                    join_failure_reason_ = rejected->reason;
                    join_resolved_ = true;
                    push_message("[system] Join failed: " + join_failure_reason_);
                }
                else if (const auto* kick = std::get_if<chatbox::protocol::Kick>(&message))
                {
                    kicked_ = true;
                    kick_reason_ = kick->reason;
                    push_message("[system] You have been kicked"
                        + (kick_reason_.empty() ? "" : ": " + kick_reason_));
                    mark_kicked();
                }
                else if (const auto* users =
                    std::get_if<chatbox::protocol::UserList>(&message))
                {
                    update_room_users(users->room_id, users->nicknames);
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

    boost::asio::io_context& io_;
    tcp::resolver resolver_;
    boost::asio::steady_timer deadline_;
    std::chrono::milliseconds phase_timeout_{10000};
    std::function<void(bool)> connect_completed_;
    std::size_t deadline_generation_ = 0;
    bool connecting_ = false;
    ConnectionFailure connection_failure_ = ConnectionFailure::None;
    std::string connection_phase_;
    chatbox::tls::ClientConfig tls_config_;
    boost::asio::ssl::context tls_context_;
    TransportStream stream_;
    boost::asio::streambuf buffer_;
    std::deque<std::string> write_queue_;

    mutable std::mutex disconnect_mutex_;
    std::string disconnect_reason_;
    std::atomic<bool> connected_{ false };
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
    std::shared_ptr<int> attempt_lifetime_;
};
