#include "dedicated_server.h"

#include "chat_log.h"
#include "chat_server.h"
#include "storage.h"
#include "upnp_mapper.h"
#include "utils.h"

void show_dedicated_usage()
{
    std::cout
        << "Usage:\n"
        << "  chatbox --server <port> [password] [logfile] [options]\n"
        << "  chatbox --dedicated <port> [password] [logfile] [options]\n"
        << "  chatbox --browser\n"
        << "  chatbox --browse <browser-host>\n\n"
        << "Options:\n"
        << "  --password <password>  - set the server password\n"
        << "  --log <file>           - write logs to a file\n"
        << "  --database <path>      - SQLite database (default chatbox.db)\n"
        << "  --history-limit <n>    - in-memory/join history size (1-500)\n"
        << "  --import-bans <file>   - one-time import from a legacy bans file\n"
        << "  --import-identities <file> - one-time import from a legacy identity file\n"
        << "  --tls-cert <path>      - PEM certificate chain\n"
        << "  --tls-key <path>       - PEM certificate private key\n"
        << "  --require-tls          - require TLS (the default)\n"
        << "  --allow-plaintext      - explicitly run without TLS\n"
        << "  --publish <host>       - publish to a browser server on port 2727\n"
        << "  --browser-plaintext    - explicitly publish to a plaintext browser\n"
        << "  --browser-tls-ca <path> - CA file for browser TLS\n"
        << "  --browser-trust-fingerprint <sha256> - pin browser certificate\n"
        << "  --browser-trust-store <path> - browser TOFU fingerprint file\n"
        << "  --name <name>          - room name shown in the server browser\n"
        << "  --public-host <host>   - host/address clients should connect to from browser results\n"
        << "  --log-stdout           - print logs only to stdout\n"
        << "  --no-log               - disable chat logging\n"
        << "  --no-upnp              - skip UPnP port forwarding\n\n"
        << "Dedicated server admin commands:\n"
        << "  /rooms                 - list rooms and room-local user counts\n"
        << "  /create <room>         - create a room\n"
        << "  /users <room>          - list users in one room\n"
        << "  /topic <room> <text>   - set a room topic\n"
        << "  /kick <room> <nick> [reason] - kick a user from one room\n"
        << "  /ban  <nick> [reason]  - ban a user server-wide and persist it\n"
        << "  /unban <nick>          - remove a persisted ban\n"
        << "  /bans                  - list banned nicknames\n"
        << "  /broadcast <msg>       - announce to every room\n"
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

bool parse_browser_address(const std::string& text, std::string& host)
{
    host = sanitize_browser_field(text, HOST_MAX_LEN);
    if (host.empty())
        return false;

    if (host.find(':') != std::string::npos)
        return false;
    return true;
}

std::string browser_entry_summary(const BrowserEntry& entry)
{
    auto age = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now() - entry.updated_at).count();

    return entry.name + " | " + entry.host + ":" + std::to_string(entry.port)
        + " | users " + std::to_string(entry.users)
        + (entry.has_password ? " | password" : " | open")
        + (entry.uses_tls ? " | TLS" : " | plaintext")
        + " | seen " + std::to_string(std::max<long long>(0, age)) + "s ago";
}

void show_browser_usage()
{
    std::cout
        << "Server browser commands:\n"
        << "  /servers               - list published servers\n"
        << "  /quit                  - shut down the browser server\n"
        << "  /help                  - show this help\n";
}

enum class ConsoleExitReason
{
    QuitRequested,
    InputClosed
};

ConsoleExitReason browser_console(ServerBrowser& browser, std::atomic<bool>& quit_flag)
{
    show_browser_usage();

    std::string line;
    while (!quit_flag && std::getline(std::cin, line))
    {
        if (line == "/quit" || line == "/exit")
        {
            quit_flag = true;
            return ConsoleExitReason::QuitRequested;
        }
        else if (line == "/help")
        {
            show_browser_usage();
        }
        else if (line == "/servers")
        {
            auto entries = browser.entries();
            if (entries.empty())
            {
                std::cout << "(no servers published)\n";
                continue;
            }

            for (const auto& entry : entries)
                std::cout << "  - " << browser_entry_summary(entry) << "\n";
        }
        else if (!line.empty())
        {
            std::cout << "Unknown command. Type /help for a list.\n";
        }
    }

    return ConsoleExitReason::InputClosed;
}

