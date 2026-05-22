/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: CC BY-NC-SA 4.0 (https://creativecommons.org/licenses/by-nc-sa/4.0/)
*/
#pragma once

#include "common.h"
#include "utils.h"
struct BrowserEntry
{
    std::string name;
    std::string host;
    uint16_t port = 0;
    bool has_password = false;
    int users = 0;
    std::chrono::system_clock::time_point updated_at;
};
std::string sanitize_browser_field(const std::string& text, size_t max_len);
std::string browser_entry_key(const std::string& host, uint16_t port, const std::string& name);
bool parse_u16_field(const std::string& text, uint16_t& out);
class ServerBrowser
{
public:
    ServerBrowser(boost::asio::io_context& io, uint16_t port)
        : io_(io), acceptor_(io)
    {
        tcp::endpoint endpoint(tcp::v6(), port);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(boost::asio::ip::v6_only(false));
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();
        accept_loop();
    }

    void stop()
    {
        boost::asio::post(io_,
            [this]
            {
                boost::system::error_code ignored;
                acceptor_.close(ignored);
            });
    }

    std::vector<BrowserEntry> entries() const
    {
        std::lock_guard lock(entries_mutex_);
        auto now = std::chrono::system_clock::now();
        std::vector<BrowserEntry> out;
        for (const auto& [_, entry] : entries_)
        {
            if (now - entry.updated_at <= std::chrono::seconds(BROWSER_ENTRY_TTL_SECONDS))
                out.push_back(entry);
        }
        return out;
    }

    std::vector<std::string> process_request(const std::string& request)
    {
        auto parts = split(request, '|');
        if (parts.empty())
            return { "ERROR|Empty request" };

        if (parts[0] == "REGISTER")
        {
            if (parts.size() < 6)
                return { "ERROR|REGISTER requires name, host, port, password flag, users" };

            BrowserEntry entry;
            entry.name = sanitize_browser_field(parts[1], BROWSER_NAME_MAX_LEN);
            entry.host = sanitize_browser_field(parts[2], HOST_MAX_LEN);

            if (entry.name.empty())
                entry.name = "chatbox room";
            if (entry.host.empty() || !parse_u16_field(parts[3], entry.port))
                return { "ERROR|Invalid host or port" };

            entry.has_password = parts[4] == "1";
            try
            {
                entry.users = std::max(0, std::stoi(parts[5]));
            }
            catch (...)
            {
                entry.users = 0;
            }
            entry.updated_at = std::chrono::system_clock::now();

            {
                std::lock_guard lock(entries_mutex_);
                remove_stale_locked();
                entries_[browser_entry_key(entry.host, entry.port, entry.name)] = entry;
            }

            return { "OK" };
        }

        if (parts[0] == "UNREGISTER")
        {
            if (parts.size() < 4)
                return { "ERROR|UNREGISTER requires host, port, name" };

            uint16_t port = 0;
            if (!parse_u16_field(parts[2], port))
                return { "ERROR|Invalid port" };

            std::string host = sanitize_browser_field(parts[1], HOST_MAX_LEN);
            std::string name = sanitize_browser_field(parts[3], BROWSER_NAME_MAX_LEN);

            std::lock_guard lock(entries_mutex_);
            entries_.erase(browser_entry_key(host, port, name));
            return { "OK" };
        }

        if (parts[0] == "LIST")
        {
            std::vector<std::string> response;
            auto now = std::chrono::system_clock::now();
            {
                std::lock_guard lock(entries_mutex_);
                remove_stale_locked();
                for (const auto& [_, entry] : entries_)
                {
                    auto age = std::chrono::duration_cast<std::chrono::seconds>(
                        now - entry.updated_at).count();
                    response.push_back("SERVER|" + entry.name
                        + "|" + entry.host
                        + "|" + std::to_string(entry.port)
                        + "|" + (entry.has_password ? "1" : "0")
                        + "|" + std::to_string(entry.users)
                        + "|" + std::to_string(age));
                }
            }
            response.push_back("END");
            return response;
        }

        return { "ERROR|Unknown request" };
    }

private:
    class Session : public std::enable_shared_from_this<Session>
    {
    public:
        Session(tcp::socket socket, ServerBrowser& browser)
            : socket_(std::move(socket)), browser_(browser), buffer_(MAX_WIRE_LINE_LENGTH)
        {}

        void start()
        {
            auto self = shared_from_this();
            boost::asio::async_read_until(socket_, buffer_, '\n',
                [this, self](boost::system::error_code ec, std::size_t)
                {
                    if (ec)
                        return;

                    std::istream is(&buffer_);
                    std::string line;
                    std::getline(is, line);
                    strip_wire_newline(line);

                    std::string decoded;
                    std::vector<std::string> responses;
                    if (!try_decode_base64(line, decoded))
                        responses = { "ERROR|Invalid framing" };
                    else
                        responses = browser_.process_request(decoded);

                    write_payload_.clear();
                    for (const auto& response : responses)
                        write_payload_ += encode_base64(response) + "\n";

                    boost::asio::async_write(socket_, boost::asio::buffer(write_payload_),
                        [this, self](boost::system::error_code, std::size_t)
                        {
                            boost::system::error_code ignored;
                            socket_.shutdown(tcp::socket::shutdown_both, ignored);
                            socket_.close(ignored);
                        });
                });
        }

    private:
        tcp::socket socket_;
        ServerBrowser& browser_;
        boost::asio::streambuf buffer_;
        std::string write_payload_;
    };

    void accept_loop()
    {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket)
            {
                if (!ec)
                    std::make_shared<Session>(std::move(socket), *this)->start();

                if (acceptor_.is_open())
                    accept_loop();
            });
    }

    void remove_stale_locked() const
    {
        auto now = std::chrono::system_clock::now();
        for (auto it = entries_.begin(); it != entries_.end();)
        {
            if (now - it->second.updated_at > std::chrono::seconds(BROWSER_ENTRY_TTL_SECONDS))
                it = entries_.erase(it);
            else
                ++it;
        }
    }

    boost::asio::io_context& io_;
    tcp::acceptor acceptor_;
    mutable std::mutex entries_mutex_;
    mutable std::map<std::string, BrowserEntry> entries_;
};

