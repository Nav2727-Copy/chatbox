/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: CC BY-NC-SA 4.0 (https://creativecommons.org/licenses/by-nc-sa/4.0/)
*/
#pragma once

#include "common.h"
#include "app_state.h"
#include "utils.h"
class ChatClient
{
public:
    ChatClient(boost::asio::io_context& io)
        : socket_(io), buffer_(MAX_WIRE_LINE_LENGTH)
    {}

    // connect() returns false on failure.
    // on_auth_result is called with true/false when AUTH_OK / AUTH_FAIL arrives.
    bool connect(const std::string& host, uint16_t port)
    {
        try
        {
            tcp::resolver resolver(socket_.get_executor());
            auto endpoints = resolver.resolve(host, std::to_string(port));
            boost::asio::connect(socket_, endpoints);
            read_loop();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void send(const std::string& msg)
    {
        boost::asio::post(
            socket_.get_executor(),
            [this, msg]
            {
                const bool writing = !write_queue_.empty();
                write_queue_.push_back(encode_base64(msg) + "\n");
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

        send("IDENTIFY|" + nick + "|" + identity_->public_key_hex);
    }

    void close()
    {
        closing_ = true;
        boost::asio::post(
            socket_.get_executor(),
            [this]
            {
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

private:
    void write_next()
    {
        boost::asio::async_write(
            socket_, boost::asio::buffer(write_queue_.front()),
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
                    boost::system::error_code ignored;
                    socket_.shutdown(tcp::socket::shutdown_both, ignored);
                    socket_.close(ignored);
                }
            });
    }

    void read_loop()
    {
        boost::asio::async_read_until(
            socket_, buffer_, '\n',
            [this](boost::system::error_code ec, std::size_t)
            {
                if (ec)
                {
                    if (!closing_ && !kicked_ && !banned_ && auth_ok_)
                        push_message("[system] disconnected");
                    return;
                }

                std::istream is(&buffer_);
                std::string  line;
                std::getline(is, line);
                strip_wire_newline(line);

                std::string decoded;
                if (!try_decode_base64(line, decoded))
                {
                    push_message("[system] Ignored invalid server message");
                    read_loop();
                    return;
                }

                // Server signals
                if (decoded == "AUTH_REQUIRED")
                {
                    auth_required_ = true;
                }
                else if (decoded == "AUTH_OK")
                {
                    auth_resolved_ = true;
                    auth_ok_ = true;
                }
                else if (decoded == "AUTH_FAIL")
                {
                    auth_resolved_ = true;
                    auth_ok_ = false;
                    push_message("[system] Wrong password - disconnected");
                }
                else if (decoded == "JOIN_OK")
                {
                    join_resolved_ = true;
                }
                else if (decoded.rfind("CHALLENGE|", 0) == 0)
                {
                    auto parts = split(decoded, '|');
                    if (parts.size() >= 2 && identity_)
                    {
                        send("PROOF|" + sign_identity_challenge(*identity_, parts[1]));
                    }
                    else
                    {
                        join_resolved_ = true;
                        join_failed_ = true;
                        join_failure_reason_ = "Identity challenge could not be answered";
                    }
                }
                else if (decoded.rfind("IDENTITY_OK|", 0) == 0)
                {
                    auto parts = split(decoded, '|');
                    if (parts.size() >= 2)
                        push_message("[system] Identity verified: " + parts[1]);
                }
                else if (decoded == "BANNED")
                {
                    join_resolved_ = true;
                    banned_ = true;
                    push_message("[system] You are banned from this server");
                }
                else if (decoded == "NICK_TAKEN")
                {
                    join_resolved_ = true;
                    nickname_taken_ = true;
                    push_message("[system] Nickname is already in use");
                }
                else if (decoded.rfind("JOIN_FAIL|", 0) == 0)
                {
                    join_resolved_ = true;
                    join_failed_ = true;
                    join_failure_reason_ = decoded.substr(10);
                    push_message("[system] Join failed: " + join_failure_reason_);
                }
                else if (decoded.rfind("KICK|", 0) == 0)
                {
                    kicked_ = true;
                    auto parts = split(decoded, '|');
                    if (parts.size() >= 3)
                        kick_reason_ = join_fields(parts, 2, '|');
                    push_message("[system] You have been kicked"
                        + (kick_reason_.empty() ? "" : ": " + kick_reason_));
                    g_kicked = true;   // signal main loop
                }
                else if (decoded.rfind("USERS|", 0) == 0)
                {
                    std::string list = decoded.substr(6);
                    std::lock_guard lock(g_mutex);
                    g_users.clear();
                    if (!list.empty())
                        for (auto& name : split(list, ','))
                            if (is_valid_nickname(name))
                                g_users.push_back(name);
                }
                else
                {
                    push_message(decoded);
                }

                read_loop();
            });
    }

    tcp::socket            socket_;
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
    std::optional<ClientIdentity> identity_;
    std::string       kick_reason_;
    std::string       join_failure_reason_;
    bool              close_after_write_ = false;
};
