#include "tls.h"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace chatbox::tls
{
namespace
{
std::mutex trust_store_mutex;

std::string openssl_error(const std::string& prefix)
{
    const unsigned long code = ERR_get_error();
    if (code == 0)
        return prefix;
    char detail[256] = {};
    ERR_error_string_n(code, detail, sizeof(detail));
    return prefix + ": " + detail;
}

std::string address_key(const std::string& host, std::uint16_t port)
{
    return host + ":" + std::to_string(port);
}

std::map<std::string, std::string> read_trust_store(const std::string& path)
{
    std::map<std::string, std::string> entries;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line))
    {
        const auto separator = line.find('|');
        if (separator == std::string::npos)
            continue;
        const std::string address = line.substr(0, separator);
        const std::string fingerprint = normalize_fingerprint(
            line.substr(separator + 1));
        if (!address.empty() && !fingerprint.empty())
            entries[address] = fingerprint;
    }
    return entries;
}

bool write_trust_store(const std::string& path,
    const std::map<std::string, std::string>& entries,
    std::string& error)
{
    std::ofstream file(path, std::ios::trunc);
    if (!file)
    {
        error = "Could not write TLS trust store '" + path + "'";
        return false;
    }
    for (const auto& [address, fingerprint] : entries)
        file << address << '|' << fingerprint << '\n';
    if (!file)
    {
        error = "Could not finish writing TLS trust store '" + path + "'";
        return false;
    }
    return true;
}

bool certificate_matches_host(X509* certificate, const std::string& host)
{
    if (host.empty())
        return false;
    if (X509_check_ip_asc(certificate, host.c_str(), 0) == 1)
        return true;
    return X509_check_host(certificate, host.c_str(), host.size(),
        X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS, nullptr) == 1;
}
}

void configure_server_context(boost::asio::ssl::context& context,
    const ServerConfig& config)
{
    if (!config.enabled)
        return;
    if (config.certificate_path.empty() || config.private_key_path.empty())
        throw std::runtime_error(
            "TLS requires both a certificate path and a private-key path");

    context.set_options(boost::asio::ssl::context::default_workarounds
        | boost::asio::ssl::context::no_sslv2
        | boost::asio::ssl::context::no_sslv3
        | boost::asio::ssl::context::no_tlsv1
        | boost::asio::ssl::context::no_tlsv1_1
        | boost::asio::ssl::context::single_dh_use);
    context.use_certificate_chain_file(config.certificate_path);
    context.use_private_key_file(config.private_key_path,
        boost::asio::ssl::context::pem);
    if (SSL_CTX_check_private_key(context.native_handle()) != 1)
        throw std::runtime_error(openssl_error(
            "TLS private key does not match the certificate"));
}

void configure_client_context(boost::asio::ssl::context& context,
    const ClientConfig& config)
{
    if (!config.enabled)
        return;
    context.set_options(boost::asio::ssl::context::default_workarounds
        | boost::asio::ssl::context::no_sslv2
        | boost::asio::ssl::context::no_sslv3
        | boost::asio::ssl::context::no_tlsv1
        | boost::asio::ssl::context::no_tlsv1_1);
    if (config.ca_path.empty())
        context.set_default_verify_paths();
    else
        context.load_verify_file(config.ca_path);

    // Complete the handshake so a self-hosted certificate can be evaluated
    // against an explicit pin or the TOFU store after OpenSSL builds its chain.
    context.set_verify_mode(boost::asio::ssl::verify_peer);
    context.set_verify_callback(
        [](bool, boost::asio::ssl::verify_context&) { return true; });
}

void set_server_name(SSL* handle, const std::string& host)
{
    if (host.empty())
        return;
    if (SSL_set_tlsext_host_name(handle, host.c_str()) != 1)
        throw std::runtime_error(openssl_error("Could not set TLS server name"));
}

std::string normalize_fingerprint(const std::string& fingerprint)
{
    std::string normalized;
    normalized.reserve(fingerprint.size());
    for (unsigned char ch : fingerprint)
    {
        if (std::isxdigit(ch))
            normalized.push_back(static_cast<char>(std::toupper(ch)));
        else if (ch != ':' && ch != '-' && !std::isspace(ch))
            return {};
    }
    if (normalized.size() != SHA256_DIGEST_LENGTH * 2)
        return {};
    return normalized;
}

