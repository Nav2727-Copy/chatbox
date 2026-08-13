#pragma once

#include <boost/asio/ssl.hpp>

#include <cstdint>
#include <string>

namespace chatbox::tls
{
struct ServerConfig
{
    bool enabled = false;
    std::string certificate_path;
    std::string private_key_path;
};

struct ClientConfig
{
    bool enabled = true;
    std::string ca_path;
    std::string expected_fingerprint;
    std::string trust_store_path = "trusted_fingerprints.txt";
    bool trust_on_first_use = true;
};

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
