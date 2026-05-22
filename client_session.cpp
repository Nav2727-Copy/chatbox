#include "chat_server.h"

void ClientSession::start()
{
    if (server_.has_password())
        deliver("AUTH_REQUIRED");
    else
        deliver("AUTH_OK");

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
            strip_wire_newline(line);

            std::string decoded;
            if (!try_decode_base64(line, decoded))
            {
                deliver("[system] Ignored invalid message frame");
                read_loop();
                return;
            }

            auto parts = split(decoded, '|');

            if (parts.empty())
            {
                read_loop();
                return;
            }

            // ---- AUTH ----
            if (parts[0] == "AUTH" && parts.size() >= 2)
            {
                if (server_.check_password(parts[1]))
                {
                    authenticated_ = true;
                    deliver("AUTH_OK");
                }
                else
                {
                    deliver("AUTH_FAIL");
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
                deliver("AUTH_REQUIRED");
                read_loop();
                return;
            }

            // ---- IDENTIFY ----
            if (parts[0] == "IDENTIFY" && parts.size() >= 3)
            {
                std::string nick = parts[1];
                std::string public_key_hex = parts[2];

                if (!nickname_.empty())
                {
                    deliver("JOIN_FAIL|Already joined");
                    read_loop();
                    return;
                }

                std::string identity_error;
                if (!server_.identity_available_for(nick, public_key_hex, identity_error))
                {
                    deliver("JOIN_FAIL|" + identity_error);
                    close();
                    server_.leave(self);
                    return;
                }

                if (server_.is_banned(nick))
                {
                    deliver("BANNED");
                    close();
                    server_.leave(self);
                    return;
                }

                if (server_.nickname_taken(nick))
                {
                    deliver("NICK_TAKEN");
                    close();
                    server_.leave(self);
                    return;
                }

                pending_nickname_ = nick;
                pending_public_key_hex_ = public_key_hex;
                pending_challenge_ = random_hex(IDENTITY_CHALLENGE_BYTES);
                deliver("CHALLENGE|" + pending_challenge_);
            }
            // ---- PROOF ----
            else if (parts[0] == "PROOF" && parts.size() >= 2)
            {
                if (!nickname_.empty())
                {
                    deliver("JOIN_FAIL|Already joined");
                    read_loop();
                    return;
                }

                if (pending_nickname_.empty() || pending_public_key_hex_.empty() ||
                    pending_challenge_.empty())
                {
                    deliver("JOIN_FAIL|Identify before sending proof");
                    close();
                    server_.leave(self);
                    return;
                }

                if (!verify_identity_signature(
                    pending_public_key_hex_,
                    pending_challenge_,
                    parts[1]))
                {
                    deliver("JOIN_FAIL|Identity proof failed");
                    close();
                    server_.leave(self);
                    return;
                }

                if (server_.is_banned(pending_nickname_))
                {
                    deliver("BANNED");
                    close();
                    server_.leave(self);
                    return;
                }

                if (server_.nickname_taken(pending_nickname_))
                {
                    deliver("NICK_TAKEN");
                    close();
                    server_.leave(self);
                    return;
                }

                if (!server_.bind_identity(pending_nickname_, pending_public_key_hex_))
                {
                    deliver("JOIN_FAIL|Identity registration failed");
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
                deliver("IDENTITY_OK|" + fingerprint);
                deliver("JOIN_OK");
                server_.deliver_history(self);
                server_.broadcast("[system] " + nickname_ + " joined");
                server_.broadcast_users();
            }
            // ---- LEAVE ----
            else if (parts[0] == "LEAVE")
            {
                if (nickname_.empty() ||
                    (parts.size() >= 2 && parts[1] != nickname_))
                {
                    deliver("[system] Ignored malformed leave request");
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
            else if (parts[0] == "MSG" && parts.size() >= 2)
            {
                if (nickname_.empty())
                {
                    deliver("[system] Join before sending messages");
                    read_loop();
                    return;
                }

                if (!consume_message_quota())
                {
                    deliver("[system] Slow down - message rate limit is "
                        + std::to_string(RATE_LIMIT_MESSAGES)
                        + " per minute");
                    read_loop();
                    return;
                }

                const size_t message_start =
                    parts.size() >= 3 && parts[1] == nickname_ ? 2 : 1;
                std::string message = join_fields(parts, message_start, '|');
                if (!is_valid_chat_message(message))
                {
                    deliver("[system] Invalid message text");
                    read_loop();
                    return;
                }

                server_.broadcast("[" + timestamp() + "] " + nickname_ + ": " + message);
            }
            // ---- WHISPER ----
            else if (parts[0] == "WHISPER" && parts.size() >= 3)
            {
                if (nickname_.empty())
                {
                    deliver("[system] Join before sending messages");
                    read_loop();
                    return;
                }

                if (!consume_message_quota())
                {
                    deliver("[system] Slow down - message rate limit is "
                        + std::to_string(RATE_LIMIT_MESSAGES)
                        + " per minute");
                    read_loop();
                    return;
                }

                const bool old_format = parts.size() >= 4 && parts[1] == nickname_;
                const std::string& target = old_format ? parts[2] : parts[1];
                std::string message = join_fields(parts, old_format ? 3 : 2, '|');
                if (!is_valid_nickname(target) || !is_valid_chat_message(message))
                {
                    deliver("[system] Invalid whisper target or message");
                    read_loop();
                    return;
                }

                server_.send_private(nickname_, target, message);
            }
            else
            {
                deliver("[system] Ignored malformed message");
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

void ClientSession::deliver(const std::string& msg)
{
    auto self = shared_from_this();
    boost::asio::post(
        socket_.get_executor(),
        [this, self, msg]
        {
            const bool writing = !write_queue_.empty();
            write_queue_.push_back(encode_base64(msg) + "\n");
            if (!writing)
                write_next();
        });
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
