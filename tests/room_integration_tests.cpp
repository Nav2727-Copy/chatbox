#include "chat_server.h"
#include "protocol.h"
#include "utils.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace protocol = chatbox::protocol;
using namespace std::chrono_literals;

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

class WireClient
{
public:
    WireClient(uint16_t port, std::string nickname)
        : socket_(io_), nickname_(std::move(nickname))
    {
        crypto_sign_keypair(identity_.public_key.data(), identity_.secret_key.data());
        identity_.public_key_hex = bytes_to_hex(
            identity_.public_key.data(), identity_.public_key.size());
        socket_.connect(tcp::endpoint(boost::asio::ip::address_v6::loopback(), port));
        socket_.non_blocking(true);
    }

    bool authenticate()
    {
        if (!wait_for<protocol::Hello>()) return false;
        if (!send(protocol::ClientMessage{ protocol::Hello{ protocol::VERSION } })) return false;
        if (!wait_for<protocol::AuthAccepted>()) return false;
        if (!send(protocol::ClientMessage{
            protocol::Identify{ nickname_, identity_.public_key_hex } })) return false;

        auto challenge = wait_for<protocol::Challenge>();
        if (!challenge) return false;
        if (!send(protocol::ClientMessage{ protocol::IdentityProof{
            sign_identity_challenge(identity_, challenge->challenge_hex) } })) return false;
        if (!wait_for<protocol::JoinAccepted>()) return false;
        auto room = wait_for<protocol::RoomJoined>();
        return room && room->id == ChatServer::DEFAULT_ROOM_ID && room->name == "lobby";
    }

    bool send(const protocol::ClientMessage& message)
    {
        const auto encoded = protocol::encode_client_frame(message);
        if (!encoded) return false;

        std::size_t offset = 0;
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (offset < encoded.value->size() &&
            std::chrono::steady_clock::now() < deadline)
        {
            boost::system::error_code ec;
            const std::size_t written = socket_.write_some(boost::asio::buffer(
                encoded.value->data() + offset, encoded.value->size() - offset), ec);
            if (!ec)
            {
                offset += written;
                continue;
            }
            if (ec != boost::asio::error::would_block &&
                ec != boost::asio::error::try_again)
                return false;
            std::this_thread::sleep_for(1ms);
        }
        return offset == encoded.value->size();
    }

    std::optional<protocol::ServerMessage> next(
        std::chrono::milliseconds timeout = 1s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const std::size_t newline = incoming_.find('\n');
            if (newline != std::string::npos)
            {
                std::string frame = incoming_.substr(0, newline + 1);
                incoming_.erase(0, newline + 1);
                auto decoded = protocol::decode_server_frame(frame);
                if (!decoded)
                {
                    std::cerr << "invalid server frame: " << decoded.detail << '\n';
                    ++failures;
                    continue;
                }
                return std::move(*decoded.value);
            }

            std::array<char, 4096> chunk{};
            boost::system::error_code ec;
            const std::size_t received = socket_.read_some(boost::asio::buffer(chunk), ec);
            if (!ec)
            {
                incoming_.append(chunk.data(), received);
                continue;
            }
            if (ec != boost::asio::error::would_block &&
                ec != boost::asio::error::try_again)
                return std::nullopt;
            std::this_thread::sleep_for(1ms);
        }
        return std::nullopt;
    }

    template <typename T>
    std::optional<T> wait_for(
        std::chrono::milliseconds timeout = 1s,
        std::vector<protocol::ServerMessage>* before = nullptr)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            auto message = next(remaining);
            if (!message) return std::nullopt;
            if (const auto* wanted = std::get_if<T>(&*message))
                return *wanted;
            if (before) before->push_back(std::move(*message));
        }
        return std::nullopt;
    }

    void drain()
    {
        while (next(20ms)) {}
    }

private:
    boost::asio::io_context io_;
    tcp::socket socket_;
    std::string nickname_;
    ClientIdentity identity_;
    std::string incoming_;
};

bool contains_room_text(const std::vector<protocol::ServerMessage>& messages,
    protocol::RoomId room_id,
    const std::string& text)
{
    for (const auto& message : messages)
    {
        const auto* room_text = std::get_if<protocol::RoomText>(&message);
        if (room_text && room_text->room_id == room_id &&
            room_text->body.find(text) != std::string::npos)
            return true;
    }
    return false;
}

bool contains_direct_text(const std::vector<protocol::ServerMessage>& messages,
    const std::string& text)
{
    for (const auto& message : messages)
    {
        const auto* direct = std::get_if<protocol::Text>(&message);
        if (direct && direct->body.find(text) != std::string::npos)
            return true;
    }
    return false;
}

