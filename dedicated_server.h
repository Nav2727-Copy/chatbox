/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: CC BY-NC-SA 4.0 (https://creativecommons.org/licenses/by-nc-sa/4.0/)
*/
#pragma once

#include "common.h"
#include "server_browser.h"
struct DedicatedServerConfig
{
    uint16_t port = 0;
    std::string password;
    std::string logfile = "chatlog.txt";
    std::string bans_file = "bans.txt";
    std::string identities_file = "identities.txt";
    std::string browser_host;
    std::string browser_name = "chatbox dedicated server";
    std::string browser_public_host;
    bool enable_logging = true;
    bool log_to_stdout_only = false;
    bool enable_upnp = true;
    bool publish_to_browser = false;
};
void show_dedicated_usage();
std::string join_args(const std::vector<std::string>& args, size_t start);
bool parse_port(const std::string& text, uint16_t& port);
bool parse_browser_address(const std::string& text, std::string& host);
std::string browser_entry_summary(const BrowserEntry& entry);
int run_browser_server(uint16_t port);
int run_browser_list(const std::string& host, uint16_t port);
int run_dedicated_server_config(const DedicatedServerConfig& config);
int run_dedicated_server(int argc, char* argv[]);
