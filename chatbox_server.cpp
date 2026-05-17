/*
chatbox_server - headless dedicated server for chatbox
written by "Nav2727"
license: CC BY-NC-SA 4.0 (https://creativecommons.org/licenses/by-nc-sa/4.0/)

Usage:
  chatbox_server <port> [password] [logfile]

  port      - TCP port to listen on
  password  - optional room password (omit or leave blank for open room)
  logfile   - optional path for the chat log  (default: chatlog.txt)

Admin console commands (type in the terminal while the server is running):
  /kick <nick> [reason]   kick a connected user
  /ban  <nick> [reason]   ban a user (kicks if connected, blocks future joins)
  /unban <nick>           remove a ban
  /bans                   list all banned nicknames
  /users                  list connected users
  /broadcast <msg>        send a server announcement to all users
  /quit                   shut down the server
  /help                   show this help
*/

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using boost::asio::ip::tcp;

// =====================================================
// Base64
// =====================================================

constexpr const char* BASE64_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string encode_base64(const std::string& input)
{
    std::string output;
    int i = 0;
    while (i < static_cast<int>(input.size()))
    {
        uint32_t b = (static_cast<unsigned char>(input[i++]) & 0xFF) << 16;
        if (i < static_cast<int>(input.size()))
            b |= (static_cast<unsigned char>(input[i++]) & 0xFF) << 8;
        if (i < static_cast<int>(input.size()))
            b |= (static_cast<unsigned char>(input[i++]) & 0xFF);

        output += BASE64_CHARS[(b >> 18) & 0x3F];
        output += BASE64_CHARS[(b >> 12) & 0x3F];
        output += (i - 2 < static_cast<int>(input.size())) ? BASE64_CHARS[(b >> 6) & 0x3F] : '=';
        output += (i - 1 < static_cast<int>(input.size())) ? BASE64_CHARS[b & 0x3F]         : '=';
    }
    return output;
}

std::string decode_base64(const std::string& input)
{
    std::string output;
    if (input.size() % 4 != 0) return "";

    auto char_to_val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    for (size_t i = 0; i < input.size(); i += 4)
    {
        int v0 = char_to_val(input[i]);
        int v1 = char_to_val(input[i + 1]);
        int v2 = char_to_val(input[i + 2]);
        int v3 = char_to_val(input[i + 3]);

        if (v0 < 0 || v1 < 0) return "";

        output += static_cast<char>((v0 << 2) | (v1 >> 4));
        if (input[i + 2] != '=')
        {
            output += static_cast<char>((v1 << 4) | (v2 >> 2));
            if (input[i + 3] != '=')
                output += static_cast<char>((v2 << 6) | v3);
        }
    }
    return output;
}

// =====================================================
// Utilities
// =====================================================

std::string timestamp()
{
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

std::vector<std::string> split(const std::string& s, char delim)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim))
        out.push_back(item);
    return out;
}

// =====================================================
// Chat log
// =====================================================

class ChatLog
{
public:
    explicit ChatLog(const std::string& path) : path_(path)
    {
        file_.open(path, std::ios::app);
        if (file_.is_open())
            write("=== Server started: " + timestamp() + " ===");
        else
            std::cerr << "[warn] Could not open log file: " << path << "\n";
    }

    ~ChatLog()
    {
        if (file_.is_open())
        {
            write("=== Server stopped: " + timestamp() + " ===");
            file_.close();
        }
    }

    void write(const std::string& line)
    {
        std::lock_guard lock(mutex_);
        if (file_.is_open())
        {
            file_ << "[" << timestamp() << "] " << line << "\n";
            file_.flush();
        }
        // Also echo to console
        std::cout << "[log] " << line << "\n";
    }

private:
    std::string   path_;
    std::ofstream file_;
    std::mutex    mutex_;
};

// =====================================================
// Globals
// =====================================================

std::mutex              g_users_mutex;
std::vector<std::string> g_users;

void add_user(const std::string& user)
{
    std::lock_guard lock(g_users_mutex);
    if (std::find(g_users.begin(), g_users.end(), user) == g_users.end())
        g_users.push_back(user);
}

void remove_user(const std::string& user)
{
    std::lock_guard lock(g_users_mutex);
    g_users.erase(std::remove(g_users.begin(), g_users.end(), user), g_users.end());
}

// =====================================================
// Forward declarations
// =====================================================

class DedicatedServer;

