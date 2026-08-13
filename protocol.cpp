#include "protocol.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <type_traits>

namespace chatbox::protocol
{
namespace
{
constexpr std::string_view BASE64_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

template <typename T>
Result<T> success(T value)
{
    return { std::move(value), Error::None, {} };
}

template <typename T>
Result<T> failure(Error error, std::string detail)
{
    return { std::nullopt, error, std::move(detail) };
}

int base64_value(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

std::vector<std::string_view> split_fields(std::string_view payload)
{
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t delimiter = payload.find('|', start);
        if (delimiter == std::string_view::npos)
        {
            fields.push_back(payload.substr(start));
            return fields;
        }
        fields.push_back(payload.substr(start, delimiter - start));
        start = delimiter + 1;
    }
}

bool is_valid_nickname(std::string_view nickname)
{
    if (nickname.empty() || nickname.size() > 31)
        return false;

    return std::none_of(nickname.begin(), nickname.end(), [](unsigned char ch)
        {
            return std::iscntrl(ch) || std::isspace(ch) || ch == '|' || ch == ',';
        });
}

bool is_valid_text(std::string_view text, std::size_t max_length)
{
    if (text.empty() || text.size() > max_length)
        return false;

    return std::none_of(text.begin(), text.end(), [](unsigned char ch)
        {
            return std::iscntrl(ch) && ch != '\t';
        });
}

bool parse_unsigned(std::string_view text, std::uint64_t& value)
{
    value = 0;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value);
    return !text.empty() && parsed.ec == std::errc{} &&
        parsed.ptr == text.data() + text.size();
}

std::string rest_after_delimiter(std::string_view payload, std::size_t delimiter_number)
{
    std::size_t position = 0;
    for (std::size_t i = 0; i < delimiter_number; ++i)
    {
        position = payload.find('|', position);
        if (position == std::string_view::npos)
            return {};
        ++position;
    }
    return std::string(payload.substr(position));
}

bool is_hex(std::string_view text, std::size_t length)
{
    return text.size() == length &&
        std::all_of(text.begin(), text.end(), [](unsigned char ch)
            {
                return std::isxdigit(ch) != 0;
            });
}

template <typename... Handlers>
struct Overloaded : Handlers...
{
    using Handlers::operator()...;
};

template <typename... Handlers>
Overloaded(Handlers...) -> Overloaded<Handlers...>;

std::string rest_after_command(std::string_view payload)
{
    const std::size_t delimiter = payload.find('|');
    return delimiter == std::string_view::npos
        ? std::string{}
        : std::string(payload.substr(delimiter + 1));
}

template <typename Message>
Result<Message> decode_message_frame(
    std::string_view frame,
    Result<Message> (*parser)(std::string_view))
{
    auto decoded = decode_frame(frame);
    if (!decoded)
        return failure<Message>(decoded.error, decoded.detail);
    return parser(*decoded.value);
}
}

std::string_view error_name(Error error)
{
    switch (error)
    {
    case Error::None: return "none";
    case Error::EmptyFrame: return "empty frame";
    case Error::FrameTooLarge: return "frame too large";
    case Error::InvalidBase64: return "invalid Base64";
    case Error::EmptyPayload: return "empty payload";
    case Error::UnknownMessage: return "unknown message";
    case Error::WrongFieldCount: return "wrong field count";
    case Error::InvalidField: return "invalid field";
    case Error::UnsupportedVersion: return "unsupported version";
    }
    return "unknown error";
}

bool is_valid_room_name(std::string_view name)
{
    if (name.empty() || name.size() > MAX_ROOM_NAME_LENGTH)
        return false;

    return std::all_of(name.begin(), name.end(), [](unsigned char ch)
        {
            return std::isalnum(ch) != 0 || ch == '-' || ch == '_';
        });
}

bool is_valid_room_topic(std::string_view topic)
{
    if (topic.size() > MAX_ROOM_TOPIC_LENGTH)
        return false;

    return std::none_of(topic.begin(), topic.end(), [](unsigned char ch)
        {
            return std::iscntrl(ch) != 0;
        });
}

