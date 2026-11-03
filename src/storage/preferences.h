#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace chatbox::storage
{
struct RecentServer
{
    std::string name;
    std::string host;
    std::uint16_t port = 7777;
    bool tls = true;
    std::string ca_path;
    std::string fingerprint;
};

// Nonsecret UI settings only. Identity keys and passwords never enter this model.
struct Preferences
{
    bool remember = false;
    std::string nickname;
    std::map<std::string, std::string> options;
    std::vector<RecentServer> recent_servers;
    void remember_server(RecentServer server);
};

Preferences load_preferences(const std::string& path);
void save_preferences(const std::string& path, const Preferences& preferences);
std::string preferences_path();
}
