/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: CC BY-NC-SA 4.0 (https://creativecommons.org/licenses/by-nc-sa/4.0/)
*/
#include "common.h"
#include "curses_app.h"
#include "dedicated_server.h"

namespace
{
void show_app_usage()
{
    std::cout
        << "Usage:\n"
        << "  chatbox                  - start the curses terminal UI\n"
        << "  chatbox --curses|--tui   - start the curses terminal UI\n\n";
    show_dedicated_usage();
}
}

int main(int argc, char* argv[])
{
    if (sodium_init() < 0)
    {
        std::cerr << "Failed to initialize libsodium\n";
        return 1;
    }

    if (argc >= 2)
    {
        std::string mode = argv[1];
        if (mode == "--curses" || mode == "--tui")
            return run_curses_app();

        if (mode == "--server" || mode == "--dedicated" || mode == "-s")
            return run_dedicated_server(argc, argv);

        if (mode == "--browser")
        {
            chatbox::tls::ServerConfig tls_config{ true, "", "" };
            for (int i = 2; i < argc; ++i)
            {
                const std::string arg = argv[i];
                if (arg == "--allow-plaintext")
                    tls_config.enabled = false;
                else if (arg == "--require-tls")
                    tls_config.enabled = true;
                else if (arg == "--tls-cert" && i + 1 < argc)
                    tls_config.certificate_path = argv[++i];
                else if (arg == "--tls-key" && i + 1 < argc)
                    tls_config.private_key_path = argv[++i];
                else
                {
                    std::cerr << "Unknown or incomplete browser option: " << arg << "\n";
                    return 1;
                }
            }
            if (tls_config.enabled && (tls_config.certificate_path.empty()
                || tls_config.private_key_path.empty()))
            {
                std::cerr << "Browser TLS requires --tls-cert and --tls-key. "
                    "Use --allow-plaintext only for an explicitly insecure browser.\n";
                return 1;
            }
            return run_browser_server(BROWSER_PORT, std::move(tls_config));
        }

        if (mode == "--browse")
        {
            if (argc < 3)
            {
                show_dedicated_usage();
                return 1;
            }

            std::string browser_host;
            if (!parse_browser_address(argv[2], browser_host))
            {
                std::cerr << "Invalid browser address. Use only the browser host/IP; port "
                    << BROWSER_PORT << " is fixed.\n";
                return 1;
            }
            chatbox::tls::ClientConfig tls_config;
            for (int i = 3; i < argc; ++i)
            {
                const std::string arg = argv[i];
                if (arg == "--plaintext")
                    tls_config.enabled = false;
                else if (arg == "--tls-ca" && i + 1 < argc)
                    tls_config.ca_path = argv[++i];
                else if (arg == "--trust-fingerprint" && i + 1 < argc)
                    tls_config.expected_fingerprint = argv[++i];
                else if (arg == "--trust-store" && i + 1 < argc)
                    tls_config.trust_store_path = argv[++i];
                else
                {
                    std::cerr << "Unknown or incomplete browse option: " << arg << "\n";
                    return 1;
                }
            }
            return run_browser_list(browser_host, BROWSER_PORT, tls_config);
        }

        if (mode == "--help" || mode == "-h")
        {
            show_app_usage();
            return 0;
        }

        std::cerr << "Unknown option: " << mode << "\n";
        show_app_usage();
        return 1;
    }

    return run_curses_app();
}
