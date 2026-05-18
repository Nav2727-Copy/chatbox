/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: CC BY-NC-SA 4.0 (https://creativecommons.org/licenses/by-nc-sa/4.0/)
*/

/*
Todo: (FINISH THIS LIST BEFORE BETA RELEASE!!!!)
- Add support for multiple chat rooms (currently only one global room)
- Add real encryption for password protected rooms
- Clean up code and improve structure (currently a bit of a mess, especially with global variables)
- Regret life.
*/

// needs vcpkg my beloved
#include <curses.h>
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <boost/asio.hpp>

// c++20 standard libraries
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <array>
#include <cctype>
#include <optional>

using boost::asio::ip::tcp;

// =====================================================
// UPnP
// =====================================================

class UPnPMapper
{
public:
    UPnPMapper() = default;

    ~UPnPMapper()
    {
        if (!discovered_)
            return;

        for (auto& m : mappings_)
            UPNP_DeletePortMapping(
                urls_.controlURL,
                data_.first.servicetype,
                m.port.c_str(),
                m.proto.c_str(),
                nullptr);

        FreeUPNPUrls(&urls_);
    }

    bool discover()
    {
        int error = 0;

        UPNPDev* devlist = upnpDiscover(
            2000, nullptr, nullptr, 0, 0, 2, &error);

        if (!devlist)
        {
            lastError_ = "No UPnP devices found (err " + std::to_string(error) + ")";
            return false;
        }

        char lanAddr[64] = {};
        char wanAddr[64] = {};

        int status = UPNP_GetValidIGD(
            devlist, &urls_, &data_,
            lanAddr, sizeof(lanAddr),
            wanAddr, sizeof(wanAddr));

        freeUPNPDevlist(devlist);

        if (status != 1)
        {
            lastError_ = "No valid IGD found (status " + std::to_string(status) + ")";
            return false;
        }

        localIP_ = lanAddr;
        discovered_ = true;

        char extIP[64] = {};
        if (UPNP_GetExternalIPAddress(
            urls_.controlURL,
            data_.first.servicetype,
            extIP) == UPNPCOMMAND_SUCCESS)
        {
            externalIP_ = extIP;
        }

        return true;
    }

    bool openPort(
        const std::string& port,
        const std::string& proto = "TCP",
        const std::string& desc = "P2P Chat")
    {
        if (!discovered_)
            return false;

        UPNP_DeletePortMapping(
            urls_.controlURL,
            data_.first.servicetype,
            port.c_str(), proto.c_str(), nullptr);

        int r = UPNP_AddPortMapping(
            urls_.controlURL,
            data_.first.servicetype,
            port.c_str(), port.c_str(),
            localIP_.c_str(), desc.c_str(),
            proto.c_str(), nullptr, "0");

        if (r != UPNPCOMMAND_SUCCESS)
        {
            lastError_ = "AddPortMapping failed: code " + std::to_string(r);
            return false;
        }

        mappings_.push_back({ port, proto });
        return true;
    }

    bool openPortBoth(const std::string& port, const std::string& desc = "P2P Chat")
    {
        bool tcp = openPort(port, "TCP", desc);
        bool udp = openPort(port, "UDP", desc);
        return tcp || udp;
    }

    std::string externalIP() const { return externalIP_; }
    std::string localIP()    const { return localIP_; }
    std::string lastError()  const { return lastError_; }

private:
    struct Mapping { std::string port, proto; };

    UPNPUrls    urls_{};
    IGDdatas    data_{};
    std::string localIP_, externalIP_, lastError_;
    bool        discovered_ = false;
    std::vector<Mapping> mappings_;
};

// =====================================================
// Globals
// =====================================================

std::mutex g_mutex;
std::mutex g_window_mutex;

std::deque<std::string> g_messages;
std::vector<std::string> g_users;

constexpr int MAX_MESSAGES = 200;
constexpr int SERVER_HISTORY_LIMIT = 100;
constexpr int RATE_LIMIT_MESSAGES = 20;
constexpr int RATE_LIMIT_WINDOW_SECONDS = 60;
constexpr int MAX_WIRE_LINE_LENGTH = 4096;
constexpr int MAX_CHAT_MESSAGE_LEN = 1000;
constexpr int USERS_WINDOW_WIDTH = 24;
constexpr int INPUT_WINDOW_HEIGHT = 3;
constexpr int NICKNAME_BUF_SIZE = 32;
constexpr int HOST_BUF_SIZE = 64;
constexpr int PORT_BUF_SIZE = 16;
constexpr int PASS_BUF_SIZE = 64;
constexpr int LOGFILE_BUF_SIZE = 260;
constexpr int NICK_MAX_LEN = 31;
constexpr int HOST_MAX_LEN = 63;
constexpr int PORT_MAX_LEN = 15;
constexpr int PASS_MAX_LEN = 63;
constexpr int LOGFILE_MAX_LEN = 259;

std::string g_nickname;

WINDOW* g_users_win = nullptr;
WINDOW* g_chat_win = nullptr;
WINDOW* g_input_win = nullptr;

int g_last_rows = 0;
int g_last_cols = 0;

std::atomic<bool> g_shutdown_requested(false);
std::atomic<bool> g_kicked(false);        // set when server sends KICK to this client

// Base64 alphabet
constexpr const char* BASE64_CHARS =
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// =====================================================
// Utilities
// =====================================================

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
        output += (i - 1 < static_cast<int>(input.size())) ? BASE64_CHARS[b & 0x3F] : '=';
    }

    return output;
}

