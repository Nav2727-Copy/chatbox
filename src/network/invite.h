#pragma once
#include <string>
#include <cstdint>
namespace chatbox {
struct Invite {
    std::string host;
    std::uint16_t port = 7777;
    bool tls = true;
    bool password_required = false;
    std::string fingerprint;
};
// chatbox:1|host|port|tls(0/1)|password-required(0/1)|sha256-or-empty
std::string make_invite(const Invite& invite);
Invite parse_invite(const std::string& text);
}
