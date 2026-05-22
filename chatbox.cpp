/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: CC BY-NC-SA 4.0 (https://creativecommons.org/licenses/by-nc-sa/4.0/)
*/
#include "common.h"
#include "dedicated_server.h"
#include "interactive_app.h"

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
        if (mode == "--server" || mode == "--dedicated" || mode == "-s")
            return run_dedicated_server(argc, argv);

        if (mode == "--browser")
        {
            if (argc >= 3)
            {
                std::cerr << "--browser uses fixed port " << BROWSER_PORT << " and takes no port argument\n";
                return 1;
            }
            return run_browser_server(BROWSER_PORT);
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
            if (argc >= 4)
            {
                std::cerr << "--browse uses fixed browser port " << BROWSER_PORT << "\n";
                return 1;
            }
            return run_browser_list(browser_host, BROWSER_PORT);
        }

        if (mode == "--help" || mode == "-h")
        {
            show_dedicated_usage();
            return 0;
        }

        std::cerr << "Unknown option: " << mode << "\n";
        show_dedicated_usage();
        return 1;
    }

    return run_interactive_app();
}