int run_browser_server(uint16_t port, chatbox::tls::ServerConfig tls_config)
{
    boost::asio::io_context io;
    const auto browser_fingerprint = tls_config.enabled
        ? chatbox::tls::certificate_sha256_fingerprint(tls_config.certificate_path)
        : std::string{};
    std::unique_ptr<ServerBrowser> browser_ptr;
    try
    {
        browser_ptr = std::make_unique<ServerBrowser>(
            io, port, std::move(tls_config));
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Could not start browser server: " << ex.what() << "\n";
        return 1;
    }
    ServerBrowser& browser = *browser_ptr;

    std::cout << "chatbox server browser running on port " << port << "\n";
    if (!browser_fingerprint.empty())
        std::cout << "[browser] TLS certificate SHA-256: "
            << browser_fingerprint << "\n";
    UPnPMapper upnp;
    if (upnp.discover())
    {
        if (upnp.openPortBoth(std::to_string(port), "chatbox server browser"))
        {
            std::cout << "[browser] UPnP port mapping succeeded\n";
            if (!upnp.externalIP().empty())
                std::cout << "[browser] External address: "
                    << upnp.externalIP() << ":" << port << "\n";
        }
        else
        {
            std::cout << "[browser] UPnP mapping failed: " << upnp.lastError() << "\n";
            std::cout << "[browser] Forward port " << port
                << " manually for internet access\n";
        }
    }
    else
    {
        std::cout << "[browser] UPnP unavailable: " << upnp.lastError() << "\n";
        std::cout << "[browser] Forward port " << port
            << " manually for internet access\n";
    }

    try
    {
        tcp::resolver resolver(io);
        auto results = resolver.resolve(boost::asio::ip::host_name(), "");
        std::cout << "[browser] LAN addresses:\n";
        for (auto& result : results)
        {
            auto addr = result.endpoint().address();
            if (!addr.is_loopback())
                std::cout << "[browser]   " << addr.to_string() << ":" << port << "\n";
        }
    }
    catch (...) {}

    std::atomic<bool> quit_flag(false);
    std::thread network_thread([&] { io.run(); });

    const auto console_exit = browser_console(browser, quit_flag);

    if (console_exit == ConsoleExitReason::InputClosed && !quit_flag)
    {
        std::cout << "[browser] Console input closed; server is still running. Stop the process to exit.\n";
        network_thread.join();
        return 0;
    }

    browser.stop();
    io.stop();
    network_thread.join();
    return 0;
}

int run_browser_list(const std::string& host, uint16_t port,
    const chatbox::tls::ClientConfig& tls_config)
{
    std::vector<BrowserEntry> entries;
    std::string error;
    if (!ServerBrowserClient::list_servers(host, port, entries, error, tls_config))
    {
        std::cerr << "Could not query browser server: " << error << "\n";
        return 1;
    }

    if (entries.empty())
    {
        std::cout << "(no servers published)\n";
        return 0;
    }

    for (size_t i = 0; i < entries.size(); ++i)
        std::cout << (i + 1) << ". " << browser_entry_summary(entries[i]) << "\n";

    return 0;
}

ConsoleExitReason admin_console(ChatServer& server, std::atomic<bool>& quit_flag)
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
            return ConsoleExitReason::QuitRequested;
        }
        else if (cmd == "/help")
        {
            show_dedicated_usage();
        }
        else if (cmd == "/rooms")
        {
            const auto rooms = server.room_list();
            for (const auto& room : rooms.rooms)
                std::cout << "  - " << room.name << " ("
                    << room.user_count << ")\n";
        }
        else if (cmd == "/create" && parts.size() == 2)
        {
            std::string error;
            if (server.create_admin_room(parts[1], error))
                std::cout << "Created room: " << parts[1] << "\n";
            else
                std::cout << error << "\n";
        }
        else if (cmd == "/users" && parts.size() == 2)
        {
            std::string error;
            auto users = server.users_in_room(parts[1], error);
            if (!error.empty())
            {
                std::cout << error << "\n";
                continue;
            }
            if (users.empty())
                std::cout << "(no users in #" << parts[1] << ")\n";
            else
                for (const auto& user : users)
                    std::cout << "  - " << user << "\n";
        }
        else if (cmd == "/topic" && parts.size() >= 3)
        {
            std::string error;
            if (server.set_admin_topic(parts[1], join_args(parts, 2), error))
                std::cout << "Updated topic for #" << parts[1] << "\n";
            else
                std::cout << error << "\n";
        }
        else if (cmd == "/kick" && parts.size() >= 3)
        {
            std::string error;
            if (server.kick_user_in_room(
                parts[1], parts[2], join_args(parts, 3), error))
                std::cout << "Kicked " << parts[2] << " from #" << parts[1] << "\n";
            else
                std::cout << error << "\n";
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

    return ConsoleExitReason::InputClosed;
}