class ServerBrowserClient
{
public:
    static bool register_server(
        const std::string& browser_host,
        uint16_t browser_port,
        const BrowserEntry& entry,
        std::string& error)
    {
        std::string request = "REGISTER|" + sanitize_browser_field(entry.name, BROWSER_NAME_MAX_LEN)
            + "|" + sanitize_browser_field(entry.host, HOST_MAX_LEN)
            + "|" + std::to_string(entry.port)
            + "|" + (entry.has_password ? "1" : "0")
            + "|" + std::to_string(std::max(0, entry.users));

        std::vector<std::string> response;
        if (!send_request(browser_host, browser_port, request, response, error))
            return false;

        if (!response.empty() && response[0] == "OK")
            return true;

        error = response.empty() ? "No browser response" : response[0];
        return false;
    }

    static bool unregister_server(
        const std::string& browser_host,
        uint16_t browser_port,
        const BrowserEntry& entry)
    {
        std::string ignored;
        std::vector<std::string> response;
        std::string request = "UNREGISTER|" + sanitize_browser_field(entry.host, HOST_MAX_LEN)
            + "|" + std::to_string(entry.port)
            + "|" + sanitize_browser_field(entry.name, BROWSER_NAME_MAX_LEN);
        return send_request(browser_host, browser_port, request, response, ignored);
    }

    static bool list_servers(
        const std::string& browser_host,
        uint16_t browser_port,
        std::vector<BrowserEntry>& entries,
        std::string& error)
    {
        std::vector<std::string> response;
        if (!send_request(browser_host, browser_port, "LIST", response, error))
            return false;

        entries.clear();
        for (const auto& line : response)
        {
            if (line == "END")
                return true;

            auto parts = split(line, '|');
            if (parts.size() < 7 || parts[0] != "SERVER")
                continue;

            BrowserEntry entry;
            entry.name = parts[1];
            entry.host = parts[2];
            if (!parse_u16_field(parts[3], entry.port))
                continue;
            entry.has_password = parts[4] == "1";
            try { entry.users = std::max(0, std::stoi(parts[5])); }
            catch (...) { entry.users = 0; }
            try
            {
                int age = std::max(0, std::stoi(parts[6]));
                entry.updated_at = std::chrono::system_clock::now() - std::chrono::seconds(age);
            }
            catch (...)
            {
                entry.updated_at = std::chrono::system_clock::now();
            }
            entries.push_back(entry);
        }

        error = "Browser response did not include END";
        return false;
    }

private:
    static bool send_request(
        const std::string& browser_host,
        uint16_t browser_port,
        const std::string& request,
        std::vector<std::string>& response,
        std::string& error)
    {
        try
        {
            boost::asio::io_context io;
            tcp::resolver resolver(io);
            tcp::socket socket(io);
            boost::system::error_code ec;
            auto endpoints = resolver.resolve(browser_host, std::to_string(browser_port), ec);
            if (ec)
            {
                error = browser_connection_error(browser_host, browser_port,
                    "Could not resolve browser server: " + ec.message());
                return false;
            }

            if (!connect_with_timeout(io, socket, endpoints, browser_host, browser_port, error))
                return false;

            std::string payload = encode_base64(request) + "\n";
            boost::asio::write(socket, boost::asio::buffer(payload), ec);
            if (ec)
            {
                error = browser_connection_error(browser_host, browser_port,
                    "Could not send request: " + ec.message());
                return false;
            }

            boost::asio::streambuf buffer(MAX_WIRE_LINE_LENGTH);
            while (true)
            {
                boost::asio::read_until(socket, buffer, '\n', ec);
                if (ec)
                    break;

                std::istream is(&buffer);
                std::string line;
                std::getline(is, line);
                strip_wire_newline(line);

                std::string decoded;
                if (try_decode_base64(line, decoded))
                    response.push_back(decoded);
            }

            if (response.empty())
            {
                error = "No response from browser server";
                return false;
            }
            return true;
        }
        catch (const boost::system::system_error& ex)
        {
            error = browser_connection_error(browser_host, browser_port, ex.code().message());
            return false;
        }
        catch (const std::exception& ex)
        {
            error = browser_connection_error(browser_host, browser_port, ex.what());
            return false;
        }
        catch (...)
        {
            error = "Unknown browser connection error";
            return false;
        }
    }

