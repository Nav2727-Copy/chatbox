#include "curses_app.h"

#include "app_state.h"
#include "chat_client.h"
#include "chat_server.h"
#include "commands.h"
#include "interactive_app.h"
#include "dedicated_server.h"
#include "server_browser.h"
#include "curses_ui.h"
#include "upnp_mapper.h"
#include "utils.h"

#include <curses.h>

int run_curses_app()
{
    boost::asio::io_context io;

    std::unique_ptr<ChatServer> server;
    std::unique_ptr<ChatClient> client;
    std::unique_ptr<UPnPMapper> upnp;
    std::unique_ptr<ServerBrowserPublisher> browser_publisher;

    initialize_curses_ui();

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
        mvprintw(6, 4, "[B] Browser server");
        mvprintw(7, 4, "[Q] Quit");
        refresh();

        int ch = getch();

        if (ch == 'q' || ch == 'Q')
        {
            shutdown_curses_ui();
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
            std::string room_password = prompt_curses_password(7, 4,
                "Room password (leave blank for none):");

            bool enable_upnp = prompt_curses_yes_no(10, 4, "Enable UPnP port forwarding? [Y/n]:");

            echo();
            mvprintw(12, 4, "Log file (blank chatlog.txt, 'none' disables, 'stdout' console only):");
            move(13, 4);
            getnstr(logfile_buf, LOGFILE_MAX_LEN);
            noecho();

            bool publish_to_browser = prompt_curses_yes_no(15, 4, "Publish to a browser server on port 2727? [y/N]:", false);
            char browser_host_buf[HOST_BUF_SIZE] = {};
            char browser_name_buf[HOST_BUF_SIZE] = {};
            char public_host_buf[HOST_BUF_SIZE] = {};
            if (publish_to_browser)
            {
                echo();
                mvprintw(17, 4, "Browser server IP:");
                move(18, 4);
                getnstr(browser_host_buf, HOST_MAX_LEN);
                mvprintw(20, 4, "Room name (blank default):");
                move(21, 4);
                getnstr(browser_name_buf, HOST_MAX_LEN);
                mvprintw(23, 4, "Public IP/address clients should use (blank auto):");
                move(24, 4);
                getnstr(public_host_buf, HOST_MAX_LEN);
                noecho();
            }

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
            config.publish_to_browser = publish_to_browser;

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

            if (publish_to_browser)
            {
                if (!parse_browser_address(browser_host_buf, config.browser_host))
                {
                    erase();
                    box(stdscr, 0, 0);
                    mvprintw(2, 4, "Enter only the browser server host/IP, without a port.");
                    refresh();
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    continue;
                }

                if (std::string(browser_name_buf).empty())
                    config.browser_name = "chatbox dedicated server";
                else
                    config.browser_name = sanitize_browser_field(browser_name_buf, BROWSER_NAME_MAX_LEN);
                config.browser_public_host = sanitize_browser_field(public_host_buf, HOST_MAX_LEN);
            }

            shutdown_curses_ui();
            return run_dedicated_server_config(config);
        }
        else if (ch == 'b' || ch == 'B')
        {
            erase();
            box(stdscr, 0, 0);
            mvprintw(2, 4, "Starting browser server on port %d...", BROWSER_PORT);
            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            shutdown_curses_ui();
            return run_browser_server(BROWSER_PORT);
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

    ClientIdentity local_identity;
    bool identity_created = false;
    if (!load_or_create_identity(g_nickname, local_identity, identity_created))
    {
        shutdown_curses_ui();
        std::cerr << "Could not load or create local identity key for " << g_nickname << "\n";
        return 1;
    }
    push_message("[system] " + std::string(identity_created ? "Created" : "Loaded")
        + " local identity " + identity_fingerprint(local_identity.public_key_hex));

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
        mvprintw(6, 4, "[B] Browse servers");
        mvprintw(7, 4, "[Q] Quit");
        refresh();

        int ch = getch();

        if (ch == 'q' || ch == 'Q')
        {
            shutdown_curses_ui();
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
            std::string room_password = prompt_curses_password(7, 4,
                "Room password (leave blank for none):");

            bool enable_upnp = prompt_curses_yes_no(10, 4, "Enable UPnP port forwarding? [Y/n]:");

            bool publish_to_browser = prompt_curses_yes_no(12, 4, "Publish to a browser server on port 2727? [y/N]:", false);
            char browser_host_buf[HOST_BUF_SIZE] = {};
            char browser_name_buf[HOST_BUF_SIZE] = {};
            char public_host_buf[HOST_BUF_SIZE] = {};
            if (publish_to_browser)
            {
                echo();
                mvprintw(14, 4, "Browser server IP:");
                move(15, 4);
                getnstr(browser_host_buf, HOST_MAX_LEN);
                mvprintw(17, 4, "Room name (blank uses nickname):");
                move(18, 4);
                getnstr(browser_name_buf, HOST_MAX_LEN);
                mvprintw(20, 4, "Public IP/address clients should use (blank auto):");
                move(21, 4);
                getnstr(public_host_buf, HOST_MAX_LEN);
                noecho();
            }

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

                std::string browser_host = browser_host_buf;
                if (publish_to_browser)
                {
                    if (!parse_browser_address(browser_host_buf, browser_host))
                    {
                        erase(); box(stdscr, 0, 0);
                        mvprintw(2, 4, "Enter only the browser server host/IP, without a port.");
                        refresh();
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        continue;
                    }
                }

                server = std::make_unique<ChatServer>(
                    io,
                    port,
                    room_password,
                    nullptr,
                    g_nickname);
                server->load_identities("identities.txt");
                if (!server->register_local_identity(g_nickname, local_identity.public_key_hex))
                {
                    erase(); box(stdscr, 0, 0);
                    mvprintw(2, 4, "Your nickname is registered to another identity key.");
                    refresh();
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                    server.reset();
                    continue;
                }

                add_user(g_nickname);
                push_message("[system] Hosting on port " + std::to_string(port));
                push_message("[system] Your identity: " + identity_fingerprint(local_identity.public_key_hex));

                if (!room_password.empty())
                    push_message("[system] Room password protection enabled");

                std::string publish_host = sanitize_browser_field(public_host_buf, HOST_MAX_LEN);

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
                                if (publish_host.empty())
                                    publish_host = upnp->externalIP();
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
                        {
                            push_message("[system]   " + addr.to_string() + ":" + std::to_string(port));
                            if (publish_host.empty())
                                publish_host = addr.to_string();
                        }
                    }
                }
                catch (...) {}

                if (publish_to_browser)
                {
                    if (publish_host.empty())
                        publish_host = "127.0.0.1";

                    BrowserEntry entry;
                    entry.name = std::string(browser_name_buf).empty()
                        ? g_nickname + "'s room"
                        : sanitize_browser_field(browser_name_buf, BROWSER_NAME_MAX_LEN);
                    entry.host = publish_host;
                    entry.port = port;
                    entry.has_password = !room_password.empty();

                    browser_publisher = std::make_unique<ServerBrowserPublisher>(
                        browser_host,
                        BROWSER_PORT,
                        entry,
                        [] { return static_cast<int>(connected_users().size()); });

                    std::string error;
                    if (browser_publisher->start(error))
                    {
                        push_message("[system] Published to browser "
                            + browser_host + ":" + std::to_string(BROWSER_PORT)
                            + " as " + entry.host + ":" + std::to_string(entry.port));
                    }
                    else
                    {
                        push_message("[system] Browser publish failed: " + error);
                        browser_publisher.reset();
                    }
                }

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

        // ---- JOIN / BROWSE ----
        else if (ch == 'j' || ch == 'J' || ch == 'b' || ch == 'B')
        {
            std::string selected_host;
            uint16_t selected_port = 0;

            if (ch == 'b' || ch == 'B')
            {
                echo();
                char browser_host_buf[HOST_BUF_SIZE] = {};

                erase();
                box(stdscr, 0, 0);
                mvprintw(2, 4, "Browse Servers");
                mvprintw(4, 4, "Browser server IP:");
                move(5, 4);
                getnstr(browser_host_buf, HOST_MAX_LEN);
                noecho();

                std::string browser_host;
                if (!parse_browser_address(browser_host_buf, browser_host))
                {
                    erase(); box(stdscr, 0, 0);
                    mvprintw(2, 4, "Enter only the browser server host/IP, without a port.");
                    refresh();
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    continue;
                }

                std::vector<BrowserEntry> entries;
                std::string error;
                if (!ServerBrowserClient::list_servers(browser_host, BROWSER_PORT, entries, error))
                {
                    erase(); box(stdscr, 0, 0);
                    mvprintw(2, 4, "Could not query browser: %s", error.c_str());
                    refresh();
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                    continue;
                }

                if (entries.empty())
                {
                    erase(); box(stdscr, 0, 0);
                    mvprintw(2, 4, "No servers are published there right now.");
                    refresh();
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    continue;
                }

                erase();
                box(stdscr, 0, 0);
                mvprintw(2, 4, "Published Servers");
                int row = 4;
                for (size_t i = 0; i < entries.size() && row < LINES - 3; ++i)
                {
                    mvprintw(row++, 4, "%zu) %s", i + 1, browser_entry_summary(entries[i]).c_str());
                }
                echo();
                char choice_buf[PORT_BUF_SIZE] = {};
                mvprintw(row + 1, 4, "Choose number:");
                move(row + 2, 4);
                getnstr(choice_buf, PORT_MAX_LEN);
                noecho();

                int choice = 0;
                try { choice = std::stoi(choice_buf); }
                catch (...) { choice = 0; }

                if (choice < 1 || choice > static_cast<int>(entries.size()))
                {
                    erase(); box(stdscr, 0, 0);
                    mvprintw(2, 4, "Invalid server selection.");
                    refresh();
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    continue;
                }

                selected_host = entries[choice - 1].host;
                selected_port = entries[choice - 1].port;
            }
            else
            {
                echo();

                char host_buf[HOST_BUF_SIZE] = {};
                char port_buf[PORT_BUF_SIZE] = {};

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

                selected_host = host_buf;
                if (!parse_port(port_buf, selected_port))
                {
                    erase(); box(stdscr, 0, 0);
                    mvprintw(2, 4, "Invalid port number (0-65535)");
                    refresh();
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    continue;
                }
            }

            try
            {
                client = std::make_unique<ChatClient>(io);
                client->set_identity(local_identity);

                if (!client->connect(selected_host, selected_port))
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
                    std::string pw = prompt_curses_password(10, 4, "Server password:");
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

                // Prove nickname identity before joining.
                client->identify(g_nickname);
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
                push_message("[system] Connected to " + selected_host + ":" + std::to_string(selected_port));
                push_message("[system] Your identity: " + identity_fingerprint(local_identity.public_key_hex));
                configured = true;

                timeout(30);

                std::string input;
                bool running = true;

                while (running)
                {
                    if (g_kicked)
                    {
                        push_message("[system] Press any key to exit...");
                        draw_curses_chat_ui(input);
                        std::this_thread::sleep_for(std::chrono::seconds(3));
                        running = false;
                        break;
                    }

                    draw_curses_chat_ui(input);
                    int c = getch();

                    switch (c)
                    {
                    case ERR: break;
                    case 27:  running = false; break;

                    case '\n':
                    {
                        if (!input.empty())
                        {
                            ChatInputContext context;
                            context.client = client.get();
                            context.local_nickname = g_nickname;
                            if (handle_chat_input(input, context) == ChatInputResult::Exit)
                                running = false;

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
                    client->send("LEAVE");
                client->close();
                if (network_thread.joinable())
                    network_thread.join();

                shutdown_curses_ui();
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
        draw_curses_chat_ui(input);
        int ch = getch();

        switch (ch)
        {
        case ERR: break;
        case 27:  running = false; break;

        case '\n':
        {
            if (!input.empty())
            {
                ChatInputContext context;
                context.server = server.get();
                context.local_nickname = g_nickname;
                context.host_commands_enabled = true;
                if (handle_chat_input(input, context) == ChatInputResult::Exit)
                    running = false;

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
    browser_publisher.reset();
    upnp.reset();

    shutdown_curses_ui();
    io.stop();
    network_thread.join();
    return 0;
}

int run_interactive_app()
{
    return run_curses_app();
}