class ClientSession
    : public std::enable_shared_from_this<ClientSession>
{
public:
    ClientSession(tcp::socket socket, DedicatedServer& server)
        : socket_(std::move(socket)), server_(server)
    {}

    void start();
    void deliver(const std::string& msg);

    const std::string& nickname() const { return nickname_; }

private:
    void read_loop();

    tcp::socket            socket_;
    DedicatedServer&       server_;
    boost::asio::streambuf buffer_;
    std::string            nickname_;
    bool                   authenticated_ = false;
};

// =====================================================
// Dedicated Server
// =====================================================

class DedicatedServer
{
public:
    DedicatedServer(boost::asio::io_context& io,
                    uint16_t                 port,
                    const std::string&       password,
                    ChatLog&                 log)
        : acceptor_(io), password_(password), log_(log)
    {
        tcp::endpoint ep(tcp::v6(), port);
        acceptor_.open(ep.protocol());
        acceptor_.set_option(boost::asio::ip::v6_only(false));
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        acceptor_.bind(ep);
        acceptor_.listen();
        accept_loop();
    }

    bool has_password()                      const { return !password_.empty(); }
    bool check_password(const std::string& pw) const { return pw == password_; }

    bool is_banned(const std::string& nick) const
    {
        std::lock_guard lock(ban_mutex_);
        return banned_.count(nick) > 0;
    }

    void join(std::shared_ptr<ClientSession> s)
    {
        std::lock_guard lock(sessions_mutex_);
        sessions_.insert(s);
    }

    void leave(std::shared_ptr<ClientSession> s)
    {
        std::lock_guard lock(sessions_mutex_);
        sessions_.erase(s);
    }

    void broadcast(const std::string& msg)
    {
        log_.write(msg);
        std::lock_guard lock(sessions_mutex_);
        for (auto& s : sessions_)
            s->deliver(msg);
    }

    void broadcast_users()
    {
        std::string payload = "USERS|";
        {
            std::lock_guard lock(g_users_mutex);
            bool first = true;
            for (const auto& u : g_users)
            {
                if (!first) payload += ',';
                payload += u;
                first = false;
            }
        }
        std::lock_guard lock(sessions_mutex_);
        for (auto& s : sessions_)
            s->deliver(payload);
    }

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
        if (!target) return false;

        std::string kick_msg = "KICK|" + nick;
        if (!reason.empty()) kick_msg += "|" + reason;
        target->deliver(kick_msg);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        leave(target);
        remove_user(nick);
        broadcast("[system] " + nick + " was kicked"
                  + (reason.empty() ? "" : ": " + reason));
        broadcast_users();
        return true;
    }

    bool ban_user(const std::string& nick, const std::string& reason = "")
    {
        {
            std::lock_guard lock(ban_mutex_);
            banned_.insert(nick);
        }
        save_bans();
        kick_user(nick, reason.empty() ? "banned" : reason);
        broadcast("[system] " + nick + " has been banned");
        log_.write("[admin] Banned: " + nick + (reason.empty() ? "" : " (" + reason + ")"));
        return true;
    }

    bool unban_user(const std::string& nick)
    {
        bool removed;
        {
            std::lock_guard lock(ban_mutex_);
            removed = banned_.erase(nick) > 0;
        }
        if (removed)
        {
            save_bans();
            log_.write("[admin] Unbanned: " + nick);
        }
        return removed;
    }

    // Load bans from a file so they survive restarts
    void load_bans(const std::string& path)
    {
        bans_file_ = path;
        std::ifstream f(path);
        if (!f.is_open()) return;

        std::string line;
        std::lock_guard lock(ban_mutex_);
        while (std::getline(f, line))
        {
            if (!line.empty())
                banned_.insert(line);
        }
        log_.write("[admin] Loaded " + std::to_string(banned_.size()) + " ban(s) from " + path);
    }

    std::set<std::string> ban_list() const
    {
        std::lock_guard lock(ban_mutex_);
        return banned_;
    }

    std::vector<std::string> connected_users() const
    {
        std::lock_guard lock(g_users_mutex);
        return g_users;
    }

private:
    void accept_loop()
    {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket)
            {
                if (!ec)
                {
                    auto s = std::make_shared<ClientSession>(
                        std::move(socket), *this);
                    join(s);
                    s->start();
                }
                accept_loop();
            });
    }

    void save_bans()
    {
        if (bans_file_.empty()) return;
        std::ofstream f(bans_file_, std::ios::trunc);
        std::lock_guard lock(ban_mutex_);
        for (const auto& b : banned_)
            f << b << "\n";
    }

    tcp::acceptor acceptor_;
    std::string   password_;
    ChatLog&      log_;
    std::string   bans_file_;

    mutable std::mutex sessions_mutex_;
    std::set<std::shared_ptr<ClientSession>> sessions_;

    mutable std::mutex    ban_mutex_;
    std::set<std::string> banned_;
};