bool try_decode_base64(const std::string& input, std::string& output)
{
    output.clear();

    if (input.empty() || input.size() % 4 != 0)
        return false;

    auto char_to_val = [](char c) -> int
        {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
        };

    for (size_t i = 0; i < input.size(); i += 4)
    {
        const bool final_group = i + 4 == input.size();
        int v0 = char_to_val(input[i]);
        int v1 = char_to_val(input[i + 1]);
        int v2 = char_to_val(input[i + 2]);
        int v3 = char_to_val(input[i + 3]);

        if (v0 < 0 || v1 < 0)
            return false;

        output += static_cast<char>((v0 << 2) | (v1 >> 4));

        if (input[i + 2] != '=')
        {
            if (v2 < 0)
                return false;

            output += static_cast<char>((v1 << 4) | (v2 >> 2));
            if (input[i + 3] != '=')
            {
                if (v3 < 0)
                    return false;
                output += static_cast<char>((v2 << 6) | v3);
            }
            else if (!final_group)
            {
                return false;
            }
        }
        else
        {
            if (input[i + 3] != '=' || !final_group)
                return false;
        }
    }

    return true;
}

std::string decode_base64(const std::string& input)
{
    std::string output;
    if (!try_decode_base64(input, output))
        return "";
    return output;
}

void strip_wire_newline(std::string& line)
{
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
}

bool is_valid_nickname(const std::string& nick)
{
    if (nick.empty() || nick.size() > NICK_MAX_LEN)
        return false;

    for (unsigned char ch : nick)
    {
        if (std::iscntrl(ch) || std::isspace(ch) || ch == '|' || ch == ',')
            return false;
    }

    return true;
}

bool is_valid_chat_message(const std::string& message)
{
    if (message.empty() || message.size() > MAX_CHAT_MESSAGE_LEN)
        return false;

    for (unsigned char ch : message)
    {
        if (std::iscntrl(ch) && ch != '\t')
            return false;
    }

    return true;
}

std::string timestamp()
{
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif

    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
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

std::string join_fields(const std::vector<std::string>& fields, size_t start, char delim)
{
    std::string out;
    for (size_t i = start; i < fields.size(); ++i)
    {
        if (i > start)
            out += delim;
        out += fields[i];
    }
    return out;
}

void push_message(const std::string& msg)
{
    std::lock_guard lock(g_mutex);
    g_messages.push_back(msg);
    if (g_messages.size() > MAX_MESSAGES)
        g_messages.pop_front();
}

void add_user(const std::string& user)
{
    std::lock_guard lock(g_mutex);
    if (std::find(g_users.begin(), g_users.end(), user) == g_users.end())
        g_users.push_back(user);
}

void remove_user(const std::string& user)
{
    std::lock_guard lock(g_mutex);
    g_users.erase(
        std::remove(g_users.begin(), g_users.end(), user),
        g_users.end());
}

std::vector<std::string> connected_users()
{
    std::lock_guard lock(g_mutex);
    return g_users;
}

// =====================================================
// Chat log
// =====================================================

class ChatLog
{
public:
    enum class Mode
    {
        File,
        StdoutOnly
    };

    explicit ChatLog(const std::string& path, Mode mode = Mode::File)
        : path_(path), mode_(mode)
    {
        if (mode_ == Mode::File)
        {
            file_.open(path_, std::ios::app);
            if (!file_.is_open())
                std::cerr << "[warn] Could not open log file: " << path_ << "\n";
        }

        write("=== Server started ===");
    }

    ~ChatLog()
    {
        if (mode_ == Mode::StdoutOnly || file_.is_open())
            write("=== Server stopped ===");

        if (file_.is_open())
            file_.close();
    }

    void write(const std::string& line)
    {
        std::lock_guard lock(mutex_);
        if (file_.is_open())
        {
            file_ << "[" << timestamp() << "] " << line << "\n";
            file_.flush();
        }
        std::cout << "[log] " << line << "\n";
    }

private:
    std::string path_;
    Mode mode_;
    std::ofstream file_;
    std::mutex mutex_;
};

// =====================================================
// Command handling
// =====================================================

struct Command
{
    std::string name;
    std::vector<std::string> args;
};

std::optional<Command> parse_command(const std::string& input)
{
    if (input.empty() || input[0] != '/')
        return std::nullopt;

    auto parts = split(input.substr(1), ' ');
    if (parts.empty())
        return std::nullopt;

    Command cmd;
    cmd.name = parts[0];
    for (size_t i = 1; i < parts.size(); ++i)
        cmd.args.push_back(parts[i]);

    return cmd;
}

void show_command_help()
{
    push_message("[system] === Available Commands ===");
    push_message("[system] /help                    - Show this help message");
    push_message("[system] /users                   - List all connected users");
    push_message("[system] /clear                   - Clear message history");
    push_message("[system] /time                    - Show current time");
    push_message("[system] /whisper <nick> <msg>    - Send private message");
    push_message("[system] /exit                    - Exit the application");
    push_message("[system] === End Help ===");
}

// =====================================================
// Forward declarations
// =====================================================

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

    // The nickname is set once AUTH/JOIN is accepted
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

    boost::asio::io_context& io_;
    tcp::acceptor acceptor_;
    std::string   password_;
    ChatLog*      log_ = nullptr;
    std::string   bans_file_;
    std::string   local_nickname_;

    mutable std::mutex sessions_mutex_;
    std::set<std::shared_ptr<ClientSession>> sessions_;

    mutable std::mutex ban_mutex_;
    std::set<std::string> banned_nicks_;

    mutable std::mutex history_mutex_;
    std::deque<std::string> history_;
};

