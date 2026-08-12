#include "chat_server.h"

void ClientSession::start()
{
    deliver(chatbox::protocol::ServerMessage{
        chatbox::protocol::Hello{ chatbox::protocol::VERSION } });

    read_loop();
}

void ClientSession::read_loop()
{
    auto self = shared_from_this();

    boost::asio::async_read_until(
        socket_, buffer_, '\n',
        [this, self](boost::system::error_code ec, std::size_t)
        {
            if (ec)
            {
                server_.leave(self);
                if (!nickname_.empty() && !suppress_leave_notice_)
                {
                    remove_user(nickname_);
                    server_.broadcast("[system] " + nickname_ + " left");
                    server_.broadcast_users();
                }
                return;
            }

            std::istream is(&buffer_);
            std::string  line;
            std::getline(is, line);
            auto parsed = chatbox::protocol::decode_client_frame(line);
            if (!parsed)
            {
                deliver(chatbox::protocol::ServerMessage{
                    chatbox::protocol::ServerError{
                        "Invalid client frame: " + parsed.detail } });
                if (parsed.error == chatbox::protocol::Error::UnsupportedVersion)
                {
                    close();
                    server_.leave(self);
                    return;
                }
                read_loop();
                return;
            }

            const auto& message = *parsed.value;
            if (const auto* hello = std::get_if<chatbox::protocol::Hello>(&message))
            {
                if (protocol_negotiated_)
                {
                    deliver(chatbox::protocol::ServerMessage{
                        chatbox::protocol::ServerError{ "Protocol handshake already completed" } });
                }
                else
                {
                    protocol_negotiated_ = hello->version == chatbox::protocol::VERSION;
                    if (server_.has_password())
                    {
                        deliver(chatbox::protocol::ServerMessage{
                            chatbox::protocol::AuthRequired{} });
                    }
                    else
                    {
                        deliver(chatbox::protocol::ServerMessage{
                            chatbox::protocol::AuthAccepted{} });
                    }
                }
                read_loop();
                return;
            }

            if (!protocol_negotiated_)
            {
                deliver(chatbox::protocol::ServerMessage{
                    chatbox::protocol::ServerError{ "Protocol handshake required" } });
                close();
                server_.leave(self);
                return;
            }

            // ---- AUTH ----
            if (const auto* auth = std::get_if<chatbox::protocol::Authenticate>(&message))
            {
                if (server_.check_password(auth->password))
                {
                    authenticated_ = true;
                    deliver(chatbox::protocol::ServerMessage{
                        chatbox::protocol::AuthAccepted{} });
                }
                else
                {
                    deliver(chatbox::protocol::ServerMessage{
                        chatbox::protocol::AuthRejected{} });
                    close();
                    server_.leave(self);
                    return;
                }
                read_loop();
                return;
            }

            // Reject all other messages from unauthenticated sessions
            if (server_.has_password() && !authenticated_)
            {
                deliver(chatbox::protocol::ServerMessage{
                    chatbox::protocol::AuthRequired{} });
                read_loop();
                return;
            }

            // ---- IDENTIFY ----
            if (const auto* identify = std::get_if<chatbox::protocol::Identify>(&message))
            {
                const std::string& nick = identify->nickname;
                const std::string& public_key_hex = identify->public_key_hex;

                if (!nickname_.empty())
                {
                    deliver(chatbox::protocol::ServerMessage{
                        chatbox::protocol::JoinRejected{ "Already joined" } });
                    read_loop();
                    return;
                }

                std::string identity_error;
                if (!server_.identity_available_for(nick, public_key_hex, identity_error))
                {
                    deliver(chatbox::protocol::ServerMessage{
                        chatbox::protocol::JoinRejected{ identity_error } });
                    close();
                    server_.leave(self);
                    return;
                }

                if (server_.is_banned(nick))
                {
                    deliver(chatbox::protocol::ServerMessage{ chatbox::protocol::Banned{} });
                    close();
                    server_.leave(self);
                    return;
                }

                if (server_.nickname_taken(nick))
                {
                    deliver(chatbox::protocol::ServerMessage{
                        chatbox::protocol::NicknameTaken{} });
                    close();
                    server_.leave(self);
                    return;
                }

                pending_nickname_ = nick;
                pending_public_key_hex_ = public_key_hex;
                pending_challenge_ = random_hex(IDENTITY_CHALLENGE_BYTES);
                deliver(chatbox::protocol::ServerMessage{
                    chatbox::protocol::Challenge{ pending_challenge_ } });
            }
            // ---- PROOF ----
            else if (const auto* proof =
                std::get_if<chatbox::protocol::IdentityProof>(&message))
            {
                if (!nickname_.empty())
                {
                    deliver(chatbox::protocol::ServerMessage{
                        chatbox::protocol::JoinRejected{ "Already joined" } });
                    read_loop();
                    return;
                }

                if (pending_nickname_.empty() || pending_public_key_hex_.empty() ||
                    pending_challenge_.empty())
                {
                    deliver(chatbox::protocol::ServerMessage{
                        chatbox::protocol::JoinRejected{
                            "Identify before sending proof" } });
                    close();
                    server_.leave(self);
                    return;
                }

                if (!verify_identity_signature(
                    pending_public_key_hex_,
                    pending_challenge_,
                    proof->signature_hex))
                {
                    deliver(chatbox::protocol::ServerMessage{
                        chatbox::protocol::JoinRejected{ "Identity proof failed" } });
                    close();
                    server_.leave(self);
                    return;
                }

                if (server_.is_banned(pending_nickname_))
                {
                    deliver(chatbox::protocol::ServerMessage{ chatbox::protocol::Banned{} });
                    close();
                    server_.leave(self);
                    return;
                }

                if (server_.nickname_taken(pending_nickname_))
                {
                    deliver(chatbox::protocol::ServerMessage{
                        chatbox::protocol::NicknameTaken{} });
                    close();
                    server_.leave(self);
                    return;
                }

                if (!server_.bind_identity(pending_nickname_, pending_public_key_hex_))
                {
                    deliver(chatbox::protocol::ServerMessage{
                        chatbox::protocol::JoinRejected{
                            "Identity registration failed" } });
                    close();
                    server_.leave(self);
                    return;
                }

                nickname_ = pending_nickname_;
                const std::string fingerprint = identity_fingerprint(pending_public_key_hex_);
                pending_nickname_.clear();
                pending_public_key_hex_.clear();
                pending_challenge_.clear();

                add_user(nickname_);
                deliver(chatbox::protocol::ServerMessage{
                    chatbox::protocol::IdentityAccepted{ fingerprint } });
                deliver(chatbox::protocol::ServerMessage{
                    chatbox::protocol::JoinAccepted{} });
                server_.deliver_history(self);
                server_.broadcast("[system] " + nickname_ + " joined");
                server_.broadcast_users();
            }
            // ---- LEAVE ----
            else if (std::holds_alternative<chatbox::protocol::Leave>(message))
            {
                if (nickname_.empty())
                {
                    deliver_text("[system] Ignored leave request before joining");
                    read_loop();
                    return;
                }

                std::string leaving_nick = nickname_;
                nickname_.clear();
                remove_user(leaving_nick);
                server_.broadcast("[system] " + leaving_nick + " left");
                server_.broadcast_users();
            }
            // ---- MSG ----
            else if (const auto* chat = std::get_if<chatbox::protocol::Chat>(&message))
            {
                if (nickname_.empty())
                {
                    deliver_text("[system] Join before sending messages");
                    read_loop();
                    return;
                }

                if (!consume_message_quota())
                {
                    deliver_text("[system] Slow down - message rate limit is "
                        + std::to_string(RATE_LIMIT_MESSAGES)
                        + " per minute");
                    read_loop();
                    return;
                }

                if (!is_valid_chat_message(chat->body))
                {
                    deliver_text("[system] Invalid message text");
                    read_loop();
                    return;
                }

                server_.broadcast("[" + timestamp() + "] " + nickname_ + ": " + chat->body);
            }
            // ---- WHISPER ----
            else if (const auto* whisper =
                std::get_if<chatbox::protocol::Whisper>(&message))
            {
                if (nickname_.empty())
                {
                    deliver_text("[system] Join before sending messages");
                    read_loop();
                    return;
                }

                if (!consume_message_quota())
                {
                    deliver_text("[system] Slow down - message rate limit is "
                        + std::to_string(RATE_LIMIT_MESSAGES)
                        + " per minute");
                    read_loop();
                    return;
                }

                if (!is_valid_nickname(whisper->target) ||
                    !is_valid_chat_message(whisper->body))
                {
                    deliver_text("[system] Invalid whisper target or message");
                    read_loop();
                    return;
                }

                server_.send_private(nickname_, whisper->target, whisper->body);
            }
            else
            {
                deliver_text("[system] Ignored message in the current session state");
            }

            read_loop();
        });
}