Result<std::string> encode_frame(std::string_view payload)
{
    if (payload.empty())
        return failure<std::string>(Error::EmptyPayload, "Cannot encode an empty payload");
    constexpr std::size_t max_payload_length = ((MAX_FRAME_LENGTH - 1) / 4) * 3;
    if (payload.size() > max_payload_length)
        return failure<std::string>(Error::FrameTooLarge,
            "Encoded frame exceeds " + std::to_string(MAX_FRAME_LENGTH) + " bytes");

    std::string output;
    output.reserve(((payload.size() + 2) / 3) * 4 + 1);

    for (std::size_t i = 0; i < payload.size(); i += 3)
    {
        const std::size_t remaining = payload.size() - i;
        const std::uint32_t first = static_cast<unsigned char>(payload[i]);
        const std::uint32_t second = remaining > 1
            ? static_cast<unsigned char>(payload[i + 1]) : 0;
        const std::uint32_t third = remaining > 2
            ? static_cast<unsigned char>(payload[i + 2]) : 0;
        const std::uint32_t block = (first << 16) | (second << 8) | third;

        output += BASE64_CHARS[(block >> 18) & 0x3f];
        output += BASE64_CHARS[(block >> 12) & 0x3f];
        output += remaining > 1 ? BASE64_CHARS[(block >> 6) & 0x3f] : '=';
        output += remaining > 2 ? BASE64_CHARS[block & 0x3f] : '=';
    }

    output += '\n';
    if (output.size() > MAX_FRAME_LENGTH)
        return failure<std::string>(Error::FrameTooLarge,
            "Encoded frame exceeds " + std::to_string(MAX_FRAME_LENGTH) + " bytes");

    return success(std::move(output));
}

Result<std::string> decode_frame(std::string_view frame)
{
    if (!frame.empty() && frame.back() == '\n')
        frame.remove_suffix(1);
    if (!frame.empty() && frame.back() == '\r')
        frame.remove_suffix(1);

    if (frame.empty())
        return failure<std::string>(Error::EmptyFrame, "Frame is empty");
    if (frame.size() + 1 > MAX_FRAME_LENGTH)
        return failure<std::string>(Error::FrameTooLarge,
            "Frame exceeds " + std::to_string(MAX_FRAME_LENGTH) + " bytes");
    if (frame.size() % 4 != 0)
        return failure<std::string>(Error::InvalidBase64,
            "Base64 length must be divisible by four");

    std::string output;
    output.reserve((frame.size() / 4) * 3);

    for (std::size_t i = 0; i < frame.size(); i += 4)
    {
        const bool final_group = i + 4 == frame.size();
        const int v0 = base64_value(frame[i]);
        const int v1 = base64_value(frame[i + 1]);
        const bool pad2 = frame[i + 2] == '=';
        const bool pad3 = frame[i + 3] == '=';
        const int v2 = pad2 ? 0 : base64_value(frame[i + 2]);
        const int v3 = pad3 ? 0 : base64_value(frame[i + 3]);

        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0 ||
            (pad2 && !pad3) || ((pad2 || pad3) && !final_group))
        {
            return failure<std::string>(Error::InvalidBase64,
                "Frame contains invalid Base64 padding or characters");
        }

        if ((pad2 && (v1 & 0x0f) != 0) ||
            (pad3 && !pad2 && (v2 & 0x03) != 0))
        {
            return failure<std::string>(Error::InvalidBase64,
                "Frame uses non-canonical Base64 padding bits");
        }

        output += static_cast<char>((v0 << 2) | (v1 >> 4));
        if (!pad2)
        {
            output += static_cast<char>((v1 << 4) | (v2 >> 2));
            if (!pad3)
                output += static_cast<char>((v2 << 6) | v3);
        }
    }

    if (output.empty())
        return failure<std::string>(Error::EmptyPayload, "Decoded payload is empty");
    return success(std::move(output));
}

std::string serialize(const ClientMessage& message)
{
    return std::visit(Overloaded{
        [](const Hello& value) { return "HELLO|" + std::to_string(value.version); },
        [](const Authenticate& value) { return "AUTH|" + value.password; },
        [](const Identify& value) {
            return "IDENTIFY|" + value.nickname + "|" + value.public_key_hex;
        },
        [](const IdentityProof& value) { return "PROOF|" + value.signature_hex; },
        [](const Leave&) { return std::string("LEAVE"); },
        [](const Disconnect&) { return std::string("DISCONNECT"); },
        [](const ListRooms&) { return std::string("ROOMS"); },
        [](const CreateRoom& value) { return "CREATE|" + value.name; },
        [](const JoinRoom& value) { return "JOIN|" + value.name; },
        [](const TopicRequest& value) {
            return value.topic ? "TOPIC|" + *value.topic : std::string("TOPIC");
        },
        [](const Chat& value) { return "MSG|" + value.body; },
        [](const Whisper& value) {
            return "WHISPER|" + value.target + "|" + value.body;
        },
        [](const KickRequest& value) {
            return "KICK|" + value.nickname + "|" + value.reason;
        }
        }, message);
}

