#include "protocol.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <variant>

namespace protocol = chatbox::protocol;

namespace
{
int failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (condition)
        return;
    std::cerr << "line " << line << ": check failed: " << expression << '\n';
    ++failures;
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void valid_frame_round_trip()
{
    const auto known_base64 = protocol::encode_frame("f");
    CHECK(known_base64);
    CHECK(*known_base64.value == "Zg==\n");
    CHECK(*protocol::decode_frame("Zg==\r\n").value == "f");

    const protocol::ClientMessage original = protocol::Chat{ "hello|there" };
    const auto encoded = protocol::encode_client_frame(original);
    CHECK(encoded);
    CHECK(encoded.value->back() == '\n');

    const auto decoded = protocol::decode_client_frame(*encoded.value);
    CHECK(decoded);
    CHECK(std::holds_alternative<protocol::Chat>(*decoded.value));
    CHECK(std::get<protocol::Chat>(*decoded.value).body == "hello|there");
}

void typed_messages_round_trip()
{
    const protocol::ClientMessage hello = protocol::Hello{ protocol::VERSION };
    const auto hello_frame = protocol::encode_client_frame(hello);
    const auto parsed_hello = protocol::decode_client_frame(*hello_frame.value);
    CHECK(parsed_hello);
    CHECK(std::get<protocol::Hello>(*parsed_hello.value).version == protocol::VERSION);

    const protocol::ServerMessage users = protocol::UserList{ { "alice", "bob" } };
    const auto users_frame = protocol::encode_server_frame(users);
    const auto parsed_users = protocol::decode_server_frame(*users_frame.value);
    CHECK(parsed_users);
    CHECK(protocol::serialize(users) == "USERS|alice,bob");
    CHECK(std::get<protocol::UserList>(*parsed_users.value).nicknames.size() == 2);
    CHECK(std::get<protocol::UserList>(*parsed_users.value).nicknames[1] == "bob");

    const protocol::ServerMessage text = protocol::Text{ "[12:00:00] alice: a|b" };
    const auto text_frame = protocol::encode_server_frame(text);
    const auto parsed_text = protocol::decode_server_frame(*text_frame.value);
    CHECK(parsed_text);
    CHECK(std::get<protocol::Text>(*parsed_text.value).body == "[12:00:00] alice: a|b");
}

void malformed_frames_are_rejected()
{
    CHECK(protocol::decode_frame("").error == protocol::Error::EmptyFrame);
    CHECK(protocol::decode_frame("abc").error == protocol::Error::InvalidBase64);
    CHECK(protocol::decode_frame("!!!!").error == protocol::Error::InvalidBase64);
    CHECK(protocol::decode_frame("AB==").error == protocol::Error::InvalidBase64);
    CHECK(protocol::decode_client_frame("Tk9QRQ==\n").error == protocol::Error::UnknownMessage);

    const auto invalid_text = protocol::encode_frame("TEXT|line\nbreak");
    CHECK(protocol::decode_server_frame(*invalid_text.value).error ==
        protocol::Error::InvalidField);

    const auto malformed_leave = protocol::encode_frame("LEAVE|unexpected");
    CHECK(protocol::decode_client_frame(*malformed_leave.value).error ==
        protocol::Error::WrongFieldCount);
}

void versions_are_enforced()
{
    const auto old_hello = protocol::encode_frame("HELLO|1");
    const auto parsed = protocol::decode_client_frame(*old_hello.value);
    CHECK(parsed.error == protocol::Error::UnsupportedVersion);
    CHECK(parsed.detail.find("1") != std::string::npos);
}

void size_limits_are_enforced()
{
    const protocol::ClientMessage max_chat =
        protocol::Chat{ std::string(protocol::MAX_CHAT_LENGTH, 'x') };
    CHECK(protocol::encode_client_frame(max_chat));

    const protocol::ClientMessage oversized_chat =
        protocol::Chat{ std::string(protocol::MAX_CHAT_LENGTH + 1, 'x') };
    const auto oversized_chat_frame = protocol::encode_client_frame(oversized_chat);
    CHECK(oversized_chat_frame.error == protocol::Error::InvalidField);

    CHECK(protocol::encode_frame(std::string(protocol::MAX_FRAME_LENGTH, 'x')).error ==
        protocol::Error::FrameTooLarge);
    CHECK(protocol::decode_frame(std::string(protocol::MAX_FRAME_LENGTH, 'A')).error ==
        protocol::Error::FrameTooLarge);
}
}

int main()
{
    valid_frame_round_trip();
    typed_messages_round_trip();
    malformed_frames_are_rejected();
    versions_are_enforced();
    size_limits_are_enforced();

    if (failures != 0)
    {
        std::cerr << failures << " protocol test(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "protocol tests passed\n";
    return EXIT_SUCCESS;
}