// =====================================================
// Client session (server side)
// =====================================================

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

            // ---- JOIN ----
            if (parts[0] == "JOIN" && parts.size() >= 2)
            {
                std::string nick = parts[1];

                if (!nickname_.empty())
                {
                    deliver("JOIN_FAIL|Already joined");
                    read_loop();
                    return;
                }

                if (!is_valid_nickname(nick))
                {
                    deliver("JOIN_FAIL|Invalid nickname");
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

                nickname_ = nick;
                add_user(nickname_);
                deliver("JOIN_OK");
                server_.deliver_history(self);
                server_.broadcast("[system] " + nickname_ + " joined");
                server_.broadcast_users();
            }
            // ---- LEAVE ----
            else if (parts[0] == "LEAVE" && parts.size() >= 2)
            {
                if (nickname_.empty() || parts[1] != nickname_)
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
            else if (parts[0] == "MSG" && parts.size() >= 3)
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

                if (parts[1] != nickname_)
                {
                    deliver("[system] Ignored message with mismatched sender");
                    read_loop();
                    return;
                }

                std::string message = join_fields(parts, 2, '|');
                if (!is_valid_chat_message(message))
                {
                    deliver("[system] Invalid message text");
                    read_loop();
                    return;
                }

                server_.broadcast("[" + timestamp() + "] " + nickname_ + ": " + message);
            }
            // ---- WHISPER ----
            else if (parts[0] == "WHISPER" && parts.size() >= 4)
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

                if (parts[1] != nickname_)
                {
                    deliver("[system] Ignored whisper with mismatched sender");
                    read_loop();
                    return;
                }

                std::string message = join_fields(parts, 3, '|');
                if (!is_valid_nickname(parts[2]) || !is_valid_chat_message(message))
                {
                    deliver("[system] Invalid whisper target or message");
                    read_loop();
                    return;
                }

                server_.send_private(nickname_, parts[2], message);
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

// =====================================================
// Client
// =====================================================

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
    std::string       kick_reason_;
    std::string       join_failure_reason_;
    bool              close_after_write_ = false;
};

// =====================================================
// UI
// =====================================================

void draw_ui(const std::string& input)
{
    std::lock_guard window_lock(g_window_mutex);

    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    if (rows != g_last_rows || cols != g_last_cols || !g_users_win)
    {
        if (g_users_win) delwin(g_users_win);
        if (g_chat_win)  delwin(g_chat_win);
        if (g_input_win) delwin(g_input_win);

        g_users_win = newwin(rows - INPUT_WINDOW_HEIGHT, USERS_WINDOW_WIDTH, 0, 0);
        g_chat_win = newwin(rows - INPUT_WINDOW_HEIGHT, cols - USERS_WINDOW_WIDTH, 0, USERS_WINDOW_WIDTH);
        g_input_win = newwin(INPUT_WINDOW_HEIGHT, cols, rows - INPUT_WINDOW_HEIGHT, 0);

        g_last_rows = rows;
        g_last_cols = cols;

        box(g_users_win, 0, 0);
        box(g_chat_win, 0, 0);
        box(g_input_win, 0, 0);
    }
    else
    {
        wclear(g_users_win);
        wclear(g_chat_win);
        wclear(g_input_win);

        box(g_users_win, 0, 0);
        box(g_chat_win, 0, 0);
        box(g_input_win, 0, 0);
    }

    wattron(g_users_win, COLOR_PAIR(1)); mvwprintw(g_users_win, 0, 2, " Users "); wattroff(g_users_win, COLOR_PAIR(1));
    wattron(g_chat_win, COLOR_PAIR(2)); mvwprintw(g_chat_win, 0, 2, " Chat ");  wattroff(g_chat_win, COLOR_PAIR(2));
    wattron(g_input_win, COLOR_PAIR(3)); mvwprintw(g_input_win, 0, 2, " Input "); wattroff(g_input_win, COLOR_PAIR(3));

    {
        std::lock_guard lock(g_mutex);

        int y = 1;
        for (const auto& user : g_users)
        {
            if (y >= rows - INPUT_WINDOW_HEIGHT - 1) break;
            mvwprintw(g_users_win, y++, 2, "%s", user.c_str());
        }

        int max_chat_lines = rows - INPUT_WINDOW_HEIGHT - 2;
        int start = std::max(0, (int)g_messages.size() - max_chat_lines);
        y = 1;
        for (int i = start; i < (int)g_messages.size(); ++i)
            mvwprintw(g_chat_win, y++, 2, "%s", g_messages[i].c_str());
    }

    mvwprintw(g_input_win, 1, 2, "> %s", input.c_str());

    wnoutrefresh(g_users_win);
    wnoutrefresh(g_chat_win);
    wnoutrefresh(g_input_win);
    doupdate();
}

// =====================================================
// Password prompt (masked input)
// =====================================================