std::string peer_sha256_fingerprint(SSL* handle)
{
    X509* certificate = SSL_get1_peer_certificate(handle);
    if (!certificate)
        return {};

    unsigned char digest[EVP_MAX_MD_SIZE] = {};
    unsigned int digest_size = 0;
    const int result = X509_digest(certificate, EVP_sha256(), digest, &digest_size);
    X509_free(certificate);
    if (result != 1)
        return {};

    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < digest_size; ++index)
        out << std::setw(2) << static_cast<unsigned int>(digest[index]);
    return out.str();
}

std::string certificate_sha256_fingerprint(const std::string& certificate_path)
{
    BIO* input = BIO_new_file(certificate_path.c_str(), "r");
    if (!input)
        return {};
    X509* certificate = PEM_read_bio_X509(input, nullptr, nullptr, nullptr);
    BIO_free(input);
    if (!certificate)
        return {};

    unsigned char digest[EVP_MAX_MD_SIZE] = {};
    unsigned int digest_size = 0;
    const int result = X509_digest(certificate, EVP_sha256(), digest, &digest_size);
    X509_free(certificate);
    if (result != 1)
        return {};

    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < digest_size; ++index)
        out << std::setw(2) << static_cast<unsigned int>(digest[index]);
    return out.str();
}

bool verify_peer(SSL* handle,
    const std::string& host,
    std::uint16_t port,
    const ClientConfig& config,
    std::string& notice,
    std::string& error)
{
    X509* certificate = SSL_get1_peer_certificate(handle);
    if (!certificate)
    {
        error = "TLS server did not present a certificate";
        return false;
    }

    const std::string fingerprint = peer_sha256_fingerprint(handle);
    const long verify_result = SSL_get_verify_result(handle);
    const bool host_matches = certificate_matches_host(certificate, host);
    X509_free(certificate);

    if (fingerprint.empty())
    {
        error = "Could not calculate the TLS certificate SHA-256 fingerprint";
        return false;
    }

    if (!config.expected_fingerprint.empty())
    {
        const std::string expected = normalize_fingerprint(
            config.expected_fingerprint);
        if (expected.empty())
        {
            error = "The configured TLS fingerprint is not a 64-digit SHA-256 value";
            return false;
        }
        if (expected != fingerprint)
        {
            error = "TLS certificate fingerprint mismatch for "
                + address_key(host, port) + "; expected " + expected
                + " but received " + fingerprint;
            return false;
        }
        notice = "TLS certificate matched the configured SHA-256 fingerprint "
            + fingerprint;
        return true;
    }

    if (verify_result == X509_V_OK && host_matches)
    {
        notice = "TLS certificate verified by a trusted CA";
        return true;
    }

    if (!config.trust_on_first_use || config.trust_store_path.empty())
    {
        if (verify_result != X509_V_OK)
            error = "TLS certificate validation failed: "
                + std::string(X509_verify_cert_error_string(verify_result));
        else
            error = "TLS certificate is valid but does not match host '" + host + "'";
        error += ". Configure a trusted CA or an explicit SHA-256 fingerprint.";
        return false;
    }

    std::lock_guard lock(trust_store_mutex);
    auto entries = read_trust_store(config.trust_store_path);
    const std::string key = address_key(host, port);
    const auto existing = entries.find(key);
    if (existing != entries.end())
    {
        if (existing->second != fingerprint)
        {
            error = "BLOCKED: TLS certificate for " + key
                + " changed. Previously trusted " + existing->second
                + ", received " + fingerprint
                + ". Verify the server operator before replacing the stored fingerprint.";
            return false;
        }
        notice = "TLS certificate matched the trusted fingerprint " + fingerprint;
        return true;
    }

    entries[key] = fingerprint;
    if (!write_trust_store(config.trust_store_path, entries, error))
        return false;
    notice = "Trusted this self-hosted TLS certificate on first use: "
        + fingerprint;
    return true;
}
}