bool ClientSession::consume_message_quota()
{
    const auto now = std::chrono::steady_clock::now();
    const auto window = std::chrono::seconds(RATE_LIMIT_WINDOW_SECONDS);

    while (!recent_messages_.empty() && now - recent_messages_.front() > window)
        recent_messages_.pop_front();

    if (recent_messages_.size() >= RATE_LIMIT_MESSAGES)
        return false;

    recent_messages_.push_back(now);
    return true;
}

void ClientSession::deliver(const chatbox::protocol::ServerMessage& msg)
{
    auto encoded = chatbox::protocol::encode_server_frame(msg);
    if (!encoded)
    {
        std::cerr << "Protocol encode error: " << encoded.detail << "\n";
        return;
    }

    auto self = shared_from_this();
    boost::asio::post(
        socket_.get_executor(),
        [this, self, wire = std::move(*encoded.value)]
        {
            const bool writing = !write_queue_.empty();
            write_queue_.push_back(wire);
            if (!writing)
                write_next();
        });
}

void ClientSession::deliver_text(const std::string& text)
{
    deliver(chatbox::protocol::ServerMessage{ chatbox::protocol::Text{ text } });
}

void ClientSession::write_next()
{
    auto self = shared_from_this();
    boost::asio::async_write(
        socket_, boost::asio::buffer(write_queue_.front()),
        [this, self](boost::system::error_code ec, std::size_t)
        {
            if (ec)
            {
                if (ec != boost::asio::error::operation_aborted)
                    std::cerr << "Write error: " << ec.message() << "\n";
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
                boost::system::error_code ignored;
                socket_.shutdown(tcp::socket::shutdown_both, ignored);
                socket_.close(ignored);
            }
        });
}

void ClientSession::close()
{
    auto self = shared_from_this();
    boost::asio::post(
        socket_.get_executor(),
        [this, self]
        {
            suppress_leave_notice_ = true;
            if (!write_queue_.empty())
            {
                close_after_write_ = true;
                return;
            }

            boost::system::error_code ignored;
            socket_.shutdown(tcp::socket::shutdown_both, ignored);
            socket_.close(ignored);
        });
}
