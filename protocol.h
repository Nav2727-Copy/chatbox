#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chatbox::protocol
{
inline constexpr unsigned VERSION = 2;
inline constexpr std::size_t MAX_FRAME_LENGTH = 4096;
inline constexpr std::size_t MAX_CHAT_LENGTH = 1000;

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

struct Chat
{
    std::string body;
};

struct Whisper
{
    std::string target;
    std::string body;
};

using ClientMessage = std::variant<
    Hello,
    Authenticate,
    Identify,
    IdentityProof,
    Leave,
    Chat,
    Whisper>;

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
    std::vector<std::string> nicknames;
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
    Text,
    ServerError>;

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
