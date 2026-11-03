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
#include "storage/preferences.h"
#include "network/invite.h"
#include "ui/network_task.h"

#include <curses.h>

namespace
{

bool reconnect_delay(int attempt)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(attempt);
    while (std::chrono::steady_clock::now() < until) {
        show_curses_notice("Reconnecting", "Attempt " + std::to_string(attempt) + " of 3. Draft preserved. Esc cancels.", false);
        timeout(50); if (getch() == 27) { timeout(-1); return false; }
    }
    timeout(-1); return true;
}

// Own all work and results in the worker, including abandoned results. No UI or
// stack references cross this boundary; cancellation never waits on router I/O.
std::shared_ptr<UPnPMapper> prepare_upnp(uint16_t port)
{
    struct State { std::atomic<bool> done{false}, cancelled{false}; std::shared_ptr<UPnPMapper> mapper; std::string error; };
    auto state = std::make_shared<State>();
    std::thread([state, port] {
        try {
            auto mapper = std::shared_ptr<UPnPMapper>(new UPnPMapper, [](UPnPMapper* value) {
                std::thread([value] { delete value; }).detach();
            });
            if (mapper->discover() && !state->cancelled) mapper->openPort(std::to_string(port), "TCP");
            if (!state->cancelled) state->mapper = std::move(mapper);
        } catch (const std::exception& error) { state->error = error.what(); }
        state->done = true;
    }).detach();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!state->done) {
        show_curses_notice("Preparing internet hosting", "Discovering your router and forwarding TCP. Esc skips UPnP.", false);
        timeout(50);
        if (getch() == 27 || std::chrono::steady_clock::now() >= deadline) {
            state->cancelled = true; timeout(-1); return {};
        }
    }
    timeout(-1);
    if (!state->error.empty()) push_message("[hosting] " + state->error);
    return state->mapper;
}

std::string explain_connection_error(ConnectionFailure failure)
{
    namespace text = chatbox::ui::ui_text;
    switch (failure) {
    case ConnectionFailure::Dns: return text::connection_dns_error;
    case ConnectionFailure::Refused: return text::connection_refused_error;
    case ConnectionFailure::Timeout: return text::connection_timeout_error;
    case ConnectionFailure::Tls: return text::connection_tls_error;
    case ConnectionFailure::Cancelled: return "Cancelled. You can edit the details and retry.";
    default: return text::connection_generic_error;
    }
}

bool connect_with_progress(ChatClient& client, boost::asio::io_context& io,
    const std::string& host, uint16_t port)
{
    bool done = false, success = false;
    io.restart();
    client.async_connect(host, port, [&](bool result) { success = result; done = true; });
    while (!done) {
        io.poll();
        if (done) break;
        show_curses_notice("Joining " + host, client.connection_phase() + ". Press Esc to cancel.", false);
        timeout(50);
        if (getch() == 27) client.cancel_connect();
    }
    timeout(-1);
    if (!success) {
        // Drain ready cancellations; late DNS callbacks are protected by an attempt lifetime token.
        io.restart();
        io.poll();
    }
    return success;
}

bool wait_for_session(const std::string& detail, const std::function<bool()>& ready,
    std::chrono::seconds limit)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
        show_curses_notice("Joining server", detail + " Press Esc to cancel.", false);
        timeout(50);
        if (getch() == 27) { timeout(-1); return false; }
    }
    timeout(-1);
    return ready();
}

// The caller keeps the network thread running throughout authentication and joining.
std::optional<std::string> join_client_session(ChatClient& client,
    const std::string& nickname)
{
    if (!wait_for_session("Waiting for protocol handshake.", [&] {
        return client.auth_required() || client.auth_resolved(); }, std::chrono::seconds(5)))
        return "Protocol handshake timed out or was cancelled.";

    if (client.auth_required())
    {
        std::string password;
        try { password = prompt_curses_password(10, 4, "Password"); }
        catch (const CursesFormCancelled&) { return "Joining was cancelled."; }
        client.send(chatbox::protocol::ClientMessage{
            chatbox::protocol::Authenticate{ password } });

        show_curses_notice("Joining server", "Checking your password...", false);

        if (!wait_for_session("Checking your password.", [&] { return client.auth_resolved(); },
            std::chrono::seconds(5)))
            return "Authentication timed out or was cancelled.";
        if (!client.auth_ok())
            return "Authentication failed. Wrong password.";
    }

    if (!client.auth_ok())
        return "Server rejected authentication or the protocol handshake.";

    client.identify(nickname);
    if (!wait_for_session("Checking your nickname and identity.", [&] { return client.join_resolved(); },
        std::chrono::seconds(5)))
        return "Joining timed out or was cancelled.";

    if (client.was_banned())
        return "You are banned from this server.";
    if (client.nickname_taken())
        return "Nickname is already in use.";
    if (client.join_failed())
        return "Join failed: " + client.join_failure_reason();

    return std::nullopt;
}


}

