#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chatbox::protocol
{
inline constexpr unsigned VERSION = 3;
inline constexpr std::size_t MAX_FRAME_LENGTH = 4096;
inline constexpr std::size_t MAX_CHAT_LENGTH = 1000;
inline constexpr std::size_t MAX_ROOM_NAME_LENGTH = 32;
inline constexpr std::size_t MAX_ROOM_TOPIC_LENGTH = 200;
inline constexpr std::size_t MAX_ROOMS = 32;

using RoomId = std::uint64_t;

enum class Error
{
    None,
    EmptyFrame,
    FrameTooLarge,
    InvalidBase64,
    EmptyPayload,
    UnknownMessage,
    WrongFieldCount,
    InvalidField,
    UnsupportedVersion
};

template <typename T>
struct Result
{
    std::optional<T> value;
    Error error = Error::None;
    std::string detail;

    explicit operator bool() const { return value.has_value(); }
};

struct Hello
{
    unsigned version = VERSION;
};

struct Authenticate
{
    std::string password;
};

struct Identify
{
    std::string nickname;
    std::string public_key_hex;
};

struct IdentityProof
{
    std::string signature_hex;
};

struct Leave {};

struct Disconnect {};

struct ListRooms {};

struct CreateRoom
{
    std::string name;
};

struct JoinRoom
{
    std::string name;
};

struct TopicRequest
{
    std::optional<std::string> topic;
};

struct Chat
{
    std::string body;
};

struct Whisper
{
    std::string target;
    std::string body;
};

struct KickRequest
{
    std::string nickname;
    std::string reason;
};

using ClientMessage = std::variant<
    Hello,
    Authenticate,
    Identify,
    IdentityProof,
    Leave,
    Disconnect,
    ListRooms,
    CreateRoom,
    JoinRoom,
    TopicRequest,
    Chat,
    Whisper,
    KickRequest>;

struct AuthRequired {};
struct AuthAccepted {};
struct AuthRejected {};

struct Challenge
{
    std::string challenge_hex;
};

struct IdentityAccepted
{
    std::string fingerprint;
};

struct JoinAccepted {};
struct Banned {};
struct NicknameTaken {};

struct JoinRejected
{
    std::string reason;
};

struct Kick
{
    std::string nickname;
    std::string reason;
};

struct UserList
{
    RoomId room_id = 0;
    std::vector<std::string> nicknames;
};

struct RoomSummary
{
    RoomId id = 0;
    std::string name;
    std::size_t user_count = 0;
};

struct RoomList
{
    std::vector<RoomSummary> rooms;
};

struct RoomJoined
{
    RoomId id = 0;
    std::string name;
    std::string topic;
};

struct RoomTopic
{
    RoomId room_id = 0;
    std::string topic;
};

struct RoomText
{
    RoomId room_id = 0;
    std::string body;
};

struct Text
{
    std::string body;
};

struct ServerError
{
    std::string reason;
};

using ServerMessage = std::variant<
    Hello,
    AuthRequired,
    AuthAccepted,
    AuthRejected,
    Challenge,
    IdentityAccepted,
    JoinAccepted,
    Banned,
    NicknameTaken,
    JoinRejected,
    Kick,
    UserList,
    RoomList,
    RoomJoined,
    RoomTopic,
    RoomText,
    Text,
    ServerError>;

bool is_valid_room_name(std::string_view name);
bool is_valid_room_topic(std::string_view topic);

std::string_view error_name(Error error);

Result<std::string> encode_frame(std::string_view payload);
Result<std::string> decode_frame(std::string_view frame);

std::string serialize(const ClientMessage& message);
std::string serialize(const ServerMessage& message);
Result<ClientMessage> parse_client_message(std::string_view payload);
Result<ServerMessage> parse_server_message(std::string_view payload);

Result<std::string> encode_client_frame(const ClientMessage& message);
Result<std::string> encode_server_frame(const ServerMessage& message);
Result<ClientMessage> decode_client_frame(std::string_view frame);
Result<ServerMessage> decode_server_frame(std::string_view frame);
}