std::string prompt_password(int row, int col, const char* label)
{
    mvprintw(row, col, "%s", label);
    refresh();

    std::string pw;
    noecho();
    int ch;
    move(row + 1, col);

    while ((ch = getch()) != '\n' && ch != ERR)
    {
        if ((ch == KEY_BACKSPACE || ch == 127 || ch == 8) && !pw.empty())
        {
            pw.pop_back();
            int y, x;
            getyx(stdscr, y, x);
            move(y, x - 1);
            addch(' ');
            move(y, x - 1);
        }
        else if (isprint(ch))
        {
            pw += static_cast<char>(ch);
            addch('*');
        }
        refresh();
    }

    return pw;
}

bool prompt_yes_no(int row, int col, const char* label, bool default_yes = true)
{
    mvprintw(row, col, "%s", label);
    refresh();

    while (true)
    {
        int ch = getch();
        if (ch == '\n' || ch == '\r')
            return default_yes;
        if (ch == 'y' || ch == 'Y')
            return true;
        if (ch == 'n' || ch == 'N')
            return false;
    }
}

// =====================================================
// Dedicated server mode
// =====================================================

struct DedicatedServerConfig
{
    uint16_t port = 0;
    std::string password;
    std::string logfile = "chatlog.txt";
    std::string bans_file = "bans.txt";
    bool enable_logging = true;
    bool log_to_stdout_only = false;
    bool enable_upnp = true;
};

void show_dedicated_usage()
{
    std::cout
        << "Usage:\n"
        << "  chatbox --server <port> [password] [logfile] [options]\n"
        << "  chatbox --dedicated <port> [password] [logfile] [options]\n\n"
        << "Options:\n"
        << "  --password <password>  - set the room password\n"
        << "  --log <file>           - write logs to a file\n"
        << "  --log-stdout           - print logs only to stdout\n"
        << "  --no-log               - disable chat logging\n"
        << "  --no-upnp              - skip UPnP port forwarding\n\n"
        << "Dedicated server admin commands:\n"
        << "  /kick <nick> [reason]  - kick a connected user\n"
        << "  /ban  <nick> [reason]  - ban a user and persist it\n"
        << "  /unban <nick>          - remove a persisted ban\n"
        << "  /bans                  - list banned nicknames\n"
        << "  /users                 - list connected users\n"
        << "  /broadcast <msg>       - send a server announcement\n"
        << "  /quit                  - shut down the server\n"
        << "  /help                  - show this help\n";
}

std::string join_args(const std::vector<std::string>& args, size_t start)
{
    return join_fields(args, start, ' ');
}