void multiple_rooms_are_isolated()
{
    boost::asio::io_context server_io;
    ChatServer server(server_io, 0);
    server.load_identities("");
    std::thread server_thread([&] { server_io.run(); });

    WireClient alice(server.port(), "alice");
    WireClient bob(server.port(), "bob");
    CHECK(alice.authenticate());
    CHECK(bob.authenticate());
    alice.drain();
    bob.drain();

    CHECK(alice.send(protocol::ClientMessage{ protocol::CreateRoom{ "red" } }));
    auto red = alice.wait_for<protocol::RoomJoined>();
    CHECK(red);
    CHECK(red && red->name == "red");
    const protocol::RoomId red_id = red ? red->id : 0;
    alice.drain();
    bob.drain();

    CHECK(alice.send(protocol::ClientMessage{ protocol::Chat{ "secret-red" } }));
    auto red_echo = alice.wait_for<protocol::RoomText>();
    CHECK(red_echo && red_echo->room_id == red_id);
    CHECK(red_echo && red_echo->body.find("secret-red") != std::string::npos);

    CHECK(bob.send(protocol::ClientMessage{ protocol::ListRooms{} }));
    std::vector<protocol::ServerMessage> before_bob_rooms;
    CHECK(bob.wait_for<protocol::RoomList>(1s, &before_bob_rooms));
    CHECK(!contains_room_text(before_bob_rooms, red_id, "secret-red"));

    CHECK(bob.send(protocol::ClientMessage{ protocol::Chat{ "lobby-only" } }));
    auto lobby_echo = bob.wait_for<protocol::RoomText>();
    CHECK(lobby_echo && lobby_echo->room_id == ChatServer::DEFAULT_ROOM_ID);
    CHECK(lobby_echo && lobby_echo->body.find("lobby-only") != std::string::npos);

    CHECK(alice.send(protocol::ClientMessage{ protocol::ListRooms{} }));
    std::vector<protocol::ServerMessage> before_alice_rooms;
    CHECK(alice.wait_for<protocol::RoomList>(1s, &before_alice_rooms));
    CHECK(!contains_room_text(before_alice_rooms,
        ChatServer::DEFAULT_ROOM_ID, "lobby-only"));

    CHECK(alice.send(protocol::ClientMessage{
        protocol::Whisper{ "bob", "private-red" } }));
    auto whisper_error = alice.wait_for<protocol::Text>();
    CHECK(whisper_error);
    CHECK(whisper_error &&
        whisper_error->body.find("not in this room") != std::string::npos);
    CHECK(bob.send(protocol::ClientMessage{ protocol::ListRooms{} }));
    std::vector<protocol::ServerMessage> before_whisper_barrier;
    CHECK(bob.wait_for<protocol::RoomList>(1s, &before_whisper_barrier));
    CHECK(!contains_direct_text(before_whisper_barrier, "private-red"));

    CHECK(bob.send(protocol::ClientMessage{ protocol::JoinRoom{ "red" } }));
    auto bob_red = bob.wait_for<protocol::RoomJoined>();
    CHECK(bob_red && bob_red->id == red_id);
    std::vector<protocol::ServerMessage> red_history;
    CHECK(bob.wait_for<protocol::RoomList>(1s, &red_history));
    CHECK(contains_room_text(red_history, red_id, "secret-red"));
    CHECK(!contains_room_text(red_history,
        ChatServer::DEFAULT_ROOM_ID, "lobby-only"));
    const protocol::UserList* red_users = nullptr;
    for (const auto& message : red_history)
    {
        if (const auto* users = std::get_if<protocol::UserList>(&message))
            red_users = users;
    }
    CHECK(red_users);
    if (red_users)
    {
        CHECK(red_users->room_id == red_id);
        CHECK(red_users->nicknames.size() == 2);
    }

    CHECK(alice.send(protocol::ClientMessage{ protocol::Chat{ "shared-red" } }));
    auto shared = bob.wait_for<protocol::RoomText>();
    CHECK(shared && shared->room_id == red_id);
    CHECK(shared && shared->body.find("shared-red") != std::string::npos);

    CHECK(bob.send(protocol::ClientMessage{
        protocol::TopicRequest{ std::string("not allowed") } }));
    auto topic_denied = bob.wait_for<protocol::ServerError>();
    CHECK(topic_denied);
    CHECK(topic_denied && topic_denied->reason.find("creator") != std::string::npos);

    CHECK(alice.send(protocol::ClientMessage{
        protocol::TopicRequest{ std::string("red topic") } }));
    auto topic_update = bob.wait_for<protocol::RoomTopic>();
    CHECK(topic_update && topic_update->room_id == red_id);
    CHECK(topic_update && topic_update->topic == "red topic");

    CHECK(bob.send(protocol::ClientMessage{
        protocol::KickRequest{ "alice", "nope" } }));
    auto kick_denied = bob.wait_for<protocol::ServerError>();
    CHECK(kick_denied);
    CHECK(kick_denied && kick_denied->reason.find("creator") != std::string::npos);

    CHECK(alice.send(protocol::ClientMessage{ protocol::Leave{} }));
    auto alice_lobby = alice.wait_for<protocol::RoomJoined>();
    CHECK(alice_lobby && alice_lobby->id == ChatServer::DEFAULT_ROOM_ID);
    std::vector<protocol::ServerMessage> lobby_history;
    CHECK(alice.wait_for<protocol::RoomList>(1s, &lobby_history));
    CHECK(contains_room_text(lobby_history,
        ChatServer::DEFAULT_ROOM_ID, "lobby-only"));
    CHECK(!contains_room_text(lobby_history, red_id, "secret-red"));

    alice.send(protocol::ClientMessage{ protocol::Disconnect{} });
    bob.send(protocol::ClientMessage{ protocol::Disconnect{} });
    std::this_thread::sleep_for(20ms);
    server.stop();
    server_io.stop();
    server_thread.join();
}
}

int main()
{
    if (sodium_init() < 0)
        return EXIT_FAILURE;

    multiple_rooms_are_isolated();
    if (failures != 0)
    {
        std::cerr << failures << " room integration test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "room integration tests passed\n";
    return EXIT_SUCCESS;
}