    static std::string browser_connection_error(
        const std::string& browser_host,
        uint16_t browser_port,
        const std::string& detail)
    {
        return detail + " (" + browser_host + ":" + std::to_string(browser_port)
            + "). Make sure a chatbox browser server is running there and that port "
            + std::to_string(browser_port) + " is open or forwarded.";
    }

    static bool connect_with_timeout(
        boost::asio::io_context& io,
        tcp::socket& socket,
        const tcp::resolver::results_type& endpoints,
        const std::string& browser_host,
        uint16_t browser_port,
        std::string& error)
    {
        bool finished = false;
        boost::system::error_code connect_ec;

        boost::asio::async_connect(socket, endpoints,
            [&](const boost::system::error_code& ec, const tcp::endpoint&)
            {
                connect_ec = ec;
                finished = true;
            });

        io.restart();
        io.run_for(std::chrono::seconds(BROWSER_CONNECT_TIMEOUT_SECONDS));

        if (!finished)
        {
            boost::system::error_code ignored;
            socket.close(ignored);
            error = browser_connection_error(browser_host, browser_port,
                "Timed out connecting to browser server");
            return false;
        }

        if (connect_ec)
        {
            error = browser_connection_error(browser_host, browser_port, connect_ec.message());
            return false;
        }

        return true;
    }
};

class ServerBrowserPublisher
{
public:
    ServerBrowserPublisher(
        std::string browser_host,
        uint16_t browser_port,
        BrowserEntry entry,
        std::function<int()> users_provider)
        : browser_host_(std::move(browser_host)),
        browser_port_(browser_port),
        entry_(std::move(entry)),
        users_provider_(std::move(users_provider))
    {}

    ~ServerBrowserPublisher()
    {
        stop();
    }

    bool start(std::string& error)
    {
        if (started_)
            return true;

        entry_.users = current_users();
        if (!ServerBrowserClient::register_server(browser_host_, browser_port_, entry_, error))
            return false;

        started_ = true;
        stop_requested_ = false;
        worker_ = std::thread([this] { publish_loop(); });
        return true;
    }

    void stop()
    {
        if (!started_)
            return;

        stop_requested_ = true;
        if (worker_.joinable())
            worker_.join();

        ServerBrowserClient::unregister_server(browser_host_, browser_port_, entry_);
        started_ = false;
    }

private:
    int current_users() const
    {
        return users_provider_ ? users_provider_() : 0;
    }

    void publish_loop()
    {
        while (!stop_requested_)
        {
            for (int i = 0; i < BROWSER_PUBLISH_INTERVAL_SECONDS && !stop_requested_; ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));

            if (stop_requested_)
                break;

            BrowserEntry entry = entry_;
            entry.users = current_users();
            std::string ignored;
            ServerBrowserClient::register_server(browser_host_, browser_port_, entry, ignored);
        }
    }

    std::string browser_host_;
    uint16_t browser_port_ = BROWSER_PORT;
    BrowserEntry entry_;
    std::function<int()> users_provider_;
    std::atomic<bool> stop_requested_{ false };
    bool started_ = false;
    std::thread worker_;
};
