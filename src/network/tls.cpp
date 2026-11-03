#include "network/tls.h"
#include "common/utils.h"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <memory>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif
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
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    const std::string temporary = path + ".tmp";
    std::ofstream file(temporary, std::ios::trunc);
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
    file.close();
    if (!file) { error = "Could not close TLS trust store"; return false; }
#ifdef _WIN32
    if (!MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Could not replace TLS trust store"; return false;
    }
#else
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) { error = "Could not replace TLS trust store: " + ec.message(); return false; }
#endif
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

std::string default_trust_store_path()
{
    return (std::filesystem::path(chatbox_data_directory()) / "trusted_fingerprints.txt").string();
}

void forget_certificate(const std::string& host, std::uint16_t port, const std::string& path)
{
    std::lock_guard lock(trust_store_mutex);
    auto entries = read_trust_store(path);
    entries.erase(address_key(host, port));
    std::string error;
    if (!write_trust_store(path, entries, error)) throw std::runtime_error(error);
}

ServerConfig ensure_host_certificate(const std::string& directory)
{
    namespace fs = std::filesystem;
    const fs::path root(directory);
    ServerConfig config{true, (root / "host-cert.pem").string(), (root / "host-key.pem").string()};
    if (fs::exists(config.certificate_path) || fs::exists(config.private_key_path)) {
        // An incomplete or invalid pair requires deliberate repair, never replacement.
        boost::asio::ssl::context context(boost::asio::ssl::context::tls_server);
        configure_server_context(context, config);
        return config;
    }
    fs::create_directories(root);
#ifndef _WIN32
    fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace);
#endif
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> key_context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY* raw_key = nullptr;
    if (!key_context || EVP_PKEY_keygen_init(key_context.get()) <= 0
        || EVP_PKEY_CTX_set_ec_paramgen_curve_nid(key_context.get(), NID_X9_62_prime256v1) <= 0
        || EVP_PKEY_keygen(key_context.get(), &raw_key) <= 0)
        throw std::runtime_error(openssl_error("Could not generate TLS key"));
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
    if (!cert || X509_set_version(cert.get(), 2) != 1
        || ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) != 1
        || !X509_gmtime_adj(X509_getm_notBefore(cert.get()), -300)
        || !X509_gmtime_adj(X509_getm_notAfter(cert.get()), 365L * 24 * 60 * 60)
        || X509_set_pubkey(cert.get(), key.get()) != 1)
        throw std::runtime_error(openssl_error("Could not create TLS certificate"));
    auto* name = X509_get_subject_name(cert.get());
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("Chatbox self-hosted server"), -1, -1, 0) != 1
        || X509_set_issuer_name(cert.get(), name) != 1
        || X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0)
        throw std::runtime_error(openssl_error("Could not sign TLS certificate"));
    // Exclusive creation plus a private directory prevents exposing key bytes
    // or overwriting a certificate created by another process.
    const auto close_file = [](FILE* file) { std::fclose(file); };
    using File = std::unique_ptr<FILE, decltype(close_file)>;
    File key_file(std::fopen(config.private_key_path.c_str(), "wx"), close_file);
    if (!key_file) throw std::runtime_error("Could not create TLS key: " + config.private_key_path);
#ifndef _WIN32
    fs::permissions(config.private_key_path, fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace);
#endif
    if (PEM_write_PrivateKey(key_file.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1
        || std::fflush(key_file.get()) != 0)
        throw std::runtime_error("Could not save TLS private key");
    key_file.reset();
    File cert_file(std::fopen(config.certificate_path.c_str(), "wx"), close_file);
    if (!cert_file || PEM_write_X509(cert_file.get(), cert.get()) != 1
        || std::fflush(cert_file.get()) != 0)
        throw std::runtime_error("Could not save TLS certificate");
    cert_file.reset();
    boost::asio::ssl::context context(boost::asio::ssl::context::tls_server);
    configure_server_context(context, config);
    return config;
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

    const std::string key = address_key(host, port);
    std::string previous;
    {
        std::lock_guard lock(trust_store_mutex);
        auto entries = read_trust_store(config.trust_store_path);
        // Merge legacy trust without replacing newer per-user decisions.
        if (config.trust_store_path == default_trust_store_path()
            && config.trust_store_path != "trusted_fingerprints.txt") {
            const auto legacy = read_trust_store("trusted_fingerprints.txt");
            bool changed = false;
            for (const auto& entry : legacy) changed |= entries.insert(entry).second;
            if (changed && !write_trust_store(config.trust_store_path, entries, error)) return false;
        }
        if (const auto it = entries.find(key); it != entries.end()) previous = it->second;
    }
    if (!config.expected_fingerprint.empty()) {
        const auto expected = normalize_fingerprint(config.expected_fingerprint);
        if (expected.empty() || expected != fingerprint) {
            error = "TLS certificate fingerprint mismatch for " + key + "; expected "
                + config.expected_fingerprint + " but received " + fingerprint;
            return false;
        }
    }
    const bool changed = !previous.empty() && previous != fingerprint;
    if (!changed && (previous == fingerprint || !config.expected_fingerprint.empty()
        || (verify_result == X509_V_OK && host_matches))) {
        notice = "Verified TLS certificate SHA-256 " + fingerprint;
        return true;
    }
    if (config.confirm_certificate) {
        if (!config.confirm_certificate(key, fingerprint, previous)) {
            error = "TLS certificate rejected for " + key + "; received " + fingerprint;
            return false;
        }
    } else if (changed) {
        error = "BLOCKED: TLS certificate for " + key + " changed. Previously trusted "
            + previous + ", received " + fingerprint + ". Review with the host before resetting trust.";
        return false;
    } else if (!config.trust_on_first_use || config.trust_store_path.empty()) {
        error = "TLS certificate validation failed for " + key + ": "
            + X509_verify_cert_error_string(verify_result)
            + ". Verify the hostname, configure a trusted CA or compare the SHA-256 fingerprint: " + fingerprint;
        return false;
    }
    std::lock_guard lock(trust_store_mutex);
    auto entries = read_trust_store(config.trust_store_path);
    // Recheck after the decision so concurrent writers cannot silently replace trust.
    if (entries.contains(key) && entries[key] != previous && entries[key] != fingerprint) {
        error = "Trust changed during review. Retry the connection."; return false;
    }
    entries[key] = fingerprint;
    if (!write_trust_store(config.trust_store_path, entries, error))
        return false;
    notice = "Saved trusted TLS certificate: "
        + fingerprint;
    return true;
}
}
