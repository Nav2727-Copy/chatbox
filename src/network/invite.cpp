#include "network/invite.h"
#include "network/tls.h"
#include "common/utils.h"
#include <stdexcept>
#include <charconv>
namespace chatbox {
namespace {
void validate(const Invite& invite) {
    if (invite.host.empty() || invite.host.size() > HOST_MAX_LEN || !invite.port
        || invite.host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:_%") != std::string::npos)
        throw std::invalid_argument("Invite contains an invalid address or port.");
    if (!invite.fingerprint.empty() && (!invite.tls || tls::normalize_fingerprint(invite.fingerprint).empty()))
        throw std::invalid_argument("Invite contains an invalid TLS fingerprint.");
}
}
std::string make_invite(const Invite& invite) {
    validate(invite);
    return "chatbox:1|" + invite.host + "|" + std::to_string(invite.port) + "|"
        + (invite.tls ? "1" : "0") + "|" + (invite.password_required ? "1" : "0") + "|"
        + tls::normalize_fingerprint(invite.fingerprint);
}
Invite parse_invite(const std::string& text) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i)
        if (i == text.size() || text[i] == '|') { fields.push_back(text.substr(start, i-start)); start = i+1; }
    if (fields.size() != 6 || fields[0] != "chatbox:1") throw std::invalid_argument("Malformed or unsupported invite version.");
    Invite invite;
    const auto parsed = std::from_chars(fields[2].data(), fields[2].data() + fields[2].size(), invite.port);
    if (parsed.ec != std::errc{} || parsed.ptr != fields[2].data() + fields[2].size() || (fields[3] != "0" && fields[3] != "1")
        || (fields[4] != "0" && fields[4] != "1")) throw std::invalid_argument("Invalid invite settings.");
    invite.host = fields[1]; invite.tls = fields[3] == "1"; invite.password_required = fields[4] == "1";
    invite.fingerprint = fields[5]; validate(invite);
    invite.fingerprint = tls::normalize_fingerprint(invite.fingerprint);
    return invite;
}
}
