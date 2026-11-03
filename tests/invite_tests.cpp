#include "network/invite.h"
#include <iostream>
#include <vector>
int main() {
    int failures = 0;
    auto check = [&](bool value) { if (!value) ++failures; };
    for (const auto& host : {"localhost", "example.com", "192.168.1.12", "2001:db8::1"}) {
        chatbox::Invite invite{host, 7777, true, true, std::string(64, 'A')};
        auto parsed = chatbox::parse_invite(chatbox::make_invite(invite));
        check(parsed.host == host && parsed.port == 7777 && parsed.tls && parsed.password_required);
        check(parsed.fingerprint == invite.fingerprint);
    }
    check(!chatbox::parse_invite("chatbox:1|localhost|7777|0|0|").tls);
    for (const auto& invalid : {
        "chatbox:2|host|7777|1|0|", "chatbox:1|host|0|1|0|", "chatbox:1|host|-1|1|0|",
        "chatbox:1|host|65536|1|0|", "chatbox:1|host|1junk|1|0|", "chatbox:1||7777|1|0|",
        "chatbox:1|host|7777|2|0|", "chatbox:1|host|7777|1|0|bad", "chatbox:1|a b|7777|1|0|",
        "chatbox:1|host|7777|1|0||", "chatbox:1|host\n|7777|1|0|"}) {
        bool rejected = false;
        try { chatbox::parse_invite(invalid); } catch (const std::exception&) { rejected = true; }
        check(rejected);
    }
    if (failures) std::cerr << failures << " invite failures\n";
    return failures ? 1 : 0;
}
