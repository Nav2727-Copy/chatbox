#include "chat/chat_client.h"
#include "chat/chat_server.h"
#include "network/server_browser.h"
#include <iostream>
using namespace std::chrono_literals;
namespace {
int failures = 0;
#define CHECK(value) do { if (!(value)) { std::cerr << __LINE__ << ": " << #value << '\n'; ++failures; } } while (false)

void stalled_tls(bool cancel)
{
    boost::asio::io_context io;
    tcp::acceptor listener(io, {tcp::v4(), 0});
    tcp::socket peer(io);
    listener.async_accept(peer, [](auto) {}); // Accept TCP but never speak TLS.
    ChatClient client(io);
    int completions = 0;
    bool success = true;
    client.async_connect("127.0.0.1", listener.local_endpoint().port(), [&](bool result) {
        ++completions; success = result;
    }, 100ms);
    boost::asio::steady_timer cancellation(io);
    if (cancel) {
        cancellation.expires_after(30ms);
        cancellation.async_wait([&](auto) { client.cancel_connect(); });
    }
    const auto start = std::chrono::steady_clock::now();
    io.run();
    CHECK(std::chrono::steady_clock::now() - start < 2s);
    CHECK(completions == 1);
    CHECK(!success);
    CHECK(client.connection_failure() == (cancel ? ConnectionFailure::Cancelled : ConnectionFailure::Timeout));
    CHECK(client.connection_phase() == "Negotiating TLS");
    client.cancel_connect();
    CHECK(completions == 1);
}

void cancel_resolution_and_retry()
{
    boost::asio::io_context io;
    chatbox::tls::ClientConfig plaintext;
    plaintext.enabled = false;
    {
        ChatClient client(io, plaintext);
        int completions = 0;
        client.async_connect("localhost", 7777, [&](bool success) { CHECK(!success); ++completions; });
        client.cancel_connect();
        CHECK(client.connection_failure() == ConnectionFailure::Cancelled);
        CHECK(completions == 1);
        io.poll();
    }
    // Late DNS completion must not access the destroyed attempt.
    io.restart();
    io.run();
    io.restart();
    ChatServer server(io, 0);
    ChatClient retry(io, plaintext);
    CHECK(retry.connect("127.0.0.1", server.port()));
    retry.close();
    server.stop();
    io.poll();
}

void directory_cancellation_and_disconnection()
{
    boost::asio::io_context server_io;
    tcp::acceptor listener(server_io, {tcp::v4(), 0});
    tcp::socket peer(server_io);
    listener.async_accept(peer, [](auto) {});
    std::thread server_thread([&] { server_io.run(); });
    chatbox::tls::ClientConfig config; config.enabled = false;
    const auto start = std::chrono::steady_clock::now();
    config.continue_waiting = [&] { return std::chrono::steady_clock::now() - start < 100ms; };
    std::vector<BrowserEntry> entries; std::string error;
    CHECK(!ServerBrowserClient::list_servers("127.0.0.1", listener.local_endpoint().port(), entries, error, config));
    CHECK(error.find("cancelled") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - start < 2s);
    server_thread.join();

    boost::asio::io_context io;
    ChatServer server(io, 0);
    ChatClient client(io, config);
    CHECK(client.connect("127.0.0.1", server.port()));
    CHECK(client.connected());
    server.stop(); io.run();
    CHECK(!client.connected());
    CHECK(!client.disconnect_reason().empty());
    client.send(chatbox::protocol::ClientMessage{chatbox::protocol::Disconnect{}});
    io.restart(); CHECK(io.poll() == 0); // disconnected sends cannot queue network work
}

void refused_connection_is_classified()
{
    boost::asio::io_context io;
    tcp::acceptor listener(io, {tcp::v4(), 0});
    const auto port = listener.local_endpoint().port();
    listener.close();
    ChatClient client(io);
    CHECK(!client.connect("127.0.0.1", port));
    CHECK(client.connection_failure() == ConnectionFailure::Refused);
    io.restart();
    io.run();
}
}
int main()
{
    if (sodium_init() < 0) return 1;
    directory_cancellation_and_disconnection();
    stalled_tls(false);
    stalled_tls(true);
    cancel_resolution_and_retry();
    refused_connection_is_classified();
    return failures ? 1 : 0;
}
