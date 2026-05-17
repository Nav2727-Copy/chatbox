/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: CC BY-NC-SA 4.0 (https://creativecommons.org/licenses/by-nc-sa/4.0/)
*/

/*
Todo: (absolutely not in order of importance)
- add real end-to-end encryption instead of base64 "obfuscation" (probably NaCl/libsodium)
- add gui for beta release instead of ncurses (Qt, Dear ImGui, or similar)
- regret everything
- linux support (is possible, but im lazy)
- terrorize the rust programmers by throwing rusted cans at them
- add file transfer for beta release, something like a mini torrent or ftp server and client so i dont have to write it myself)
- find a way to put the numbers 27 in an important place in the codebase to further fuel my ego
- clean up codebase and seperate into multiple files instead of one giant cpp file
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
constexpr int USERS_WINDOW_WIDTH = 24;
constexpr int INPUT_WINDOW_HEIGHT = 3;
constexpr int NICKNAME_BUF_SIZE = 32;
constexpr int HOST_BUF_SIZE = 64;
constexpr int PORT_BUF_SIZE = 16;
constexpr int PASS_BUF_SIZE = 64;
constexpr int NICK_MAX_LEN = 31;
constexpr int HOST_MAX_LEN = 63;
constexpr int PORT_MAX_LEN = 15;
constexpr int PASS_MAX_LEN = 63;

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

std::string decode_base64(const std::string& input)
{
    std::string output;

    if (input.size() % 4 != 0)
        return "";

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
        int v0 = char_to_val(input[i]);
        int v1 = char_to_val(input[i + 1]);
        int v2 = char_to_val(input[i + 2]);
        int v3 = char_to_val(input[i + 3]);

        if (v0 < 0 || v1 < 0)
            return "";

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
        : socket_(std::move(socket)), server_(server)
    {}

    void start();
    void deliver(const std::string& msg);

    // The nickname is set once AUTH/JOIN is accepted
    const std::string& nickname() const { return nickname_; }

private:
    void read_loop();

    tcp::socket   socket_;
    ChatServer& server_;
    boost::asio::streambuf buffer_;
    std::string   nickname_;
    bool          authenticated_ = false;   // true after password accepted
};

// =====================================================
// Server
// =====================================================

class ChatServer
{
public:
    ChatServer(boost::asio::io_context& io, uint16_t port,
        const std::string& password = "")
        : acceptor_(io), password_(password)
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
        std::lock_guard lock(sessions_mutex_);
        for (auto& s : sessions_)
            s->deliver(msg);
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
        std::lock_guard lock(sessions_mutex_);
        for (auto& s : sessions_)
            s->deliver(payload);
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

        // Give the message a moment to flush, then forcibly drop the session
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
        // If they're currently connected, kick them too
        kick_user(nick, reason.empty() ? "banned" : reason);
        broadcast("[system] " + nick + " has been banned");
        return true;
    }

    // Unban a user
    bool unban_user(const std::string& nick)
    {
        std::lock_guard lock(ban_mutex_);
        return banned_nicks_.erase(nick) > 0;
    }

    const std::set<std::string>& ban_list() const { return banned_nicks_; }

private:
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
                accept_loop();
            });
    }

    tcp::acceptor acceptor_;
    std::string   password_;

    mutable std::mutex sessions_mutex_;
    std::set<std::shared_ptr<ClientSession>> sessions_;

    mutable std::mutex ban_mutex_;
    std::set<std::string> banned_nicks_;
};

// =====================================================
// Client session (server side)
// =====================================================

void ClientSession::start()
{
    // If there's a password, ask for it first
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
                    server_.broadcast("[system] " + nickname_ + " left");
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
                    // Close connection after a short delay
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    server_.leave(self);
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

                if (server_.is_banned(nick))
                {
                    deliver("BANNED");
                    server_.leave(self);
                    return;
                }

                nickname_ = nick;
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
                server_.broadcast(
                    "[" + timestamp() + "] " + parts[1] + ": " + parts[2]);
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
// Client
// =====================================================

class ChatClient
{
public:
    ChatClient(boost::asio::io_context& io)
        : socket_(io)
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
                auto data = std::make_shared<std::string>(
                    encode_base64(msg) + "\n");

                boost::asio::async_write(
                    socket_, boost::asio::buffer(*data),
                    [data](boost::system::error_code ec, std::size_t)
                    {
                        if (ec)
                            std::cerr << "Send error: " << ec.message() << "\n";
                    });
            });
    }

    // AUTH state accessors (polled by main thread during login)
    bool auth_required()       const { return auth_required_; }
    bool auth_resolved()       const { return auth_resolved_; }
    bool auth_ok()             const { return auth_ok_; }
    bool was_kicked()          const { return kicked_; }
    bool was_banned()          const { return banned_; }
    const std::string& kick_reason() const { return kick_reason_; }

private:
    void read_loop()
    {
        boost::asio::async_read_until(
            socket_, buffer_, '\n',
            [this](boost::system::error_code ec, std::size_t)
            {
                if (ec)
                {
                    push_message("[system] disconnected");
                    return;
                }

                std::istream is(&buffer_);
                std::string  line;
                std::getline(is, line);

                std::string decoded = decode_base64(line);

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
                else if (decoded == "BANNED")
                {
                    banned_ = true;
                    push_message("[system] You are banned from this server");
                }
                else if (decoded.rfind("KICK|", 0) == 0)
                {
                    kicked_ = true;
                    auto parts = split(decoded, '|');
                    if (parts.size() >= 3)
                        kick_reason_ = parts[2];
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

    std::atomic<bool> auth_required_{ false };
    std::atomic<bool> auth_resolved_{ false };
    std::atomic<bool> auth_ok_{ false };
    std::atomic<bool> kicked_{ false };
    std::atomic<bool> banned_{ false };
    std::string       kick_reason_;
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

// =====================================================
// Main
// =====================================================

int main()
{
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
    // Nickname prompt
    // =========================================

    echo();

    char nick_buf[NICKNAME_BUF_SIZE];
    erase();
    box(stdscr, 0, 0);
    mvprintw(2, 4, "Enter nickname:");
    move(3, 4);
    getnstr(nick_buf, NICK_MAX_LEN);
    g_nickname = nick_buf;

    noecho();

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

            try
            {
                int port = std::stoi(port_buf);
                if (port < 0 || port > 65535)
                {
                    erase(); box(stdscr, 0, 0);
                    mvprintw(2, 4, "Invalid port number (0-65535)");
                    refresh();
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    continue;
                }

                server = std::make_unique<ChatServer>(
                    io,
                    static_cast<uint16_t>(port),
                    room_password);

                add_user(g_nickname);
                push_message("[system] Hosting on port " + std::to_string(port));

                if (!room_password.empty())
                    push_message("[system] Room password protection enabled");

                // UPnP
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
                int port = std::stoi(port_buf);
                if (port < 0 || port > 65535)
                {
                    erase(); box(stdscr, 0, 0);
                    mvprintw(2, 4, "Invalid port number (0-65535)");
                    refresh();
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    continue;
                }

                client = std::make_unique<ChatClient>(io);

                if (!client->connect(host_buf, static_cast<uint16_t>(port)))
                {
                    mvprintw(10, 4, "Connection failed.");
                    refresh();
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    client.reset();
                    continue;
                }

                // Start network thread early so async reads can progress
                std::thread net_tmp([&] { io.run(); });
                net_tmp.detach(); // temporary; we re-join below

                // Wait briefly to see if server demands auth
                std::this_thread::sleep_for(std::chrono::milliseconds(300));

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
                        io.stop();
                        client.reset();
                        io.restart();
                        continue;
                    }
                }

                // Check ban before joining
                client->send("JOIN|" + g_nickname);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));

                if (client->was_banned())
                {
                    erase(); box(stdscr, 0, 0);
                    mvprintw(2, 4, "You are banned from this server.");
                    refresh();
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    io.stop();
                    client.reset();
                    io.restart();
                    continue;
                }

                add_user(g_nickname);
                push_message("[system] Connected to " + std::string(host_buf));
                configured = true;

                // Network thread already running (detached above); we don't rejoin here.
                // The main chat loop starts immediately.

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
                                    std::lock_guard lock(g_mutex);
                                    for (const auto& u : g_users)
                                        push_message("[system]   - " + u);
                                }
                                else if (cmd->name == "clear")
                                {
                                    std::lock_guard lock(g_mutex);
                                    g_messages.clear();
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
                client->send("LEAVE|" + g_nickname);

                std::lock_guard window_lock(g_window_mutex);
                if (g_users_win) delwin(g_users_win);
                if (g_chat_win)  delwin(g_chat_win);
                if (g_input_win) delwin(g_input_win);

                endwin();
                io.stop();
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
                        std::lock_guard lock(g_mutex);
                        for (const auto& u : g_users)
                            push_message("[system]   - " + u);
                    }
                    else if (cmd->name == "clear")
                    {
                        std::lock_guard lock(g_mutex);
                        g_messages.clear();
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
                                push_message("[PRIVATE " + timestamp() + "] " + g_nickname + " -> " + target + ": " + message);
                            }
                        }
                    }
                    // ---- KICK (host only) ----
                    else if (cmd->name == "kick" && server && cmd->args.size() >= 1)
                    {
                        std::string nick = cmd->args[0];
                        std::string reason = cmd->args.size() >= 2 ? cmd->args[1] : "";
                        if (!server->kick_user(nick, reason))
                            push_message("[system] User '" + nick + "' not found");
                    }
                    // ---- BAN (host only) ----
                    else if (cmd->name == "ban" && server && cmd->args.size() >= 1)
                    {
                        std::string nick = cmd->args[0];
                        std::string reason = cmd->args.size() >= 2 ? cmd->args[1] : "";
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
                        auto& bans = server->ban_list();
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