int run_curses_app()
{
    boost::asio::io_context io;

    std::shared_ptr<ChatServer> server;
    std::unique_ptr<ChatClient> client;
    std::shared_ptr<UPnPMapper> upnp;
    std::shared_ptr<ServerBrowserPublisher> browser_publisher;

    reset_chat_state();
    initialize_curses_ui();

    if (auto exit_code = run_startup_menu())
        return *exit_code;

    // =========================================
    // Nickname prompt
    // =========================================

    using namespace chatbox::ui;
    namespace display = chatbox::ui::ui_text;
    chatbox::storage::Preferences preferences;
    try { preferences = chatbox::storage::load_preferences(chatbox::storage::preferences_path()); }
    catch (const std::exception& error) { show_curses_notice("Could not load preferences", error.what()); }
    auto save_preferences = [&] {
        try { chatbox::storage::save_preferences(chatbox::storage::preferences_path(), preferences); }
        catch (const std::exception& error) { show_curses_notice("Could not save preferences", error.what()); }
    };
    std::string nickname;
    try
    {
        std::vector<CursesField> fields = {
            { display::nickname, preferences.nickname, NICK_MAX_LEN, false, false, [](const std::string& value) {
                return is_valid_nickname(value) ? std::string{}
                    : display::nickname_error;
            } },
            { "Remember nickname and recent servers", preferences.remember ? "yes" : "no", 3, false, true }
        };
        edit_curses_form(display::nickname_prompt, display::nickname_description, fields);
        nickname = fields[0].value;
        preferences.remember = fields[1].value == "yes";
        preferences.nickname = nickname;
        if (!preferences.remember) { preferences.recent_servers.clear(); preferences.options.clear(); }
        save_preferences();
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

    std::string draft;
    std::string previous_host = "localhost";
    std::string previous_port = "7777";
    std::string previous_host_port = "7777";
    std::string previous_join_tls = "yes";
    std::string previous_password;
    std::string previous_upnp = "yes";
    std::string previous_host_tls = "yes";
    chatbox::tls::ServerConfig previous_host_certificate;
    previous_host_certificate.certificate_path = preferences.options["host.certificate"];
    previous_host_certificate.private_key_path = preferences.options["host.key"];
    std::string hosting_details;

    while (true)
    {
        const int choice = choose_curses_menu(std::string(display::hello_name) + nickname + ".", display::where_to_go, {
            { 'j', display::join_server, display::join_server_detail },
            { 'h', display::host_conversation, display::host_detail },
            { 'b', display::discover_servers, display::discover_detail_main },
            { 'r', "Recent servers", "Review, join, rename, or remove a remembered server." },
            { 'n', "Nickname and preferences", "Choose a local identity and whether to remember servers." },
            { 'i', "Join by invite", "Paste a versioned Chatbox invite and review its settings." },
            { 'g', "Quick guide", "Revisit joining, hosting and security help." },
            { 'q', display::quit, display::close_chatbox }
        });
        const int ch = choice < 0 ? 'q' : std::string("jhbrnigq")[choice];
        if (ch == 'g') { show_walkthrough(); continue; }
        std::optional<chatbox::storage::RecentServer> recent;
        if (ch == 'r') {
            std::vector<CursesMenuItem> items;
            for (const auto& entry : preferences.recent_servers)
                items.push_back({0, entry.name, entry.host + ":" + std::to_string(entry.port)
                    + (entry.tls ? " (TLS)" : " (insecure plaintext)")});
            if (items.empty()) { show_curses_notice("Recent servers", "Enable remembering in the nickname form, then join a server successfully."); continue; }
            const int selected = choose_curses_menu("Recent servers", "Choose a saved connection.", items);
            if (selected < 0) continue;
            const int action = choose_curses_menu("Saved server", items[selected].description, {
                {'j', "Join", "Connect using these settings."},
                {'n', "Rename", "Change the display name."},
                {'d', "Remove", "Forget this saved connection."}
            });
            if (action == 0) recent = preferences.recent_servers[selected];
            else if (action == 1) {
                std::vector<CursesField> fields{required_field("Display name", items[selected].title, HOST_MAX_LEN)};
                try {
                    edit_curses_form("Rename server", "Connection details stay the same.", fields);
                    preferences.recent_servers[selected].name = fields[0].value;
                    save_preferences();
                } catch (const CursesFormCancelled&) {}
                continue;
            } else if (action == 2) {
                preferences.recent_servers.erase(preferences.recent_servers.begin() + selected);
                save_preferences();
                continue;
            } else continue;
        }
        try
        {
            if (ch == 'q' || ch == 'Q')
            {
                shutdown_curses_ui();
                return 0;
            }

            else if (ch == 'n') {
                std::vector<CursesField> fields = {
                    {display::nickname, nickname, NICK_MAX_LEN, false, false, [](const std::string& value) {
                        return is_valid_nickname(value) ? std::string{} : display::nickname_error;
                    }},
                    {"Remember nickname and recent servers", preferences.remember ? "yes" : "no", 3, false, true}
                };
                edit_curses_form("Nickname and preferences", "Each nickname uses its existing local identity key.", fields);
                ClientIdentity selected_identity;
                bool created = false;
                if (!load_or_create_identity(fields[0].value, selected_identity, created)) {
                    show_curses_notice("Identity unavailable", "Could not load the identity for that nickname.");
                    continue;
                }
                nickname = fields[0].value;
                local_identity = selected_identity;
                preferences.nickname = nickname;
                preferences.remember = fields[1].value == "yes";
                if (!preferences.remember) { preferences.recent_servers.clear(); preferences.options.clear(); }
                save_preferences();
                continue;
            }

            // ---- HOST ----
            else if (ch == 'h' || ch == 'H')
            {
                std::vector<CursesField> fields = {
                    { display::host_port_tcp, previous_host_port, 5, false, false, [](const std::string& value) {
                        uint16_t port = 0;
                        return parse_port(value, port) ? std::string{} : ui_text::invalid_port;
                    } }, { display::host_password_optional, previous_password, PASS_MAX_LEN, true },
                    { display::upnp_forwarding, previous_upnp, 3, false, true },
                    { display::publish_directory, "no", 3, false, true },
                    { display::host_tls_field, previous_host_tls, 3, false, true }
                };
                edit_curses_form(display::host_conversation, display::host_form_guidance, fields);
                previous_host_port = fields[0].value;
                previous_password = fields[1].value;
                previous_upnp = fields[2].value;
                previous_host_tls = fields[4].value;
                const auto& port_buf = fields[0].value;
                const auto& room_password = fields[1].value;
                const bool enable_upnp = fields[2].value == "yes";
                const bool publish_to_browser = fields[3].value == "yes";
                auto& host_tls = previous_host_certificate;
                host_tls.enabled = fields[4].value == "yes";
                configure_tls(host_tls);
                preferences.options["host.certificate"] = host_tls.certificate_path;
                preferences.options["host.key"] = host_tls.private_key_path;
                save_preferences();
                PublicationSettings publication;
                if (publish_to_browser) {
                    publication = configure_publication({preferences.options["publish.host"], preferences.options["publish.name"],
                        preferences.options["publish.address"], preferences.options["publish.tls"] != "no",
                        preferences.options["publish.ca"], preferences.options["publish.pin"]});
                    preferences.options["publish.host"] = publication.host; preferences.options["publish.name"] = publication.name;
                    preferences.options["publish.address"] = publication.public_host;
                    preferences.options["publish.tls"] = publication.tls ? "yes" : "no";
                    preferences.options["publish.ca"] = publication.ca_path; preferences.options["publish.pin"] = publication.fingerprint;
                    save_preferences();
                }
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
                    server = std::make_shared<ChatServer>(
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
                        server->stop(); io.restart(); io.run(); server.reset();
                        continue;
                    }

                    port = server->port();
                    server->start_local_user();
                    push_message("[system] Hosting on port " + std::to_string(port));
                    push_message(std::string(display::host_listening_prefix) + std::to_string(port)
                        + (host_tls.enabled ? display::host_tls_enabled_suffix : display::host_tls_disabled_suffix));
                    push_message("[system] Your identity: " + identity_fingerprint(local_identity.public_key_hex));

                    if (!room_password.empty())
                        push_message("[system] Server password protection enabled");

                    std::string publish_host = sanitize_browser_field(public_host_buf, HOST_MAX_LEN);

                    hosting_details = "Listening on TCP port " + std::to_string(port)
                        + (host_tls.enabled ? " with TLS.\n" : " — INSECURE plaintext.\n")
                        + (room_password.empty() ? "No server password.\n" : "Password required (share separately).\n")
                        + "TLS protects transport, not end-to-end messages.\n";
                    if (host_tls.enabled) hosting_details += "SHA-256: "
                        + chatbox::tls::certificate_sha256_fingerprint(host_tls.certificate_path) + "\n";
                    if (enable_upnp) upnp = prepare_upnp(port);
                    if (upnp && upnp->lastError().empty()) {
                        hosting_details += "Router TCP mapping requested successfully; internet reachability is NOT verified.\n";
                        if (publish_host.empty()) publish_host = upnp->externalIP();
                    } else hosting_details += "UPnP unavailable, failed, skipped, or timed out. "
                        + (upnp ? upnp->lastError() : "") + "\n";
                    hosting_details += "For LAN guests, use this computer's LAN address. For internet guests, use a public address "
                        "and forward TCP port " + std::to_string(port) + " to this computer. Check your firewall too.\n";
                    // Interface enumeration avoids blocking DNS merely to find local addresses.
                    auto add_invite = [&](const std::string& address, const std::string& scope) {
                        hosting_details += scope + ": " + address + ":" + std::to_string(port) + "\n"
                            + chatbox::make_invite({address, port, host_tls.enabled, !room_password.empty(),
                                host_tls.enabled ? chatbox::tls::certificate_sha256_fingerprint(host_tls.certificate_path) : ""}) + "\n";
                    };
                    for (const auto& address : local_network_addresses()) add_invite(address, "LAN");
                    if (!publish_host.empty()) add_invite(publish_host, "Public/advertised (unverified)");
                    add_invite("localhost", "This computer only");
                    hosting_details += "Invites contain connection details and a public fingerprint, never passwords or private keys. "
                        "Share through a trusted channel. F3 reopens this summary.";
                    show_curses_notice("Hosting summary / invites", hosting_details);

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

                        const auto weak_server = std::weak_ptr<ChatServer>(server);
                        auto directory_tls = interactive_tls();
                        directory_tls.enabled = publication.tls; directory_tls.ca_path = publication.ca_path;
                        directory_tls.expected_fingerprint = publication.fingerprint;
                        auto publisher = run_network_task<std::shared_ptr<ServerBrowserPublisher>>(directory_tls,
                            [browser_host, entry, weak_server](const auto& config) {
                                auto result = std::shared_ptr<ServerBrowserPublisher>(new ServerBrowserPublisher(
                                    browser_host, BROWSER_PORT, entry, [weak_server] {
                                        const auto host = weak_server.lock();
                                        return host ? static_cast<int>(host->connected_users().size()) : 0;
                                    }, config), [](ServerBrowserPublisher* value) {
                                        std::thread([value] { delete value; }).detach();
                                    });
                                std::string error;
                                if (!result->start(error)) throw std::runtime_error(error);
                                return result;
                            });
                        if (publisher) {
                            browser_publisher = *publisher;
                            push_message("[hosting] Published directory listing for " + entry.host);
                        }
                        else
                        {
                            push_message("[hosting] Directory publication cancelled or timed out.");
                            browser_publisher.reset();
                        }
                    }

                    io.restart();
                    std::thread network_thread([&] { io.run(); });
                    ChatInputContext context;
                    context.server = server.get(); context.local_nickname = nickname;
                    context.host_commands_enabled = true; context.connection_details = hosting_details;
                    try { run_curses_chat_loop(context, draft); }
                    catch (...) { server->stop(); network_thread.join(); throw; }
                    browser_publisher.reset(); upnp.reset();
                    server->stop();
                    network_thread.join();
                    io.restart(); io.poll(); server.reset();
                    reset_chat_state();
                    continue;
                }
                catch (const std::exception& error)
                {
                    if (server) { server->stop(); io.restart(); io.run(); server.reset(); }
                    browser_publisher.reset(); upnp.reset();
                    show_curses_notice("Hosting failed", error.what());
                    continue;
                }
            }

            // ---- JOIN / BROWSE ----
            else if (ch == 'j' || ch == 'J' || ch == 'b' || ch == 'B' || ch == 'i' || recent)
            {
                std::string selected_host;
                uint16_t selected_port = 0;
                auto client_tls = interactive_tls();

                if (recent) {
                    selected_host = recent->host;
                    selected_port = recent->port;
                    client_tls.enabled = recent->tls;
                    client_tls.ca_path = recent->ca_path;
                    client_tls.expected_fingerprint = recent->fingerprint;
                }
                else if (ch == 'i') {
                    std::vector<CursesField> fields{required_field("Chatbox invite", "", 1024)};
                    edit_curses_form("Join by invite", "Paste an invite from someone you trust. It contains no password.", fields);
                    const auto invite = chatbox::parse_invite(fields[0].value);
                    show_curses_notice("Review invite", invite.host + ":" + std::to_string(invite.port)
                        + (invite.tls ? "\nTLS enabled." : "\nINSECURE plaintext exposes messages and passwords.")
                        + (invite.password_required ? "\nPassword required; ask the host separately." : "\nNo password indicated.")
                        + "\nSHA-256: " + (invite.fingerprint.empty() ? "not supplied" : invite.fingerprint)
                        + "\nA fingerprint is useful only if this invite came through a trusted channel.");
                    if (choose_curses_menu("Use this invite?", "Review the address and security details first.", {
                        {'b', "Back", "Discard invite."}, {'j', "Join", "Use these connection details."}}) != 1) continue;
                    selected_host = invite.host; selected_port = invite.port;
                    client_tls.enabled = invite.tls; client_tls.expected_fingerprint = invite.fingerprint;
                }
                else if (ch == 'b'  || ch == 'B')
                {
                    std::vector<CursesField> fields = {
                        required_field("Directory host", preferences.options["directory.host"], HOST_MAX_LEN),
                        { "Use TLS (no = insecure plaintext)", preferences.options["directory.tls"] == "no" ? "no" : "yes", 3, false, true },
                        { "Trusted CA file (optional)", preferences.options["directory.ca"], LOGFILE_MAX_LEN },
                        { "SHA-256 fingerprint (optional)", preferences.options["directory.pin"], 95 }
                    };
                    edit_curses_form("Discover servers", "Connect to a server directory to see who is online.", fields);
                    preferences.options["directory.host"] = fields[0].value;
                    preferences.options["directory.tls"] = fields[1].value;
                    preferences.options["directory.ca"] = fields[2].value;
                    preferences.options["directory.pin"] = fields[3].value;
                    save_preferences();
                    client_tls.ca_path = fields[2].value; client_tls.expected_fingerprint = fields[3].value;
                    const auto& browser_host_buf = fields[0].value;
                    client_tls.enabled = fields[1].value == "yes";
                    show_curses_notice("Finding servers", "Loading the server directory...", false);

                    std::string browser_host;
                    if (!parse_browser_address(browser_host_buf, browser_host))
                    {
                        show_curses_notice("Could not continue", "Enter only the browser server host/IP, without a port.");
                        continue;
                    }

                    struct DirectoryResult { std::vector<BrowserEntry> entries; std::string error; bool success; };
                    auto result = run_network_task<DirectoryResult>(client_tls, [browser_host](const auto& config) {
                        DirectoryResult result;
                        result.success = ServerBrowserClient::list_servers(browser_host, BROWSER_PORT,
                            result.entries, result.error, config);
                        return result;
                    });
                    if (!result) continue;
                    if (!result->success) {
                        show_curses_notice("Directory unavailable", result->error + "\nChoose Discover to retry or Join for a manual address."); continue;
                    }
                    const auto& entries = result->entries;

                    if (entries.empty())
                    {
                        show_curses_notice("Could not continue", "No servers are published there right now. Ask the host for an invite or choose Join from the menu. Choose Discover again to refresh.");
                        continue;
                    }

                    std::vector<CursesMenuItem> servers;
                    for (const auto& entry : entries)
                        servers.push_back({ 0, entry.name, browser_entry_summary(entry) });
                    const int selected = choose_curses_menu("Discover servers", "Select a server to join.", servers);
                    if (selected < 0) continue;
                    const int choice = selected + 1;

                    show_curses_notice(entries[choice - 1].name, browser_entry_summary(entries[choice - 1]));
                    client_tls = interactive_tls();
                    selected_host = entries[choice - 1].host;
                    selected_port = entries[choice - 1].port;
                    client_tls.enabled = entries[choice - 1].uses_tls;
                }
                else
                {
                    std::vector<CursesField> fields = {
                        required_field(display::join_host_field, previous_host, HOST_MAX_LEN),
                        { display::join_port_field, previous_port, 5, false, false, [](const std::string& value) {
                            uint16_t port = 0;
                            return parse_port(value, port) ? std::string{} : ui_text::invalid_port;
                        } }, { display::join_tls_field, previous_join_tls, 3, false, true },
                        { "Trusted CA file (optional)", preferences.options["join.ca"], LOGFILE_MAX_LEN },
                        { "SHA-256 fingerprint (optional)", preferences.options["join.pin"], 95 }
                    };
                    edit_curses_form(display::join_server, display::join_form_guidance, fields);
                    selected_host = fields[0].value;
                    previous_host = fields[0].value;
                    previous_port = fields[1].value;
                    previous_join_tls = fields[2].value;
                    parse_port(fields[1].value, selected_port);
                    client_tls.enabled = fields[2].value == "yes";
                    client_tls.ca_path = fields[3].value; client_tls.expected_fingerprint = fields[4].value;
                    preferences.options["join.ca"] = fields[3].value; preferences.options["join.pin"] = fields[4].value;
                    save_preferences();
                }

                std::string restore_room;
                int retries = 0;
                bool reconnecting = false;
                while (true) {
                    if (reconnecting && !reconnect_delay(++retries)) break;
                    reset_chat_state();
                    client = std::make_unique<ChatClient>(io, client_tls);
                    client->set_identity(local_identity);
                    if (!connect_with_progress(*client, io, selected_host, selected_port)) {
                        const auto failure = client->connection_failure();
                        if (reconnecting && retries < 3 && failure != ConnectionFailure::Tls && failure != ConnectionFailure::Cancelled) {
                            client.reset(); continue;
                        }
                        show_curses_notice("Connection failed", explain_connection_error(failure) + "\nDetails: " + client->connect_error());
                        client.reset();
                        if (failure == ConnectionFailure::Cancelled || choose_curses_menu("Connection ended", "Your unsent draft is preserved.", {
                            {'m', "Server selection", "Correct settings or select another server."},
                            {'r', "Retry", "Try these settings again."}}) != 1) break;
                        reconnecting = false; retries = 0; continue;
                    }
                    io.restart();
                    auto work = boost::asio::make_work_guard(io);
                    std::thread network_thread([&] { io.run(); });
                    // Ensure every exit joins before destroying the client or its io_context.
                    auto finish = [&] {
                        client->close(); work.reset(); network_thread.join(); io.restart(); io.poll();
                    };
                    try {
                        if (auto error = join_client_session(*client, nickname)) {
                            finish(); show_curses_notice("Could not join", *error + display::join_error_next_step);
                            break; // auth / ban / nickname errors always require user action
                        }
                        if (!restore_room.empty() && restore_room != "lobby") handle_chat_input("/join " + restore_room,
                            ChatInputContext{client.get(), nullptr, nickname});
                        push_message("[system] Connected to " + selected_host + ":" + std::to_string(selected_port));
                        if (preferences.remember) {
                            preferences.remember_server({selected_host, selected_host, selected_port, client_tls.enabled,
                                client_tls.ca_path, client_tls.expected_fingerprint}); save_preferences();
                        }
                        ChatInputContext context;
                        context.client = client.get(); context.local_nickname = nickname;
                        context.connection_details = "Connected to " + selected_host + ":" + std::to_string(selected_port)
                            + (client_tls.enabled ? " with TLS." : " — INSECURE plaintext.")
                            + "\nTLS protects transport, not end-to-end chat. Trust store: " + client_tls.trust_store_path;
                        const auto result = run_curses_chat_loop(context, draft);
                        restore_room = active_room_name();
                        const bool denied = client->was_kicked() || client->was_banned();
                        finish();
                        if (result == ChatExit::Left || denied) break;
                        reconnecting = true; retries = 0;
                    } catch (...) { if (network_thread.joinable()) finish(); throw; }
                }
                client.reset(); reset_chat_state();
                continue;
            }
        }
        catch (const CursesFormCancelled&) { continue; }
        catch (const std::exception& error) { show_curses_notice("Setup failed", error.what()); }
    }

    shutdown_curses_ui();
    return 0;
}

int run_interactive_app()
{
    return run_curses_app();
}