// =====================================================
// ClientSession implementation
// =====================================================

void ClientSession::start()
{
    if (server_.has_password())
        deliver("AUTH_REQUIRED");
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
                if (!nickname_.empty())
                {
                    remove_user(nickname_);
                    server_.broadcast("[system] " + nickname_ + " disconnected");
                    server_.broadcast_users();
                }
                return;
            }

            std::istream is(&buffer_);
            std::string  line;
            std::getline(is, line);

            std::string decoded = decode_base64(line);
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
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    server_.leave(self);
                }
                read_loop();
                return;
            }

            if (server_.has_password() && !authenticated_)
            {
                deliver("AUTH_REQUIRED");
                read_loop();
                return;
            }

            // ---- JOIN ----
            if (parts[0] == "JOIN" && parts.size() >= 2)
            {
                if (server_.is_banned(parts[1]))
                {
                    deliver("BANNED");
                    server_.leave(self);
                    return;
                }
                nickname_ = parts[1];
                add_user(nickname_);
                server_.broadcast("[system] " + nickname_ + " joined");
                server_.broadcast_users();
            }
            // ---- LEAVE ----
            else if (parts[0] == "LEAVE" && parts.size() >= 2)
            {
                remove_user(parts[1]);
                server_.broadcast("[system] " + parts[1] + " left");
                server_.broadcast_users();
            }
            // ---- MSG ----
            else if (parts[0] == "MSG" && parts.size() >= 3)
            {
                server_.broadcast("[" + timestamp() + "] " + parts[1] + ": " + parts[2]);
            }
            // ---- WHISPER ----
            else if (parts[0] == "WHISPER" && parts.size() >= 4)
            {
                server_.broadcast(
                    "[PRIVATE " + timestamp() + "] "
                    + parts[1] + " -> " + parts[2] + ": " + parts[3]);
            }

            read_loop();
        });
}

void ClientSession::deliver(const std::string& msg)
{
    auto self = shared_from_this();
    auto data = std::make_shared<std::string>(encode_base64(msg) + "\n");

    boost::asio::async_write(
        socket_, boost::asio::buffer(*data),
        [data, self](boost::system::error_code ec, std::size_t)
        {
            if (ec)
                std::cerr << "Write error: " << ec.message() << "\n";
        });
}

// =====================================================
// UPnP (same as main client)
// =====================================================

class UPnPMapper
{
public:
    ~UPnPMapper()
    {
        if (!discovered_) return;
        for (auto& m : mappings_)
            UPNP_DeletePortMapping(
                urls_.controlURL, data_.first.servicetype,
                m.port.c_str(), m.proto.c_str(), nullptr);
        FreeUPNPUrls(&urls_);
    }

    bool discover()
    {
        int error = 0;
        UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
        if (!devlist) return false;

        char lanAddr[64] = {}, wanAddr[64] = {};
        int status = UPNP_GetValidIGD(devlist, &urls_, &data_,
                                       lanAddr, sizeof(lanAddr),
                                       wanAddr, sizeof(wanAddr));
        freeUPNPDevlist(devlist);
        if (status != 1) return false;

        localIP_    = lanAddr;
        discovered_ = true;

        char extIP[64] = {};
        if (UPNP_GetExternalIPAddress(urls_.controlURL,
                                       data_.first.servicetype,
                                       extIP) == UPNPCOMMAND_SUCCESS)
            externalIP_ = extIP;

        return true;
    }

    bool openPortBoth(const std::string& port, const std::string& desc = "P2P Chat")
    {
        auto open = [&](const std::string& proto) -> bool
        {
            UPNP_DeletePortMapping(urls_.controlURL, data_.first.servicetype,
                                    port.c_str(), proto.c_str(), nullptr);
            int r = UPNP_AddPortMapping(urls_.controlURL, data_.first.servicetype,
                                         port.c_str(), port.c_str(), localIP_.c_str(),
                                         desc.c_str(), proto.c_str(), nullptr, "0");
            if (r == UPNPCOMMAND_SUCCESS)
            {
                mappings_.push_back({ port, proto });
                return true;
            }
            return false;
        };
        return open("TCP") || open("UDP");
    }

    std::string externalIP() const { return externalIP_; }

private:
    struct Mapping { std::string port, proto; };
    UPNPUrls    urls_{};
    IGDdatas    data_{};
    std::string localIP_, externalIP_;
    bool        discovered_ = false;
    std::vector<Mapping> mappings_;
};

// =====================================================
// Admin console
// =====================================================

