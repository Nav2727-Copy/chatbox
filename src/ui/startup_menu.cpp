#include "ui/startup_menu.h"
#include "app/dedicated_server.h"
#include "ui/curses_ui.h"
#include "ui/setup_forms.h"
#include "ui/display_text.h"
#include "ui/network_task.h"

std::optional<int> run_startup_menu()
{
    using namespace chatbox::ui;
    namespace display = chatbox::ui::ui_text;
    while (true)
    {
        const int mode = choose_curses_menu(display::startup_title, display::startup_subtitle, {
            { 'c', display::start_chatting, display::start_chatting_detail },
            { 'd', display::run_server, display::run_server_detail },
            { 'b', display::run_directory, display::run_directory_detail },
            { 'g', "Quick guide", "Joining, hosting, security and keyboard help" },
            { 'q', display::quit, display::see_you }
        });
        if (mode < 0 || mode == 4) { shutdown_curses_ui(); return 0; }
        if (mode == 3) { show_walkthrough(); continue; }
        if (mode == 0) return std::nullopt;
        try
        {
            if (mode == 1)
            {
                std::vector<CursesField> fields = {
                    port_field(), { "Password (optional)", "", PASS_MAX_LEN, true },
                    { "Automatic port forwarding (UPnP)", "yes", 3, false, true },
                    { "Use TLS", "yes", 3, false, true },
                    { "Log file (none / stdout / path)", "chatlog.txt", LOGFILE_MAX_LEN },
                    { "Publish to a server directory", "no", 3, false, true }
                };
                edit_curses_form(display::dedicated_server, display::dedicated_description, fields);
                DedicatedServerConfig config;
                parse_port(fields[0].value, config.port);
                config.password = fields[1].value;
                config.enable_upnp = fields[2].value == "yes";
                config.tls.enabled = fields[3].value == "yes";
                config.enable_logging = fields[4].value != "none";
                config.log_to_stdout_only = fields[4].value == "stdout";
                if (!fields[4].value.empty() && config.enable_logging && !config.log_to_stdout_only)
                    config.logfile = fields[4].value;
                config.publish_to_browser = fields[5].value == "yes";
                configure_tls(config.tls);
                if (config.publish_to_browser)
                {
                    const auto publication = configure_publication();
                    config.browser_host = publication.host;
                    config.browser_name = publication.name.empty() ? "chatbox dedicated server"
                        : sanitize_browser_field(publication.name, BROWSER_NAME_MAX_LEN);
                    config.browser_public_host = sanitize_browser_field(publication.public_host, HOST_MAX_LEN);
                }
                shutdown_curses_ui();
                return run_dedicated_server_config(config);
            }
            chatbox::tls::ServerConfig tls;
            std::vector<CursesField> fields = { { "Use TLS", "yes", 3, false, true } };
            edit_curses_form(display::server_directory, display::directory_description, fields);
            tls.enabled = fields[0].value == "yes";
            configure_tls(tls);
            shutdown_curses_ui();
            return run_browser_server(BROWSER_PORT, std::move(tls));
        }
        catch (const CursesFormCancelled&) { /* Return to the menu. */ }
        catch (const std::exception& error) { show_curses_notice("Setup failed", error.what()); }
    }
}