bool parse_port(const std::string& text, uint16_t& port)
{
    try
    {
        size_t consumed = 0;
        int value = std::stoi(text, &consumed);
        if (consumed != text.size() || value < 0 || value > 65535)
            return false;

        port = static_cast<uint16_t>(value);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void admin_console(ChatServer& server, std::atomic<bool>& quit_flag)
{
    show_dedicated_usage();

    std::string line;
    while (!quit_flag && std::getline(std::cin, line))
    {
        if (line.empty())
            continue;

        auto parts = split(line, ' ');
        if (parts.empty())
            continue;

        const std::string& cmd = parts[0];

        if (cmd == "/quit" || cmd == "/exit")
        {
            quit_flag = true;
            break;
        }
        else if (cmd == "/help")
        {
            show_dedicated_usage();
        }
        else if (cmd == "/users")
        {
            auto users = server.connected_users();
            if (users.empty())
                std::cout << "(no users connected)\n";
            else
                for (const auto& user : users)
                    std::cout << "  - " << user << "\n";
        }
        else if (cmd == "/kick" && parts.size() >= 2)
        {
            std::string reason = join_args(parts, 2);
            if (server.kick_user(parts[1], reason))
                std::cout << "Kicked: " << parts[1] << "\n";
            else
                std::cout << "User not found: " << parts[1] << "\n";
        }
        else if (cmd == "/ban" && parts.size() >= 2)
        {
            server.ban_user(parts[1], join_args(parts, 2));
            std::cout << "Banned: " << parts[1] << "\n";
        }
        else if (cmd == "/unban" && parts.size() >= 2)
        {
            if (server.unban_user(parts[1]))
                std::cout << "Unbanned: " << parts[1] << "\n";
            else
                std::cout << "Not in ban list: " << parts[1] << "\n";
        }
        else if (cmd == "/bans")
        {
            auto bans = server.ban_list();
            if (bans.empty())
                std::cout << "(ban list is empty)\n";
            else
                for (const auto& nick : bans)
                    std::cout << "  - " << nick << "\n";
        }
        else if (cmd == "/broadcast" && parts.size() >= 2)
        {
            server.broadcast_server(join_args(parts, 1));
        }
        else
        {
            std::cout << "Unknown command. Type /help for a list.\n";
        }
    }
}

int run_dedicated_server_config(const DedicatedServerConfig& config)
{
    std::unique_ptr<ChatLog> log;
    if (config.enable_logging)
    {
        log = std::make_unique<ChatLog>(
            config.log_to_stdout_only ? "" : config.logfile,
            config.log_to_stdout_only ? ChatLog::Mode::StdoutOnly : ChatLog::Mode::File);
    }

    auto log_admin = [&](const std::string& line)
        {
            if (log)
                log->write(line);
            else
                std::cout << line << "\n";
        };

    log_admin("[admin] Starting dedicated server on port " + std::to_string(config.port));
    if (!config.password.empty())
        log_admin("[admin] Password protection enabled");

    boost::asio::io_context io;
    ChatServer server(io, config.port, config.password, log.get());
    server.load_bans(config.bans_file);

    UPnPMapper upnp;
    if (config.enable_upnp && upnp.discover())
    {
        if (upnp.openPortBoth(std::to_string(config.port), "chatbox dedicated server"))
        {
            log_admin("[admin] UPnP port mapping succeeded");
            if (!upnp.externalIP().empty())
                log_admin("[admin] External address: " + upnp.externalIP() + ":" + std::to_string(config.port));
        }
        else
        {
            log_admin("[admin] UPnP mapping failed: " + upnp.lastError());
            log_admin("[admin] Forward port " + std::to_string(config.port) + " manually for internet access");
        }
    }
    else if (!config.enable_upnp)
    {
        log_admin("[admin] UPnP disabled; forward port "
            + std::to_string(config.port) + " manually for internet access");
    }
    else
    {
        log_admin("[admin] UPnP unavailable: " + upnp.lastError());
        log_admin("[admin] Forward port " + std::to_string(config.port) + " manually for internet access");
    }

    try
    {
        tcp::resolver resolver(io);
        auto results = resolver.resolve(boost::asio::ip::host_name(), "");
        log_admin("[admin] LAN addresses:");
        for (auto& result : results)
        {
            auto addr = result.endpoint().address();
            if (!addr.is_loopback())
                log_admin("[admin]   " + addr.to_string() + ":" + std::to_string(config.port));
        }
    }
    catch (...) {}

    std::cout << "chatbox dedicated server running on port " << config.port << "\n";
    std::cout << "Log: ";
    if (!config.enable_logging)
        std::cout << "disabled";
    else if (config.log_to_stdout_only)
        std::cout << "stdout only";
    else
        std::cout << config.logfile;
    std::cout << " | Bans: " << config.bans_file << "\n";

    std::atomic<bool> quit_flag(false);
    std::thread network_thread([&] { io.run(); });

    admin_console(server, quit_flag);

    log_admin("[admin] Shutting down");
    server.stop();
    io.stop();
    network_thread.join();
    return 0;
}

int run_dedicated_server(int argc, char* argv[])
{
    if (argc < 3)
    {
        show_dedicated_usage();
        return 1;
    }

    uint16_t port = 0;
    if (!parse_port(argv[2], port))
    {
        std::cerr << "Invalid port number: " << argv[2] << "\n";
        return 1;
    }

    DedicatedServerConfig config;
    config.port = port;

    bool positional_password_set = false;
    bool positional_log_set = false;

    for (int i = 3; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (arg == "--no-upnp")
        {
            config.enable_upnp = false;
        }
        else if (arg == "--no-log")
        {
            config.enable_logging = false;
        }
        else if (arg == "--log-stdout")
        {
            config.enable_logging = true;
            config.log_to_stdout_only = true;
        }
        else if (arg == "--password" && i + 1 < argc)
        {
            config.password = argv[++i];
            positional_password_set = true;
        }
        else if (arg == "--log" && i + 1 < argc)
        {
            config.logfile = argv[++i];
            config.enable_logging = true;
            config.log_to_stdout_only = false;
            positional_log_set = true;
        }
        else if (arg == "--help" || arg == "-h")
        {
            show_dedicated_usage();
            return 0;
        }
        else if (arg == "--password" || arg == "--log")
        {
            std::cerr << arg << " requires a value\n";
            show_dedicated_usage();
            return 1;
        }
        else if (arg.rfind("--", 0) == 0)
        {
            std::cerr << "Unknown option: " << arg << "\n";
            show_dedicated_usage();
            return 1;
        }
        else if (!positional_password_set)
        {
            config.password = arg;
            positional_password_set = true;
        }
        else if (!positional_log_set)
        {
            config.logfile = arg;
            positional_log_set = true;
        }
        else
        {
            std::cerr << "Unknown option: " << arg << "\n";
            show_dedicated_usage();
            return 1;
        }
    }

    return run_dedicated_server_config(config);
}

// =====================================================
// Main
// =====================================================

int main(int argc, char* argv[])
{
    if (argc >= 2)
    {
        std::string mode = argv[1];
        if (mode == "--server" || mode == "--dedicated" || mode == "-s")
            return run_dedicated_server(argc, argv);

        if (mode == "--help" || mode == "-h")
        {
            show_dedicated_usage();
            return 0;
        }

        std::cerr << "Unknown option: " << mode << "\n";
        show_dedicated_usage();
        return 1;
    }

    boost::asio::io_context io;

    std::unique_ptr<ChatServer> server;
    std::unique_ptr<ChatClient> client;
    std::unique_ptr<UPnPMapper> upnp;

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    start_color();

    init_pair(1, COLOR_CYAN, COLOR_BLACK);
    init_pair(2, COLOR_GREEN, COLOR_BLACK);
    init_pair(3, COLOR_YELLOW, COLOR_BLACK);

    // =========================================
    // Startup mode prompt
    // =========================================

    bool startup_selected = false;
    while (!startup_selected)
    {
        erase();
        box(stdscr, 0, 0);
        mvprintw(2, 4, "chatbox alpha");
        mvprintw(4, 4, "[C] Chat mode");
        mvprintw(5, 4, "[D] Dedicated server");
        mvprintw(6, 4, "[Q] Quit");
        refresh();

        int ch = getch();

        if (ch == 'q' || ch == 'Q')
        {
            endwin();
            return 0;
        }
        else if (ch == 'c' || ch == 'C')
        {
            startup_selected = true;
        }
        else if (ch == 'd' || ch == 'D')
        {
            echo();

            char port_buf[PORT_BUF_SIZE] = {};
            char logfile_buf[LOGFILE_BUF_SIZE] = {};

            erase();
            box(stdscr, 0, 0);
            mvprintw(2, 4, "Dedicated Server");
            mvprintw(4, 4, "Port:");
            move(5, 4);
            getnstr(port_buf, PORT_MAX_LEN);

            noecho();
            std::string room_password = prompt_password(7, 4,
                "Room password (leave blank for none):");

            bool enable_upnp = prompt_yes_no(10, 4, "Enable UPnP port forwarding? [Y/n]:");

            echo();
            mvprintw(12, 4, "Log file (blank chatlog.txt, 'none' disables, 'stdout' console only):");
            move(13, 4);
            getnstr(logfile_buf, LOGFILE_MAX_LEN);
            noecho();

            uint16_t port = 0;
            if (!parse_port(port_buf, port))
            {
                erase();
                box(stdscr, 0, 0);
                mvprintw(2, 4, "Invalid port number (0-65535)");
                refresh();
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }

            DedicatedServerConfig config;
            config.port = port;
            config.password = room_password;
            config.enable_upnp = enable_upnp;

            std::string logfile = logfile_buf;
            if (logfile == "none")
            {
                config.enable_logging = false;
            }
            else if (logfile == "stdout")
            {
                config.log_to_stdout_only = true;
            }
            else if (!logfile.empty())
            {
                config.logfile = logfile;
            }

            endwin();
            return run_dedicated_server_config(config);
        }
    }

    // =========================================
    // Nickname prompt
    // =========================================

    while (true)
    {
        echo();

        char nick_buf[NICKNAME_BUF_SIZE] = {};
        erase();
        box(stdscr, 0, 0);
        mvprintw(2, 4, "Enter nickname:");
        move(3, 4);
        getnstr(nick_buf, NICK_MAX_LEN);
        g_nickname = nick_buf;

        noecho();

        if (is_valid_nickname(g_nickname))
            break;

        erase();
        box(stdscr, 0, 0);
        mvprintw(2, 4, "Invalid nickname. Use 1-%d non-space characters without | or ,.", NICK_MAX_LEN);
        refresh();
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    // =========================================
    // Main menu
    // =========================================

    bool configured = false;

    while (!configured)
    {
        erase();
        box(stdscr, 0, 0);
        mvprintw(2, 4, "chatbox alpha");
        mvprintw(4, 4, "[H] Host server");
        mvprintw(5, 4, "[J] Join server");
        mvprintw(6, 4, "[Q] Quit");
        refresh();

        int ch = getch();

        if (ch == 'q' || ch == 'Q')
        {
            endwin();
            return 0;
        }

        // ---- HOST ----
        else if (ch == 'h' || ch == 'H')
        {
            echo();

            char port_buf[PORT_BUF_SIZE];
            erase();
            box(stdscr, 0, 0);
            mvprintw(2, 4, "Host Server");
            mvprintw(4, 4, "Port:");
            move(5, 4);
            getnstr(port_buf, PORT_MAX_LEN);

            noecho();

            // Password setup (optional)
            std::string room_password = prompt_password(7, 4,
                "Room password (leave blank for none):");

            bool enable_upnp = prompt_yes_no(10, 4, "Enable UPnP port forwarding? [Y/n]:");

            try
            {
                uint16_t port = 0;
                if (!parse_port(port_buf, port))
                {
                    erase(); box(stdscr, 0, 0);
                    mvprintw(2, 4, "Invalid port number (0-65535)");
                    refresh();
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    continue;
                }

                server = std::make_unique<ChatServer>(
                    io,
                    port,
                    room_password,
                    nullptr,
                    g_nickname);

                add_user(g_nickname);
                push_message("[system] Hosting on port " + std::to_string(port));

                if (!room_password.empty())
                    push_message("[system] Room password protection enabled");

                // UPnP
                if (enable_upnp)
                {
                    upnp = std::make_unique<UPnPMapper>();
                    push_message("[system] Attempting UPnP port mapping...");

                    if (upnp->discover())
                    {
                        if (upnp->openPortBoth(std::to_string(port)))
                        {
                            push_message("[system] UPnP OK - port " + std::to_string(port) + " opened on router");
                            if (!upnp->externalIP().empty())
                            {
                                push_message("[system] External address: " + upnp->externalIP() + ":" + std::to_string(port));
                                push_message("[system] Share that address with your peer");
                            }
                        }
                        else
                        {
                            push_message("[system] UPnP mapping failed: " + upnp->lastError());
                            push_message("[system] You may need to forward port " + std::to_string(port) + " manually");
                        }
                    }
                    else
                    {
                        push_message("[system] UPnP unavailable: " + upnp->lastError());
                        push_message("[system] Forward port " + std::to_string(port) + " manually for internet access");
                    }
                }
                else
                {
                    push_message("[system] UPnP disabled; forward port " + std::to_string(port) + " manually for internet access");
                }

                // LAN addresses
                try
                {
                    tcp::resolver resolver(io);
                    auto results = resolver.resolve(boost::asio::ip::host_name(), "");
                    push_message("[system] LAN addresses:");
                    for (auto& r : results)
                    {
                        auto addr = r.endpoint().address();
                        if (!addr.is_loopback())
                            push_message("[system]   " + addr.to_string() + ":" + std::to_string(port));
                    }
                }
                catch (...) {}

                configured = true;
            }
            catch (...)
            {
                erase(); box(stdscr, 0, 0);
                mvprintw(2, 4, "Invalid port number");
                refresh();
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
        }

        // ---- JOIN ----
        else if (ch == 'j' || ch == 'J')
        {
            echo();

            char host_buf[HOST_BUF_SIZE];
            char port_buf[PORT_BUF_SIZE];

            erase();
            box(stdscr, 0, 0);
            mvprintw(2, 4, "Join Server");
            mvprintw(4, 4, "Host:");
            move(5, 4);
            getnstr(host_buf, HOST_MAX_LEN);

            mvprintw(7, 4, "Port:");
            move(8, 4);
            getnstr(port_buf, PORT_MAX_LEN);

            noecho();

            try
            {
                uint16_t port = 0;
                if (!parse_port(port_buf, port))
                {
                    erase(); box(stdscr, 0, 0);
                    mvprintw(2, 4, "Invalid port number (0-65535)");
                    refresh();
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    continue;
                }

                client = std::make_unique<ChatClient>(io);

                if (!client->connect(host_buf, port))
                {
                    mvprintw(10, 4, "Connection failed.");
                    refresh();
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    client.reset();
                    continue;
                }

                // Start network thread early so async reads can progress.
                std::thread network_thread([&] { io.run(); });

                for (int i = 0; i < 20 && !client->auth_required() && !client->auth_resolved(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));

                if (client->auth_required())
                {
                    // Prompt for password (masked)
                    std::string pw = prompt_password(10, 4, "Server password:");
                    client->send("AUTH|" + pw);

                    // Wait for result
                    erase(); box(stdscr, 0, 0);
                    mvprintw(2, 4, "Authenticating...");
                    refresh();

                    for (int i = 0; i < 50 && !client->auth_resolved(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));

                    if (!client->auth_ok())
                    {
                        erase(); box(stdscr, 0, 0);
                        mvprintw(2, 4, "Authentication failed. Wrong password.");
                        refresh();
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        client->close();
                        if (network_thread.joinable())
                            network_thread.join();
                        client.reset();
                        io.restart();
                        continue;
                    }
                }

                // Check ban before joining
                client->send("JOIN|" + g_nickname);
                for (int i = 0; i < 50 && !client->join_resolved(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                if (client->was_banned())
                {
                    erase(); box(stdscr, 0, 0);
                    mvprintw(2, 4, "You are banned from this server.");
                    refresh();
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    client->close();
                    if (network_thread.joinable())
                        network_thread.join();
                    client.reset();
                    io.restart();
                    continue;
                }

                if (client->nickname_taken())
                {
                    erase(); box(stdscr, 0, 0);
                    mvprintw(2, 4, "Nickname is already in use.");
                    refresh();
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    client->close();
                    if (network_thread.joinable())
                        network_thread.join();
                    client.reset();
                    io.restart();
                    continue;
                }

                if (client->join_failed())
                {
                    erase(); box(stdscr, 0, 0);
                    mvprintw(2, 4, "Join failed: %s", client->join_failure_reason().c_str());
                    refresh();
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    client->close();
                    if (network_thread.joinable())
                        network_thread.join();
                    client.reset();
                    io.restart();
                    continue;
                }

                add_user(g_nickname);
                push_message("[system] Connected to " + std::string(host_buf));
                configured = true;

                timeout(30);

                std::string input;
                bool running = true;

                while (running)
                {
                    if (g_kicked)
                    {
                        push_message("[system] Press any key to exit...");
                        draw_ui(input);
                        std::this_thread::sleep_for(std::chrono::seconds(3));
                        running = false;
                        break;
                    }

                    draw_ui(input);
                    int c = getch();

                    switch (c)
                    {
                    case ERR: break;
                    case 27:  running = false; break;

                    case '\n':
                    {
                        if (!input.empty())
                        {
                            auto cmd = parse_command(input);
                            if (cmd)
                            {
                                if (cmd->name == "help")
                                {
                                    show_command_help();
                                }
                                else if (cmd->name == "users")
                                {
                                    push_message("[system] Connected users:");
                                    for (const auto& u : connected_users())
                                        push_message("[system]   - " + u);
                                }
                                else if (cmd->name == "clear")
                                {
                                    {
                                        std::lock_guard lock(g_mutex);
                                        g_messages.clear();
                                    }
                                    push_message("[system] Message history cleared");
                                }
                                else if (cmd->name == "time")
                                {
                                    push_message("[system] Current time: " + timestamp());
                                }
                                else if (cmd->name == "whisper" && cmd->args.size() >= 1)
                                {
                                    size_t cmd_end = input.find(' ');
                                    if (cmd_end != std::string::npos)
                                    {
                                        cmd_end = input.find(' ', cmd_end + 1);
                                        if (cmd_end != std::string::npos)
                                        {
                                            std::string target = cmd->args[0];
                                            std::string message = input.substr(cmd_end + 1);
                                            client->send("WHISPER|" + g_nickname + "|" + target + "|" + message);
                                        }
                                    }
                                }
                                else if (cmd->name == "exit")
                                {
                                    running = false;
                                }
                                else
                                {
                                    push_message("[system] Unknown command: " + cmd->name + ". Type /help.");
                                }
                            }
                            else
                            {
                                client->send("MSG|" + g_nickname + "|" + input);
                            }

                            input.clear();
                        }
                        break;
                    }

                    case KEY_BACKSPACE:
                    case 127:
                    case 8:
                        if (!input.empty()) input.pop_back();
                        break;

                    default:
                        if (isprint(c))
                            input += static_cast<char>(c);
                        break;
                    }
                }

                // Shutdown client
                if (!g_kicked)
                    client->send("LEAVE|" + g_nickname);
                client->close();
                if (network_thread.joinable())
                    network_thread.join();

                std::lock_guard window_lock(g_window_mutex);
                if (g_users_win) delwin(g_users_win);
                if (g_chat_win)  delwin(g_chat_win);
                if (g_input_win) delwin(g_input_win);

                endwin();
                return 0;
            }
            catch (...)
            {
                erase(); box(stdscr, 0, 0);
                mvprintw(2, 4, "Invalid port number");
                refresh();
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
        }
    }

    // =========================================
    // Host: run chat loop
    // =========================================

    timeout(30);

    std::thread network_thread([&] { io.run(); });

    std::string input;
    bool running = true;

    while (running)
    {
        draw_ui(input);
        int ch = getch();

        switch (ch)
        {
        case ERR: break;
        case 27:  running = false; break;

        case '\n':
        {
            if (!input.empty())
            {
                auto cmd = parse_command(input);
                if (cmd)
                {
                    if (cmd->name == "help")
                    {
                        show_command_help();
                        push_message("[system] /kick <nick> [reason] - Kick a user (host only)");
                        push_message("[system] /ban  <nick> [reason] - Ban  a user (host only)");
                        push_message("[system] /unban <nick>         - Unban a user (host only)");
                        push_message("[system] /bans                 - List banned users");
                    }
                    else if (cmd->name == "users")
                    {
                        push_message("[system] Connected users:");
                        for (const auto& u : connected_users())
                            push_message("[system]   - " + u);
                    }
                    else if (cmd->name == "clear")
                    {
                        {
                            std::lock_guard lock(g_mutex);
                            g_messages.clear();
                        }
                        push_message("[system] Message history cleared");
                    }
                    else if (cmd->name == "time")
                    {
                        push_message("[system] Current time: " + timestamp());
                    }
                    else if (cmd->name == "whisper" && cmd->args.size() >= 1)
                    {
                        size_t cmd_end = input.find(' ');
                        if (cmd_end != std::string::npos)
                        {
                            cmd_end = input.find(' ', cmd_end + 1);
                            if (cmd_end != std::string::npos)
                            {
                                std::string target = cmd->args[0];
                                std::string message = input.substr(cmd_end + 1);
                                if (server)
                                    server->send_private(g_nickname, target, message, true);
                            }
                        }
                    }
                    // ---- KICK (host only) ----
                    else if (cmd->name == "kick" && server && cmd->args.size() >= 1)
                    {
                        std::string nick = cmd->args[0];
                        std::string reason = join_args(cmd->args, 1);
                        if (!server->kick_user(nick, reason))
                            push_message("[system] User '" + nick + "' not found");
                    }
                    // ---- BAN (host only) ----
                    else if (cmd->name == "ban" && server && cmd->args.size() >= 1)
                    {
                        std::string nick = cmd->args[0];
                        std::string reason = join_args(cmd->args, 1);
                        server->ban_user(nick, reason);
                    }
                    // ---- UNBAN (host only) ----
                    else if (cmd->name == "unban" && server && cmd->args.size() >= 1)
                    {
                        std::string nick = cmd->args[0];
                        if (server->unban_user(nick))
                            push_message("[system] " + nick + " has been unbanned");
                        else
                            push_message("[system] '" + nick + "' is not on the ban list");
                    }
                    // ---- BAN LIST ----
                    else if (cmd->name == "bans" && server)
                    {
                        auto bans = server->ban_list();
                        if (bans.empty())
                            push_message("[system] Ban list is empty");
                        else
                        {
                            push_message("[system] Banned users:");
                            for (const auto& b : bans)
                                push_message("[system]   - " + b);
                        }
                    }
                    else if (cmd->name == "exit")
                    {
                        running = false;
                    }
                    else
                    {
                        push_message("[system] Unknown command: " + cmd->name + ". Type /help.");
                    }
                }
                else
                {
                    // Host broadcasts directly
                    if (server)
                    {
                        server->broadcast(
                            "[" + timestamp() + "] " + g_nickname + ": " + input);
                    }
                }

                input.clear();
            }
            break;
        }

        case KEY_BACKSPACE:
        case 127:
        case 8:
            if (!input.empty()) input.pop_back();
            break;

        default:
            if (isprint(ch))
                input += static_cast<char>(ch);
            break;
        }
    }

    // =========================================
    // Shutdown (host path)
    // =========================================

    g_shutdown_requested = true;
    upnp.reset();

    {
        std::lock_guard window_lock(g_window_mutex);
        if (g_users_win) delwin(g_users_win);
        if (g_chat_win)  delwin(g_chat_win);
        if (g_input_win) delwin(g_input_win);
    }

    endwin();
    io.stop();
    network_thread.join();
    return 0;
}
