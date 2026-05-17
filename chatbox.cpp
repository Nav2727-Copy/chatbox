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

//needs vcpkg my beloved
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

	// Discover the IGD (router). Returns false if UPnP is unavailable.
	bool discover()
	{
		int error = 0;

		UPNPDev* devlist = upnpDiscover(
			2000,       // timeout ms
			nullptr,    // multicast interface (all)
			nullptr,    // minissdpd socket
			0,          // local port (any)
			0,          // IPv6? no
			2,          // TTL
			&error);

		if (!devlist)
		{
			lastError_ = "No UPnP devices found (err "
				+ std::to_string(error) + ")";
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
			lastError_ = "No valid IGD found (status "
				+ std::to_string(status) + ")";
			return false;
		}

		localIP_ = lanAddr;
		discovered_ = true;

		// Grab external IP while we're here
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

	// Open a single port. Call discover() first.
	bool openPort(
		const std::string& port,
		const std::string& proto = "TCP",
		const std::string& desc = "P2P Chat")
	{
		if (!discovered_)
			return false;

		// Clear any stale mapping from a previous run first
		UPNP_DeletePortMapping(
			urls_.controlURL,
			data_.first.servicetype,
			port.c_str(),
			proto.c_str(),
			nullptr);

		int r = UPNP_AddPortMapping(
			urls_.controlURL,
			data_.first.servicetype,
			port.c_str(),       // external port
			port.c_str(),       // internal port (same)
			localIP_.c_str(),   // this machine's LAN IP
			desc.c_str(),
			proto.c_str(),
			nullptr,            // remote host (any)
			"0");               // lease: 0 = indefinite

		if (r != UPNPCOMMAND_SUCCESS)
		{
			lastError_ = "AddPortMapping failed: code "
				+ std::to_string(r);
			return false;
		}

		mappings_.push_back({ port, proto });
		return true;
	}

	// Convenience: open TCP + UDP for the same port
	bool openPortBoth(
		const std::string& port,
		const std::string& desc = "P2P Chat")
	{
		bool tcp = openPort(port, "TCP", desc);
		bool udp = openPort(port, "UDP", desc);
		return tcp || udp; // succeed if at least one worked
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
constexpr int NICK_MAX_LEN = 31;
constexpr int HOST_MAX_LEN = 63;
constexpr int PORT_MAX_LEN = 15;

std::string g_nickname;

WINDOW* g_users_win = nullptr;
WINDOW* g_chat_win = nullptr;
WINDOW* g_input_win = nullptr;

int g_last_rows = 0;
int g_last_cols = 0;

std::atomic<bool> g_shutdown_requested(false);

// Base64 alphabet for encoding/decoding
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

std::vector<std::string> split(
	const std::string& s,
	char delim)
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

	if (std::find(g_users.begin(),
		g_users.end(),
		user) == g_users.end())
	{
		g_users.push_back(user);
	}
}

void remove_user(const std::string& user)
{
	std::lock_guard lock(g_mutex);

	g_users.erase(
		std::remove(g_users.begin(),
			g_users.end(),
			user),
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
	push_message("[system] /help           - Show this help message");
	push_message("[system] /users          - List all connected users");
	push_message("[system] /clear          - Clear message history");
	push_message("[system] /time           - Show current time");
	push_message("[system] /whisper <nick> - Send private message (client only)");
	push_message("[system] /exit           - Exit the application");
	push_message("[system] === End Help ===");
}

// =====================================================
// Forward declarations
// ===================================================

class ChatServer;

class ClientSession
	: public std::enable_shared_from_this<ClientSession>
{
public:
	ClientSession(
		tcp::socket socket,
		ChatServer& server)
		: socket_(std::move(socket)),
		server_(server)
	{}

	void start();

	void deliver(const std::string& msg);

private:
	void read_loop();

	tcp::socket socket_;
	ChatServer& server_;
	boost::asio::streambuf buffer_;
};

// =====================================================
// Server
// =====================================================

class ChatServer
{
public:
	ChatServer(
		boost::asio::io_context& io,
		uint16_t port)
		: acceptor_(io)
	{
		// Open a dual-stack (IPv4 + IPv6) acceptor so the server is reachable
		// over both protocols from any interface, including public IPs.
		tcp::endpoint endpoint(tcp::v6(), port);
		acceptor_.open(endpoint.protocol());

		// Disable IPv6-only mode so IPv4 clients are accepted via IPv4-mapped
		// addresses on platforms that support it (Linux, macOS, Windows).
		acceptor_.set_option(boost::asio::ip::v6_only(false));

		// Allow immediate rebind after a crash (avoids TIME_WAIT failures).
		acceptor_.set_option(tcp::acceptor::reuse_address(true));

		acceptor_.bind(endpoint);
		acceptor_.listen();
		accept_loop();
	}

	void join(std::shared_ptr<ClientSession> session)
	{
		sessions_.insert(session);
	}

	void leave(std::shared_ptr<ClientSession> session)
	{
		sessions_.erase(session);
	}

	void broadcast(const std::string& msg)
	{
		push_message(msg);

		for (auto& s : sessions_)
			s->deliver(msg);
	}

	// Send the current user list to all connected clients so their user
	// panels stay in sync after any join or leave event.
	void broadcast_users()
	{
		std::string payload = "USERS|";

		{
			std::lock_guard lock(g_mutex);

			bool first = true;

			for (const auto& u : g_users)
			{
				if (!first)
					payload += ',';

				payload += u;
				first = false;
			}
		}

		for (auto& s : sessions_)
			s->deliver(payload);
	}

private:
	void accept_loop()
	{
		acceptor_.async_accept(
			[this](
				boost::system::error_code ec,
				tcp::socket socket)
			{
				if (!ec)
				{
					auto session =
						std::make_shared<ClientSession>(
							std::move(socket),
							*this);

					join(session);

					session->start();
				}

				accept_loop();
			});
	}

	tcp::acceptor acceptor_;

	std::set<std::shared_ptr<ClientSession>>
		sessions_;
};

// =====================================================
// Client session
// =====================================================

void ClientSession::start()
{
	read_loop();
}

void ClientSession::read_loop()
{
	auto self = shared_from_this();

	boost::asio::async_read_until(
		socket_,
		buffer_,
		'\n',
		[this, self](
			boost::system::error_code ec,
			std::size_t)
		{
			if (ec)
			{
				server_.leave(self);
				return;
			}

			std::istream is(&buffer_);

			std::string line;

			std::getline(is, line);

			std::string decoded_line = decode_base64(line);

			auto parts = split(decoded_line, '|');

			if (parts.empty())
			{
				read_loop();
				return;
			}

			if (parts[0] == "JOIN"
				&& parts.size() >= 2)
			{
				add_user(parts[1]);

				server_.broadcast(
					"[system] "
					+ parts[1]
					+ " joined");

				// Push updated user list to all clients so their
				// user panel reflects the new member immediately.
				server_.broadcast_users();
			}
			else if (parts[0] == "LEAVE"
				&& parts.size() >= 2)
			{
				remove_user(parts[1]);

				server_.broadcast(
					"[system] "
					+ parts[1]
					+ " left");

				// Push updated user list after the member leaves.
				server_.broadcast_users();
			}
			else if (parts[0] == "MSG"
				&& parts.size() >= 3)
			{
				server_.broadcast(
					"["
					+ timestamp()
					+ "] "
					+ parts[1]
					+ ": "
					+ parts[2]);
			}
			else if (parts[0] == "WHISPER"
				&& parts.size() >= 4)
			{
				// WHISPER|from|to|message format
				server_.broadcast(
					"[PRIVATE "
					+ timestamp()
					+ "] "
					+ parts[1]
					+ " -> "
					+ parts[2]
					+ ": "
					+ parts[3]);
			}

			read_loop();
		});
}

void ClientSession::deliver(
	const std::string& msg)
{
	auto self = shared_from_this();

	std::string encoded_msg = encode_base64(msg);

	auto data =
		std::make_shared<std::string>(
			encoded_msg + "\n");

	boost::asio::async_write(
		socket_,
		boost::asio::buffer(*data),
		[data, self](
			boost::system::error_code ec,
			std::size_t)
		{
			if (ec)
			{
				std::cerr << "Write error: " << ec.message() << std::endl;
			}
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

	bool connect(
		const std::string& host,
		uint16_t port)
	{
		try
		{
			tcp::resolver resolver(
				socket_.get_executor());

			auto endpoints =
				resolver.resolve(
					host,
					std::to_string(port));

			boost::asio::connect(
				socket_,
				endpoints);

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
				std::string encoded_msg = encode_base64(msg);

				auto data =
					std::make_shared<std::string>(
						encoded_msg + "\n");

				boost::asio::async_write(
					socket_,
					boost::asio::buffer(*data),
					[data](
						boost::system::error_code ec,
						std::size_t)
					{
						if (ec)
						{
							std::cerr << "Send error: " << ec.message() << std::endl;
						}
					});
			});
	}

private:
	void read_loop()
	{
		boost::asio::async_read_until(
			socket_,
			buffer_,
			'\n',
			[this](
				boost::system::error_code ec,
				std::size_t)
			{
				if (ec)
				{
					push_message(
						"[system] disconnected");
					return;
				}

				std::istream is(&buffer_);

				std::string line;

				std::getline(is, line);

				std::string decoded_line = decode_base64(line);

				// The server sends "USERS|nick1,nick2,..." after every join/leave
				// so clients keep their user panel in sync.  Everything else is
				// a plain chat or system message and goes straight to the log.
				if (decoded_line.rfind("USERS|", 0) == 0)
				{
					std::string list = decoded_line.substr(6);

					std::lock_guard lock(g_mutex);

					g_users.clear();

					if (!list.empty())
					{
						auto names = split(list, ',');

						for (auto& name : names)
							g_users.push_back(name);
					}
				}
				else
				{
					push_message(decoded_line);
				}

				read_loop();
			});
	}

	tcp::socket socket_;
	boost::asio::streambuf buffer_;
};

// =====================================================
// UI
// =====================================================

void draw_ui(const std::string& input)
{
	std::lock_guard window_lock(g_window_mutex);

	int rows, cols;

	getmaxyx(stdscr, rows, cols);

	if (rows != g_last_rows
		|| cols != g_last_cols
		|| !g_users_win)
	{
		if (g_users_win)
			delwin(g_users_win);
		if (g_chat_win)
			delwin(g_chat_win);
		if (g_input_win)
			delwin(g_input_win);

		g_users_win =
			newwin(rows - INPUT_WINDOW_HEIGHT,
				USERS_WINDOW_WIDTH,
				0,
				0);

		g_chat_win =
			newwin(rows - INPUT_WINDOW_HEIGHT,
				cols - USERS_WINDOW_WIDTH,
				0,
				USERS_WINDOW_WIDTH);

		g_input_win =
			newwin(INPUT_WINDOW_HEIGHT,
				cols,
				rows - INPUT_WINDOW_HEIGHT,
				0);

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

	wattron(g_users_win, COLOR_PAIR(1));
	mvwprintw(g_users_win, 0, 2, " Users ");
	wattroff(g_users_win, COLOR_PAIR(1));

	wattron(g_chat_win, COLOR_PAIR(2));
	mvwprintw(g_chat_win, 0, 2, " Chat ");
	wattroff(g_chat_win, COLOR_PAIR(2));

	wattron(g_input_win, COLOR_PAIR(3));
	mvwprintw(g_input_win, 0, 2, " Input ");
	wattroff(g_input_win, COLOR_PAIR(3));

	{
		std::lock_guard lock(g_mutex);

		int y = 1;

		for (const auto& user : g_users)
		{
			if (y >= rows - INPUT_WINDOW_HEIGHT - 1)
				break;

			mvwprintw(g_users_win,
				y++,
				2,
				"%s",
				user.c_str());
		}

		int max_chat_lines =
			rows - INPUT_WINDOW_HEIGHT - 2;

		int start =
			std::max(
				0,
				(int)g_messages.size()
				- max_chat_lines);

		y = 1;

		for (int i = start;
			i < (int)g_messages.size();
			++i)
		{
			mvwprintw(g_chat_win,
				y++,
				2,
				"%s",
				g_messages[i].c_str());
		}
	}

	mvwprintw(g_input_win,
		1,
		2,
		"> %s",
		input.c_str());

	wnoutrefresh(g_users_win);
	wnoutrefresh(g_chat_win);
	wnoutrefresh(g_input_win);
	doupdate();
}

// =====================================================
// Main
// =====================================================

int main()
{
	boost::asio::io_context io;

	std::unique_ptr<ChatServer>  server;
	std::unique_ptr<ChatClient>  client;
	std::unique_ptr<UPnPMapper>  upnp;

	initscr();

	cbreak();
	noecho();
	keypad(stdscr, TRUE);

	start_color();

	init_pair(
		1,
		COLOR_CYAN,
		COLOR_BLACK);

	init_pair(
		2,
		COLOR_GREEN,
		COLOR_BLACK);

	init_pair(
		3,
		COLOR_YELLOW,
		COLOR_BLACK);

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

		if (ch == 'q'
			|| ch == 'Q')
		{
			endwin();
			return 0;
		}

		else if (ch == 'h'
			|| ch == 'H')
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

			try
			{
				int port = std::stoi(port_buf);

				if (port < 0 || port > 65535)
				{
					erase();
					box(stdscr, 0, 0);
					mvprintw(2, 4, "Invalid port number (0-65535)");
					refresh();
					std::this_thread::sleep_for(std::chrono::seconds(2));
					continue;
				}

				server =
					std::make_unique<ChatServer>(
						io,
						static_cast<uint16_t>(port));

				add_user(g_nickname);

				push_message(
					"[system] Hosting on port "
					+ std::to_string(port));

				// --- UPnP: try to open the port on the router automatically ---
				upnp = std::make_unique<UPnPMapper>();

				push_message("[system] Attempting UPnP port mapping...");

				if (upnp->discover())
				{
					if (upnp->openPortBoth(std::to_string(port)))
					{
						push_message(
							"[system] UPnP OK - port "
							+ std::to_string(port)
							+ " opened on router");

						if (!upnp->externalIP().empty())
						{
							push_message(
								"[system] External address: "
								+ upnp->externalIP()
								+ ":" + std::to_string(port));

							push_message(
								"[system] Share that address with your peer");
						}
					}
					else
					{
						push_message(
							"[system] UPnP discovered router but mapping failed: "
							+ upnp->lastError());
						push_message(
							"[system] You may need to forward port "
							+ std::to_string(port)
							+ " manually");
					}
				}
				else
				{
					push_message(
						"[system] UPnP unavailable: "
						+ upnp->lastError());
					push_message(
						"[system] Forward port "
						+ std::to_string(port)
						+ " manually for internet access");
				}

				// Also show local LAN addresses as a fallback
				try
				{
					tcp::resolver resolver(io);
					auto results = resolver.resolve(
						boost::asio::ip::host_name(), "");

					push_message("[system] LAN addresses:");

					for (auto& r : results)
					{
						auto addr = r.endpoint().address();

						if (!addr.is_loopback())
						{
							push_message(
								"[system]   "
								+ addr.to_string()
								+ ":" + std::to_string(port));
						}
					}
				}
				catch (...) {}

				configured = true;
			}
			catch (const std::exception& e)
			{
				erase();
				box(stdscr, 0, 0);
				mvprintw(2, 4, "Invalid port number");
				refresh();
				std::this_thread::sleep_for(std::chrono::seconds(2));
				continue;
			}
		}

		else if (ch == 'j'
			|| ch == 'J')
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
					erase();
					box(stdscr, 0, 0);
					mvprintw(2, 4, "Invalid port number (0-65535)");
					refresh();
					std::this_thread::sleep_for(std::chrono::seconds(2));
					continue;
				}

				client =
					std::make_unique<ChatClient>(
						io);

				if (!client->connect(
					host_buf,
					static_cast<uint16_t>(port)))
				{
					mvprintw(
						10,
						4,
						"Connection failed.");

					refresh();

					std::this_thread::sleep_for(
						std::chrono::seconds(2));

					continue;
				}

				client->send(
					"JOIN|" + g_nickname);

				add_user(g_nickname);

				push_message(
					"[system] Connected to "
					+ std::string(host_buf));

				configured = true;
			}
			catch (const std::exception& e)
			{
				erase();
				box(stdscr, 0, 0);
				mvprintw(2, 4, "Invalid port number");
				refresh();
				std::this_thread::sleep_for(std::chrono::seconds(2));
				continue;
			}
		}
	}

	timeout(30);

	std::thread network_thread(
		[&]
		{
			io.run();
		});

	// =========================================
	// Chat loop
	// =========================================

	std::string input;

	bool running = true;

	while (running)
	{
		draw_ui(input);

		int ch = getch();

		switch (ch)
		{
		case ERR:
			break;

		case 27:
			running = false;
			break;

		case '\n':
		{
			if (!input.empty())
			{
				// Check if it's a command
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
						{
							std::lock_guard lock(g_mutex);
							for (const auto& user : g_users)
								push_message("[system]   - " + user);
						}
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
						// Reconstruct message from remaining input
						size_t cmd_end = input.find(' ');
						if (cmd_end != std::string::npos)
						{
							cmd_end = input.find(' ', cmd_end + 1);
							if (cmd_end != std::string::npos)
							{
								std::string target = cmd->args[0];
								std::string message = input.substr(cmd_end + 1);

								std::string msg =
									"WHISPER|"
									+ g_nickname
									+ "|"
									+ target
									+ "|"
									+ message;

								if (client)
									client->send(msg);
								else
									push_message("[system] Whisper only works in client mode");
							}
						}
					}
					else if (cmd->name == "exit")
					{
						running = false;
					}
					else
					{
						push_message("[system] Unknown command: " + cmd->name + ". Type /help for available commands.");
					}
				}
				else
				{
					// Regular message
					std::string msg =
						"MSG|"
						+ g_nickname
						+ "|"
						+ input;

					if (client)
					{
						client->send(msg);
					}
					else if (server)
					{
						server->broadcast(
							"["
							+ timestamp()
							+ "] "
							+ g_nickname
							+ ": "
							+ input);
					}
				}

				input.clear();
			}

			break;
		}

		case KEY_BACKSPACE:
		case 127:
		case 8:
			if (!input.empty())
				input.pop_back();
			break;

		default:
			if (isprint(ch))
			{
				input +=
					static_cast<char>(ch);
			}

			break;
		}
	}

	// =========================================
	// Shutdown
	// =========================================

	g_shutdown_requested = true;

	if (client)
	{
		client->send(
			"LEAVE|" + g_nickname);
	}

	// Remove the UPnP port mapping before tearing down the network.
	// The destructor does this automatically, but we reset explicitly
	// here so it runs while io_context is still live.
	upnp.reset();

	{
		std::lock_guard window_lock(g_window_mutex);

		if (g_users_win)
			delwin(g_users_win);
		if (g_chat_win)
			delwin(g_chat_win);
		if (g_input_win)
			delwin(g_input_win);
	}

	endwin();

	io.stop();

	network_thread.join();

	return 0;
}