std::string serialize(const ServerMessage& message)
{
    return std::visit(Overloaded{
        [](const Hello& value) { return "HELLO|" + std::to_string(value.version); },
        [](const AuthRequired&) { return std::string("AUTH_REQUIRED"); },
        [](const AuthAccepted&) { return std::string("AUTH_OK"); },
        [](const AuthRejected&) { return std::string("AUTH_FAIL"); },
        [](const Challenge& value) { return "CHALLENGE|" + value.challenge_hex; },
        [](const IdentityAccepted& value) { return "IDENTITY_OK|" + value.fingerprint; },
        [](const JoinAccepted&) { return std::string("JOIN_OK"); },
        [](const Banned&) { return std::string("BANNED"); },
        [](const NicknameTaken&) { return std::string("NICK_TAKEN"); },
        [](const JoinRejected& value) { return "JOIN_FAIL|" + value.reason; },
        [](const Kick& value) {
            return "KICK|" + value.nickname + "|" + value.reason;
        },
        [](const UserList& value) {
            std::string output = "USERS|" + std::to_string(value.room_id) + "|";
            for (std::size_t i = 0; i < value.nicknames.size(); ++i)
            {
                if (i != 0) output += ',';
                output += value.nicknames[i];
            }
            return output;
        },
        [](const RoomList& value) {
            std::string output = "ROOMS|";
            for (std::size_t i = 0; i < value.rooms.size(); ++i)
            {
                if (i != 0) output += ';';
                output += std::to_string(value.rooms[i].id) + ","
                    + value.rooms[i].name + ","
                    + std::to_string(value.rooms[i].user_count);
            }
            return output;
        },
        [](const RoomJoined& value) {
            return "ROOM|" + std::to_string(value.id) + "|"
                + value.name + "|" + value.topic;
        },
        [](const RoomTopic& value) {
            return "TOPIC|" + std::to_string(value.room_id) + "|" + value.topic;
        },
        [](const RoomText& value) {
            return "ROOM_TEXT|" + std::to_string(value.room_id) + "|" + value.body;
        },
        [](const Text& value) { return "TEXT|" + value.body; },
        [](const ServerError& value) { return "ERROR|" + value.reason; }
        }, message);
}