void show_admin_help()
{
    std::cout << "\nAdmin commands:\n"
              << "  /kick <nick> [reason]  - kick a user\n"
              << "  /ban  <nick> [reason]  - ban a user\n"
              << "  /unban <nick>          - unban a user\n"
              << "  /bans                  - list all bans\n"
              << "  /users                 - list connected users\n"
              << "  /broadcast <msg>       - send server announcement\n"
              << "  /quit                  - shut down the server\n"
              << "  /help                  - show this help\n\n";
}

// Runs on a dedicated thread, reads stdin for admin commands
void admin_console(DedicatedServer& server, ChatLog& log,
                   std::atomic<bool>& quit_flag)
{
    show_admin_help();

    std::string line;
    while (!quit_flag && std::getline(std::cin, line))
    {
        if (line.empty()) continue;

        auto parts = split(line, ' ');
        if (parts.empty()) continue;

        std::string cmd = parts[0];

        if (cmd == "/quit" || cmd == "/exit")
        {
            quit_flag = true;
            break;
        }
        else if (cmd == "/help")
        {
            show_admin_help();
        }
        else if (cmd == "/users")
        {
            auto users = server.connected_users();
            if (users.empty())
                std::cout << "(no users connected)\n";
            else
                for (const auto& u : users)
                    std::cout << "  - " << u << "\n";
        }
        else if (cmd == "/kick" && parts.size() >= 2)
        {
            std::string nick   = parts[1];
            std::string reason = parts.size() >= 3 ? parts[2] : "";
            if (server.kick_user(nick, reason))
                std::cout << "Kicked: " << nick << "\n";
            else
                std::cout << "User not found: " << nick << "\n";
        }
        else if (cmd == "/ban" && parts.size() >= 2)
        {
            std::string nick   = parts[1];
            std::string reason = parts.size() >= 3 ? parts[2] : "";
            server.ban_user(nick, reason);
            std::cout << "Banned: " << nick << "\n";
        }
        else if (cmd == "/unban" && parts.size() >= 2)
        {
            std::string nick = parts[1];
            if (server.unban_user(nick))
                std::cout << "Unbanned: " << nick << "\n";
            else
                std::cout << "Not in ban list: " << nick << "\n";
        }
        else if (cmd == "/bans")
        {
            auto bans = server.ban_list();
            if (bans.empty())
                std::cout << "(ban list is empty)\n";
            else
                for (const auto& b : bans)
                    std::cout << "  - " << b << "\n";
        }
        else if (cmd == "/broadcast" && parts.size() >= 2)
        {
            // Reconstruct message from remaining tokens
            std::string msg;
            for (size_t i = 1; i < parts.size(); ++i)
            {
                if (i > 1) msg += ' ';
                msg += parts[i];
            }
            server.broadcast("[SERVER] " + msg);
        }
        else
        {
            std::cout << "Unknown command. Type /help for a list.\n";
        }
    }
}

// =====================================================
// main
// =====================================================

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cout << "Usage: chatbox_server <port> [password] [logfile]\n";
        return 1;
    }

    uint16_t    port     = static_cast<uint16_t>(std::stoi(argv[1]));
    std::string password = (argc >= 3) ? argv[2] : "";
    std::string logfile  = (argc >= 4) ? argv[3] : "chatlog.txt";

    ChatLog log(logfile);

    log.write("[admin] Starting server on port " + std::to_string(port));
    if (!password.empty())
        log.write("[admin] Password protection enabled");

    boost::asio::io_context io;

    DedicatedServer server(io, port, password, log);

    // Load persistent bans from a file next to the log
    std::string bans_file = "bans.txt";
    server.load_bans(bans_file);

    // UPnP
    UPnPMapper upnp;
    if (upnp.discover())
    {
        if (upnp.openPortBoth(std::to_string(port)))
        {
            log.write("[admin] UPnP port mapping succeeded");
            if (!upnp.externalIP().empty())
                log.write("[admin] External address: " + upnp.externalIP() + ":" + std::to_string(port));
        }
        else
        {
            log.write("[admin] UPnP discover OK but mapping failed - forward port manually");
        }
    }
    else
    {
        log.write("[admin] UPnP unavailable - forward port " + std::to_string(port) + " manually");
    }

    std::cout << "chatbox dedicated server running on port " << port << "\n";
    std::cout << "Log: " << logfile << " | Bans: " << bans_file << "\n";

    std::atomic<bool> quit_flag(false);

    // Run network on a background thread
    std::thread net_thread([&]{ io.run(); });

    // Admin console on the main thread (blocks on stdin)
    admin_console(server, log, quit_flag);

    log.write("[admin] Shutting down");
    io.stop();
    net_thread.join();

    return 0;
}
