#pragma once

#include <boost/asio/ssl.hpp>

#include <cstdint>
#include <string>
#include <functional>

namespace chatbox::tls
{
struct ServerConfig
{
    bool enabled = false;
    std::string certificate_path;
    std::string private_key_path;
};

std::string default_trust_store_path();
void forget_certificate(const std::string& host, std::uint16_t port, const std::string& path);

struct ClientConfig
{
    bool enabled = true;
    std::string ca_path;
    std::string expected_fingerprint;
    std::string trust_store_path = default_trust_store_path();
    bool trust_on_first_use = true;
    std::function<bool()> continue_waiting; // false cancels a directory request
    // Called on the connection executor before any application traffic is sent.
    // Previous is nonempty for a changed certificate; acceptance replaces it.
    std::function<bool(const std::string& address, const std::string& fingerprint,
        const std::string& previous)> confirm_certificate;
};

// Create once, then validate and reuse. Never overwrite an existing pair.
ServerConfig ensure_host_certificate(const std::string& directory);

void configure_server_context(boost::asio::ssl::context& context,
    const ServerConfig& config);
void configure_client_context(boost::asio::ssl::context& context,
    const ClientConfig& config);
void set_server_name(SSL* handle, const std::string& host);

std::string peer_sha256_fingerprint(SSL* handle);
std::string certificate_sha256_fingerprint(const std::string& certificate_path);
bool verify_peer(SSL* handle,
    const std::string& host,
    std::uint16_t port,
    const ClientConfig& config,
    std::string& notice,
    std::string& error);

std::string normalize_fingerprint(const std::string& fingerprint);
}
