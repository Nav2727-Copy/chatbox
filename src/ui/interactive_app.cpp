#include "ui/curses_app.h"
#include "ui/startup_menu.h"

#include "app/app_state.h"
#include "chat/chat_client.h"
#include "chat/chat_server.h"
#include "app/commands.h"
#include "ui/interactive_app.h"
#include "app/dedicated_server.h"
#include "network/server_browser.h"
#include "ui/curses_ui.h"
#include "ui/setup_forms.h"
#include "ui/display_text.h"
#include "network/upnp_mapper.h"
#include "common/utils.h"

#include <curses.h>

namespace
{
// The caller keeps the network thread running throughout authentication and joining.
std::optional<std::string> join_client_session(ChatClient& client,
    const std::string& nickname)
{
    for (int i = 0; i < 20 && !client.auth_required() && !client.auth_resolved(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (!client.auth_required() && !client.auth_resolved())
        return "Protocol handshake timed out.";

    if (client.auth_required())
    {
        std::string password;
        try { password = prompt_curses_password(10, 4, "Password"); }
        catch (const CursesFormCancelled&) { return "Joining was cancelled."; }
        client.send(chatbox::protocol::ClientMessage{
            chatbox::protocol::Authenticate{ password } });

        show_curses_notice("Joining server", "Checking your password...", false);

        for (int i = 0; i < 50 && !client.auth_resolved(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (!client.auth_resolved())
            return "Authentication timed out.";
        if (!client.auth_ok())
            return "Authentication failed. Wrong password.";
    }

    if (!client.auth_ok())
        return "Server rejected authentication or the protocol handshake.";

    client.identify(nickname);
    for (int i = 0; i < 50 && !client.join_resolved(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (!client.join_resolved())
        return "Joining the server timed out.";

    if (client.was_banned())
        return "You are banned from this server.";
    if (client.nickname_taken())
        return "Nickname is already in use.";
    if (client.join_failed())
        return "Join failed: " + client.join_failure_reason();

    return std::nullopt;
}

void close_client_session(ChatClient& client, std::thread& network_thread)
{
    client.close();
    if (network_thread.joinable())
        network_thread.join();
}
}

int run_curses_app()
{
    boost::asio::io_context io;

    std::unique_ptr<ChatServer> server;
    std::unique_ptr<ChatClient> client;
    std::unique_ptr<UPnPMapper> upnp;
    std::unique_ptr<ServerBrowserPublisher> browser_publisher;

    reset_chat_state();
    initialize_curses_ui();

    if (auto exit_code = run_startup_menu())
        return *exit_code;

    // =========================================
    // Nickname prompt
    // =========================================

    using namespace chatbox::ui;
    namespace display = chatbox::ui::ui_text;
    std::string nickname;
    try
    {
        std::vector<CursesField> fields = {
            { display::nickname, "", NICK_MAX_LEN, false, false, [](const std::string& value) {
                return is_valid_nickname(value) ? std::string{}
                    : display::nickname_error;
            } }
        };
        edit_curses_form(display::nickname_prompt, display::nickname_description, fields);
        nickname = fields[0].value;
    }
    catch (const CursesFormCancelled&) { shutdown_curses_ui(); return 0; }

    ClientIdentity local_identity;
    bool identity_created = false;
    if (!load_or_create_identity(nickname, local_identity, identity_created))
    {
        shutdown_curses_ui();
        std::cerr << "Could not load or create local identity key for " << nickname << "\n";
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
        const int choice = choose_curses_menu(std::string(display::hello_name) + nickname + ".", display::where_to_go, {
            { 'j', display::join_server, display::join_server_detail },
            { 'h', display::host_conversation, display::host_detail },
            { 'b', display::discover_servers, display::discover_detail_main },
            { 'q', display::quit, display::close_chatbox }
        });
        const int ch = choice < 0 ? 'q' : std::string("jhbq")[choice];
        try
        {
            if (ch == 'q' || ch == 'Q')
            {
                shutdown_curses_ui();
                return 0;
            }

            // ---- HOST ----
            else if (ch == 'h' || ch == 'H')
            {
                std::vector<CursesField> fields = {
                    port_field(), { "Password (optional)", "", PASS_MAX_LEN, true },
                    { "Automatic port forwarding (UPnP)", "yes", 3, false, true },
                    { "Publish to a server directory", "no", 3, false, true },
                    { "Use TLS", "yes", 3, false, true }
                };
                edit_curses_form("Host a conversation", "Choose how people can join your server.", fields);
                const auto& port_buf = fields[0].value;
                const auto& room_password = fields[1].value;
                const bool enable_upnp = fields[2].value == "yes";
                const bool publish_to_browser = fields[3].value == "yes";
                chatbox::tls::ServerConfig host_tls;
                host_tls.enabled = fields[4].value == "yes";
                configure_tls(host_tls);
                PublicationSettings publication;
                if (publish_to_browser) publication = configure_publication();
                const auto& browser_host_buf = publication.host;
                const auto& browser_name_buf = publication.name;
                const auto& public_host_buf = publication.public_host;
                show_curses_notice("Starting your server", "Preparing your room and network connection...", false);

                try
                {
                    uint16_t port = 0;
                    if (!parse_port(port_buf, port))
                    {
                        show_curses_notice("Could not continue", "Invalid port number (0-65535)");
                        continue;
                    }

                    std::string browser_host = browser_host_buf;
                    if (publish_to_browser)
                    {
                        if (!parse_browser_address(browser_host_buf, browser_host))
                        {
                            show_curses_notice("Could not continue", "Enter only the browser server host/IP, without a port.");
                            continue;
                        }
                    }

                    auto host_storage = std::make_shared<chatbox::storage::SQLiteStorage>(
                        chatbox_database_path());
                    host_storage->import_legacy_files("bans.txt", "identities.txt");
                    server = std::make_unique<ChatServer>(
                        io,
                        port,
                        room_password,
                        nullptr,
                        nickname,
                        host_storage,
                        host_tls);
                    if (!server->register_local_identity(nickname, local_identity.public_key_hex))
                    {
                        show_curses_notice("Could not continue", "Your nickname is registered to another identity key.");
                        server.reset();
                        continue;
                    }

                    server->start_local_user();
                    push_message("[system] Hosting on port " + std::to_string(port));
                    push_message("[system] Your identity: " + identity_fingerprint(local_identity.public_key_hex));

                    if (!room_password.empty())
                        push_message("[system] Server password protection enabled");

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
                            ? nickname + "'s room"
                            : sanitize_browser_field(browser_name_buf, BROWSER_NAME_MAX_LEN);
                        entry.host = publish_host;
                        entry.port = port;
                        entry.has_password = !room_password.empty();
                        entry.uses_tls = host_tls.enabled;

                        browser_publisher = std::make_unique<ServerBrowserPublisher>(
                            browser_host,
                            BROWSER_PORT,
                            entry,
                            [&server] {
                                return server
                                    ? static_cast<int>(server->connected_users().size())
                                    : 0;
                            });

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
                catch (const std::exception& error)
                {
                    show_curses_notice("Connection failed", error.what());
                    continue;
                }
            }

            // ---- JOIN / BROWSE ----
            else if (ch == 'j' || ch == 'J' || ch == 'b' || ch == 'B')
            {
                std::string selected_host;
                uint16_t selected_port = 0;
                chatbox::tls::ClientConfig client_tls;

                if (ch == 'b' || ch == 'B')
                {
                    std::vector<CursesField> fields = {
                        required_field("Directory host", "", HOST_MAX_LEN),
                        { "Use TLS", "yes", 3, false, true }
                    };
                    edit_curses_form("Discover servers", "Connect to a server directory to see who is online.", fields);
                    const auto& browser_host_buf = fields[0].value;
                    client_tls.enabled = fields[1].value == "yes";
                    show_curses_notice("Finding servers", "Loading the server directory...", false);

                    std::string browser_host;
                    if (!parse_browser_address(browser_host_buf, browser_host))
                    {
                        show_curses_notice("Could not continue", "Enter only the browser server host/IP, without a port.");
                        continue;
                    }

                    std::vector<BrowserEntry> entries;
                    std::string error;
                    if (!ServerBrowserClient::list_servers(browser_host, BROWSER_PORT,
                        entries, error, client_tls))
                    {
                        show_curses_notice("Directory unavailable", error);
                        continue;
                    }

                    if (entries.empty())
                    {
                        show_curses_notice("Could not continue", "No servers are published there right now.");
                        continue;
                    }

                    std::vector<CursesMenuItem> servers;
                    for (const auto& entry : entries)
                        servers.push_back({ 0, entry.name, browser_entry_summary(entry) });
                    const int selected = choose_curses_menu("Discover servers", "Select a server to join.", servers);
                    if (selected < 0) continue;
                    const int choice = selected + 1;

                    selected_host = entries[choice - 1].host;
                    selected_port = entries[choice - 1].port;
                    client_tls.enabled = entries[choice - 1].uses_tls;
                }
                else
                {
                    std::vector<CursesField> fields = {
                        required_field("Host address", "localhost", HOST_MAX_LEN),
                        port_field(), { "Use TLS", "yes", 3, false, true }
                    };
                    edit_curses_form("Join a server", "Enter the address shared by your host.", fields);
                    selected_host = fields[0].value;
                    parse_port(fields[1].value, selected_port);
                    client_tls.enabled = fields[2].value == "yes";
                }

                show_curses_notice("Joining server", "Connecting to " + selected_host + ":" + std::to_string(selected_port) + "...", false);
                try
                {
                    client = std::make_unique<ChatClient>(io, client_tls);
                    client->set_identity(local_identity);

                    if (!client->connect(selected_host, selected_port))
                    {
                        show_curses_notice("Connection failed", client->connect_error());
                        client.reset();
                        continue;
                    }

                    // Start network thread early so async reads can progress.
                    std::thread network_thread([&] { io.run(); });

                    if (auto error = join_client_session(*client, nickname))
                    {
                        show_curses_notice("Could not join", *error);
                        close_client_session(*client, network_thread);
                        client.reset();
                        io.restart();
                        continue;
                    }

                    push_message("[system] Connected to " + selected_host + ":" + std::to_string(selected_port));
                    push_message("[system] Your identity: " + identity_fingerprint(local_identity.public_key_hex));
                    configured = true;

                    ChatInputContext context;
                    context.client = client.get();
                    context.local_nickname = nickname;
                    run_curses_chat_loop(context);

                    // Shutdown client
                    if (!was_kicked())
                        client->send(chatbox::protocol::ClientMessage{
                            chatbox::protocol::Disconnect{} });
                    close_client_session(*client, network_thread);

                    shutdown_curses_ui();
                    return 0;
                }
                catch (const std::exception& error)
                {
                    show_curses_notice("Connection failed", error.what());
                    continue;
                }
            }
        }
        catch (const CursesFormCancelled&) { continue; }
    }

    // =========================================
    // Host: run chat loop
    // =========================================

    std::thread network_thread([&] { io.run(); });

    ChatInputContext context;
    context.server = server.get();
    context.local_nickname = nickname;
    context.host_commands_enabled = true;
    run_curses_chat_loop(context);

    // =========================================
    // Shutdown (host path)
    // =========================================

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