int run_dedicated_server_config(const DedicatedServerConfig& config)
{
    if (config.tls.enabled &&
        (config.tls.certificate_path.empty() || config.tls.private_key_path.empty()))
    {
        std::cerr << "TLS is required by default. Supply --tls-cert and --tls-key, "
            "or explicitly use --allow-plaintext.\n";
        return 1;
    }

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
    if (config.tls.enabled)
    {
        const auto fingerprint = chatbox::tls::certificate_sha256_fingerprint(
            config.tls.certificate_path);
        if (!fingerprint.empty())
            log_admin("[security] TLS certificate SHA-256: " + fingerprint);
    }

    boost::asio::io_context io;
    std::shared_ptr<chatbox::storage::SQLiteStorage> storage;
    try
    {
        storage = std::make_shared<chatbox::storage::SQLiteStorage>(config.database);
        const auto imported = storage->import_legacy_files(
            config.bans_file, config.identities_file);
        if (imported.bans || imported.identities)
            log_admin("[storage] Imported " + std::to_string(imported.bans)
                + " legacy ban(s) and " + std::to_string(imported.identities)
                + " identity binding(s)");
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Could not initialize server database: " << ex.what() << "\n";
        return 1;
    }

    std::unique_ptr<ChatServer> server_ptr;
    try
    {
        server_ptr = std::make_unique<ChatServer>(io, config.port,
            config.password, log.get(), "", storage, config.tls,
            config.history_limit);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Could not start server: " << ex.what() << "\n";
        return 1;
    }
    ChatServer& server = *server_ptr;

    UPnPMapper upnp;
    std::string publish_host = config.browser_public_host;
    if (config.enable_upnp && upnp.discover())
    {
        if (upnp.openPortBoth(std::to_string(config.port), "chatbox dedicated server"))
        {
            log_admin("[admin] UPnP port mapping succeeded");
            if (!upnp.externalIP().empty())
            {
                log_admin("[admin] External address: " + upnp.externalIP() + ":" + std::to_string(config.port));
                if (publish_host.empty())
                    publish_host = upnp.externalIP();
            }
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
            {
                log_admin("[admin]   " + addr.to_string() + ":" + std::to_string(config.port));
                if (publish_host.empty())
                    publish_host = addr.to_string();
            }
        }
    }
    catch (...) {}

    std::unique_ptr<ServerBrowserPublisher> publisher;
    if (config.publish_to_browser)
    {
        if (publish_host.empty())
            publish_host = "127.0.0.1";

        BrowserEntry entry;
        entry.name = config.browser_name;
        entry.host = publish_host;
        entry.port = config.port;
        entry.has_password = !config.password.empty();
        entry.uses_tls = config.tls.enabled;

        publisher = std::make_unique<ServerBrowserPublisher>(
            config.browser_host,
            BROWSER_PORT,
            entry,
            [&server] { return static_cast<int>(server.connected_users().size()); },
            config.browser_tls);

        std::string error;
        if (publisher->start(error))
        {
            log_admin("[admin] Published to server browser "
                + config.browser_host + ":" + std::to_string(BROWSER_PORT)
                + " as " + entry.host + ":" + std::to_string(entry.port));
        }
        else
        {
            log_admin("[admin] Browser publish failed: " + error);
            publisher.reset();
        }
    }

    std::cout << "chatbox dedicated server running on port " << config.port << "\n";
    std::cout << "Log: ";
    if (!config.enable_logging)
        std::cout << "disabled";
    else if (config.log_to_stdout_only)
        std::cout << "stdout only";
    else
        std::cout << config.logfile;
    std::cout << " | Database: " << config.database << "\n";
    std::cout << "Transport: " << (config.tls.enabled ? "TLS" : "PLAINTEXT (explicit)")
        << "\n";

    std::atomic<bool> quit_flag(false);
    std::thread network_thread([&] { io.run(); });

    const auto console_exit = admin_console(server, quit_flag);

    if (console_exit == ConsoleExitReason::InputClosed && !quit_flag)
    {
        log_admin("[admin] Console input closed; dedicated server is still running. Stop the process to exit.");
        network_thread.join();
        return 0;
    }

    log_admin("[admin] Shutting down");
    publisher.reset();
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
        else if (arg == "--allow-plaintext")
        {
            config.tls.enabled = false;
        }
        else if (arg == "--require-tls")
        {
            config.tls.enabled = true;
        }
        else if (arg == "--browser-plaintext")
        {
            config.browser_tls.enabled = false;
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
        else if (arg == "--database" && i + 1 < argc)
        {
            config.database = argv[++i];
        }
        else if (arg == "--history-limit" && i + 1 < argc)
        {
            try
            {
                const auto value = std::stoul(argv[++i]);
                if (value < 1 || value > chatbox::storage::SQLiteStorage::MAX_HISTORY_PAGE)
                    throw std::out_of_range("history limit");
                config.history_limit = value;
            }
            catch (...)
            {
                std::cerr << "--history-limit must be between 1 and 500\n";
                return 1;
            }
        }
        else if (arg == "--tls-cert" && i + 1 < argc)
        {
            config.tls.certificate_path = argv[++i];
            config.tls.enabled = true;
        }
        else if (arg == "--tls-key" && i + 1 < argc)
        {
            config.tls.private_key_path = argv[++i];
            config.tls.enabled = true;
        }
        else if (arg == "--import-bans" && i + 1 < argc)
        {
            config.bans_file = argv[++i];
        }
        else if (arg == "--import-identities" && i + 1 < argc)
        {
            config.identities_file = argv[++i];
        }
        else if (arg == "--identities" && i + 1 < argc)
        {
            config.identities_file = argv[++i];
        }
        else if ((arg == "--browser-tls-ca" || arg == "--tls-ca") &&
            i + 1 < argc)
        {
            config.browser_tls.ca_path = argv[++i];
        }
        else if (arg == "--browser-trust-fingerprint" && i + 1 < argc)
        {
            config.browser_tls.expected_fingerprint = argv[++i];
        }
        else if (arg == "--browser-trust-store" && i + 1 < argc)
        {
            config.browser_tls.trust_store_path = argv[++i];
        }
        else if (arg == "--publish" && i + 1 < argc)
        {
            if (!parse_browser_address(argv[++i], config.browser_host))
            {
                std::cerr << "Invalid browser address\n";
                show_dedicated_usage();
                return 1;
            }
            config.publish_to_browser = true;
        }
        else if (arg == "--name" && i + 1 < argc)
        {
            config.browser_name = sanitize_browser_field(argv[++i], BROWSER_NAME_MAX_LEN);
            if (config.browser_name.empty())
                config.browser_name = "chatbox dedicated server";
        }
        else if (arg == "--public-host" && i + 1 < argc)
        {
            config.browser_public_host = sanitize_browser_field(argv[++i], HOST_MAX_LEN);
        }
        else if (arg == "--help" || arg == "-h")
        {
            show_dedicated_usage();
            return 0;
        }
        else if (arg == "--password" || arg == "--log" || arg == "--identities"
            || arg == "--database" || arg == "--tls-cert" || arg == "--tls-key"
            || arg == "--history-limit"
            || arg == "--import-bans" || arg == "--import-identities"
            || arg == "--browser-tls-ca" || arg == "--tls-ca"
            || arg == "--browser-trust-fingerprint" || arg == "--browser-trust-store"
            || arg == "--publish" || arg == "--name" || arg == "--public-host")
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