Result<ClientMessage> parse_client_message(std::string_view payload)
{
    if (payload.empty())
        return failure<ClientMessage>(Error::EmptyPayload, "Client message is empty");

    const auto fields = split_fields(payload);
    const std::string_view command = fields.front();

    if (command == "HELLO")
    {
        if (fields.size() != 2)
            return failure<ClientMessage>(Error::WrongFieldCount, "HELLO requires one version field");
        unsigned version = 0;
        const auto parsed = std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(), version);
        if (parsed.ec != std::errc{} || parsed.ptr != fields[1].data() + fields[1].size())
            return failure<ClientMessage>(Error::InvalidField, "HELLO version must be an integer");
        if (version != VERSION)
            return failure<ClientMessage>(Error::UnsupportedVersion,
                "Protocol version " + std::to_string(version) + " is unsupported");
        return success(ClientMessage{ Hello{ version } });
    }

    if (command == "AUTH")
    {
        if (fields.size() < 2)
            return failure<ClientMessage>(Error::WrongFieldCount, "AUTH requires a password field");
        std::string password = rest_after_command(payload);
        if (password.size() > 63 ||
            std::any_of(password.begin(), password.end(), [](unsigned char ch)
                {
                    return std::iscntrl(ch) != 0;
                }))
        {
            return failure<ClientMessage>(Error::InvalidField,
                "AUTH password is oversized or contains control characters");
        }
        return success(ClientMessage{ Authenticate{ std::move(password) } });
    }

    if (command == "IDENTIFY")
    {
        if (fields.size() != 3)
            return failure<ClientMessage>(Error::WrongFieldCount, "IDENTIFY requires nickname and public key fields");
        if (!is_valid_nickname(fields[1]) || !is_hex(fields[2], 64))
            return failure<ClientMessage>(Error::InvalidField, "IDENTIFY contains an invalid nickname or public key");
        return success(ClientMessage{ Identify{ std::string(fields[1]), std::string(fields[2]) } });
    }

    if (command == "PROOF")
    {
        if (fields.size() != 2)
            return failure<ClientMessage>(Error::WrongFieldCount, "PROOF requires one signature field");
        if (!is_hex(fields[1], 128))
            return failure<ClientMessage>(Error::InvalidField, "PROOF signature is invalid");
        return success(ClientMessage{ IdentityProof{ std::string(fields[1]) } });
    }

    if (command == "LEAVE")
    {
        if (fields.size() != 1)
            return failure<ClientMessage>(Error::WrongFieldCount, "LEAVE does not accept fields");
        return success(ClientMessage{ Leave{} });
    }

    if (command == "DISCONNECT" || command == "ROOMS")
    {
        if (fields.size() != 1)
            return failure<ClientMessage>(Error::WrongFieldCount,
                std::string(command) + " does not accept fields");
        if (command == "DISCONNECT")
            return success(ClientMessage{ Disconnect{} });
        return success(ClientMessage{ ListRooms{} });
    }

    if (command == "CREATE" || command == "JOIN")
    {
        if (fields.size() != 2)
            return failure<ClientMessage>(Error::WrongFieldCount,
                std::string(command) + " requires one room name");
        if (!is_valid_room_name(fields[1]))
            return failure<ClientMessage>(Error::InvalidField,
                std::string(command) + " contains an invalid room name");
        if (command == "CREATE")
            return success(ClientMessage{ CreateRoom{ std::string(fields[1]) } });
        return success(ClientMessage{ JoinRoom{ std::string(fields[1]) } });
    }

    if (command == "TOPIC")
    {
        if (fields.size() == 1)
            return success(ClientMessage{ TopicRequest{ std::nullopt } });
        std::string topic = rest_after_command(payload);
        if (topic.empty() || !is_valid_room_topic(topic))
            return failure<ClientMessage>(Error::InvalidField,
                "TOPIC text is empty, oversized, or contains control characters");
        return success(ClientMessage{ TopicRequest{ std::move(topic) } });
    }

    if (command == "MSG")
    {
        if (fields.size() < 2)
            return failure<ClientMessage>(Error::WrongFieldCount, "MSG requires a body field");
        std::string body = rest_after_command(payload);
        if (!is_valid_text(body, MAX_CHAT_LENGTH))
            return failure<ClientMessage>(Error::InvalidField, "MSG body is empty, oversized, or contains control characters");
        return success(ClientMessage{ Chat{ std::move(body) } });
    }

    if (command == "WHISPER")
    {
        if (fields.size() < 3)
            return failure<ClientMessage>(Error::WrongFieldCount, "WHISPER requires target and body fields");
        const std::size_t body_start = payload.find('|', payload.find('|') + 1);
        std::string body(payload.substr(body_start + 1));
        if (!is_valid_nickname(fields[1]) || !is_valid_text(body, MAX_CHAT_LENGTH))
            return failure<ClientMessage>(Error::InvalidField, "WHISPER contains an invalid target or body");
        return success(ClientMessage{ Whisper{ std::string(fields[1]), std::move(body) } });
    }

    if (command == "KICK")
    {
        if (fields.size() < 2 || !is_valid_nickname(fields[1]))
            return failure<ClientMessage>(Error::InvalidField,
                "KICK requires a valid nickname");
        std::string reason;
        if (fields.size() > 2)
        {
            reason = rest_after_delimiter(payload, 2);
            if (!reason.empty() && !is_valid_text(reason, MAX_CHAT_LENGTH))
                return failure<ClientMessage>(Error::InvalidField,
                    "KICK reason is oversized or contains control characters");
        }
        return success(ClientMessage{
            KickRequest{ std::string(fields[1]), std::move(reason) } });
    }

    return failure<ClientMessage>(Error::UnknownMessage,
        "Unknown client message type: " + std::string(command));
}

Result<ServerMessage> parse_server_message(std::string_view payload)
{
    if (payload.empty())
        return failure<ServerMessage>(Error::EmptyPayload, "Server message is empty");

    const auto fields = split_fields(payload);
    const std::string_view command = fields.front();

    if (command == "HELLO")
    {
        if (fields.size() != 2)
            return failure<ServerMessage>(Error::WrongFieldCount, "HELLO requires one version field");
        unsigned version = 0;
        const auto parsed = std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(), version);
        if (parsed.ec != std::errc{} || parsed.ptr != fields[1].data() + fields[1].size())
            return failure<ServerMessage>(Error::InvalidField, "HELLO version must be an integer");
        if (version != VERSION)
            return failure<ServerMessage>(Error::UnsupportedVersion,
                "Protocol version " + std::to_string(version) + " is unsupported");
        return success(ServerMessage{ Hello{ version } });
    }

    if (command == "AUTH_REQUIRED" || command == "AUTH_OK" || command == "AUTH_FAIL" ||
        command == "JOIN_OK" || command == "BANNED" || command == "NICK_TAKEN")
    {
        if (fields.size() != 1)
            return failure<ServerMessage>(Error::WrongFieldCount,
                std::string(command) + " does not accept fields");
        if (command == "AUTH_REQUIRED") return success(ServerMessage{ AuthRequired{} });
        if (command == "AUTH_OK") return success(ServerMessage{ AuthAccepted{} });
        if (command == "AUTH_FAIL") return success(ServerMessage{ AuthRejected{} });
        if (command == "JOIN_OK") return success(ServerMessage{ JoinAccepted{} });
        if (command == "BANNED") return success(ServerMessage{ Banned{} });
        return success(ServerMessage{ NicknameTaken{} });
    }

    if (command == "CHALLENGE")
    {
        if (fields.size() != 2 || !is_hex(fields[1], 64))
            return failure<ServerMessage>(Error::InvalidField, "CHALLENGE must contain 32 bytes of hexadecimal data");
        return success(ServerMessage{ Challenge{ std::string(fields[1]) } });
    }

    if (command == "IDENTITY_OK")
    {
        if (fields.size() != 2 || !is_hex(fields[1], 16))
            return failure<ServerMessage>(Error::InvalidField, "IDENTITY_OK fingerprint is invalid");
        return success(ServerMessage{ IdentityAccepted{ std::string(fields[1]) } });
    }

    if (command == "USERS")
    {
        if (fields.size() != 3)
            return failure<ServerMessage>(Error::WrongFieldCount, "USERS requires room and list fields");
        std::uint64_t room_id = 0;
        if (!parse_unsigned(fields[1], room_id) || room_id == 0)
            return failure<ServerMessage>(Error::InvalidField, "USERS contains an invalid room ID");
        if (!fields[2].empty() && fields[2].back() == ',')
            return failure<ServerMessage>(Error::InvalidField, "USERS contains an empty nickname");
        UserList users;
        users.room_id = room_id;
        std::size_t start = 0;
        while (start < fields[2].size())
        {
            const std::size_t comma = fields[2].find(',', start);
            const std::string_view nickname = fields[2].substr(
                start, comma == std::string_view::npos ? comma : comma - start);
            if (!is_valid_nickname(nickname))
                return failure<ServerMessage>(Error::InvalidField, "USERS contains an invalid nickname");
            users.nicknames.emplace_back(nickname);
            if (comma == std::string_view::npos) break;
            start = comma + 1;
        }
        return success(ServerMessage{ std::move(users) });
    }

    if (command == "ROOMS")
    {
        if (fields.size() != 2)
            return failure<ServerMessage>(Error::WrongFieldCount, "ROOMS requires one list field");
        RoomList list;
        std::size_t start = 0;
        while (start < fields[1].size())
        {
            const std::size_t semicolon = fields[1].find(';', start);
            const std::string_view entry = fields[1].substr(
                start, semicolon == std::string_view::npos ? semicolon : semicolon - start);
            const std::size_t first_comma = entry.find(',');
            const std::size_t second_comma = first_comma == std::string_view::npos
                ? std::string_view::npos : entry.find(',', first_comma + 1);
            if (first_comma == std::string_view::npos || second_comma == std::string_view::npos)
                return failure<ServerMessage>(Error::InvalidField, "ROOMS contains a malformed entry");

            std::uint64_t id = 0;
            std::uint64_t count = 0;
            const std::string_view name = entry.substr(
                first_comma + 1, second_comma - first_comma - 1);
            if (!parse_unsigned(entry.substr(0, first_comma), id) || id == 0 ||
                !is_valid_room_name(name) ||
                !parse_unsigned(entry.substr(second_comma + 1), count))
            {
                return failure<ServerMessage>(Error::InvalidField, "ROOMS contains invalid room data");
            }
            list.rooms.push_back(RoomSummary{
                id, std::string(name), static_cast<std::size_t>(count) });
            if (list.rooms.size() > MAX_ROOMS)
                return failure<ServerMessage>(Error::InvalidField, "ROOMS contains too many rooms");
            if (semicolon == std::string_view::npos) break;
            start = semicolon + 1;
        }
        return success(ServerMessage{ std::move(list) });
    }

    if (command == "ROOM")
    {
        if (fields.size() < 4)
            return failure<ServerMessage>(Error::WrongFieldCount,
                "ROOM requires ID, name, and topic fields");
        std::uint64_t id = 0;
        std::string topic = rest_after_delimiter(payload, 3);
        if (!parse_unsigned(fields[1], id) || id == 0 ||
            !is_valid_room_name(fields[2]) || !is_valid_room_topic(topic))
        {
            return failure<ServerMessage>(Error::InvalidField, "ROOM contains invalid room data");
        }
        return success(ServerMessage{
            RoomJoined{ id, std::string(fields[2]), std::move(topic) } });
    }

    if (command == "TOPIC")
    {
        if (fields.size() < 3)
            return failure<ServerMessage>(Error::WrongFieldCount,
                "TOPIC requires room and topic fields");
        std::uint64_t id = 0;
        std::string topic = rest_after_delimiter(payload, 2);
        if (!parse_unsigned(fields[1], id) || id == 0 || !is_valid_room_topic(topic))
            return failure<ServerMessage>(Error::InvalidField, "TOPIC contains invalid room data");
        return success(ServerMessage{ RoomTopic{ id, std::move(topic) } });
    }

    if (command == "ROOM_TEXT")
    {
        if (fields.size() < 3)
            return failure<ServerMessage>(Error::WrongFieldCount,
                "ROOM_TEXT requires room and text fields");
        std::uint64_t id = 0;
        std::string body = rest_after_delimiter(payload, 2);
        if (!parse_unsigned(fields[1], id) || id == 0 ||
            !is_valid_text(body, MAX_FRAME_LENGTH))
        {
            return failure<ServerMessage>(Error::InvalidField, "ROOM_TEXT contains invalid room data");
        }
        return success(ServerMessage{ RoomText{ id, std::move(body) } });
    }

    if (command == "KICK")
    {
        if (fields.size() < 2 || !is_valid_nickname(fields[1]))
            return failure<ServerMessage>(Error::InvalidField, "KICK requires a valid nickname");
        std::string reason;
        if (fields.size() > 2)
        {
            const std::size_t reason_start = payload.find('|', payload.find('|') + 1);
            reason = std::string(payload.substr(reason_start + 1));
            if (!reason.empty() && !is_valid_text(reason, MAX_CHAT_LENGTH))
                return failure<ServerMessage>(Error::InvalidField,
                    "KICK reason is oversized or contains control characters");
        }
        return success(ServerMessage{ Kick{ std::string(fields[1]), std::move(reason) } });
    }

    if (command == "JOIN_FAIL" || command == "TEXT" || command == "ERROR")
    {
        if (fields.size() < 2)
            return failure<ServerMessage>(Error::WrongFieldCount,
                std::string(command) + " requires a text field");
        std::string body = rest_after_command(payload);
        if (!is_valid_text(body, MAX_FRAME_LENGTH))
            return failure<ServerMessage>(Error::InvalidField,
                std::string(command) + " text is empty, oversized, or contains control characters");
        if (command == "JOIN_FAIL") return success(ServerMessage{ JoinRejected{ std::move(body) } });
        if (command == "TEXT") return success(ServerMessage{ Text{ std::move(body) } });
        return success(ServerMessage{ ServerError{ std::move(body) } });
    }

    return failure<ServerMessage>(Error::UnknownMessage,
        "Unknown server message type: " + std::string(command));
}

Result<std::string> encode_client_frame(const ClientMessage& message)
{
    const std::string payload = serialize(message);
    const auto validation = parse_client_message(payload);
    if (!validation)
        return failure<std::string>(validation.error, validation.detail);
    return encode_frame(payload);
}

Result<std::string> encode_server_frame(const ServerMessage& message)
{
    const std::string payload = serialize(message);
    const auto validation = parse_server_message(payload);
    if (!validation)
        return failure<std::string>(validation.error, validation.detail);
    return encode_frame(payload);
}

Result<ClientMessage> decode_client_frame(std::string_view frame)
{
    return decode_message_frame<ClientMessage>(frame, parse_client_message);
}

Result<ServerMessage> decode_server_frame(std::string_view frame)
{
    return decode_message_frame<ServerMessage>(frame, parse_server_message);
}
}
