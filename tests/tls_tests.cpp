#include "chat_client.h"
#include "chat_server.h"
#include "server_browser.h"
#include "tls.h"

#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>
#include <sodium.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

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

struct CertificateFiles
{
    std::filesystem::path certificate;
    std::filesystem::path private_key;
    std::string fingerprint;
};

std::filesystem::path temporary_path(const std::string& suffix)
{
    static std::atomic<unsigned long long> counter{ 0 };
    const auto value = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("chatbox_tls_" + std::to_string(value) + "_"
            + std::to_string(counter++) + "_" + suffix);
}

std::string certificate_fingerprint(X509* certificate)
{
    unsigned char digest[EVP_MAX_MD_SIZE] = {};
    unsigned int size = 0;
    if (X509_digest(certificate, EVP_sha256(), digest, &size) != 1)
        return {};
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < size; ++index)
        out << std::setw(2) << static_cast<unsigned int>(digest[index]);
    return out.str();
}

CertificateFiles create_certificate(long serial)
{
    CertificateFiles files{ temporary_path("cert.pem"),
        temporary_path("key.pem"), {} };

    EVP_PKEY* key = nullptr;
    EVP_PKEY_CTX* key_context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    CHECK(key_context != nullptr);
    CHECK(EVP_PKEY_keygen_init(key_context) == 1);
    CHECK(EVP_PKEY_CTX_set_rsa_keygen_bits(key_context, 2048) == 1);
    CHECK(EVP_PKEY_keygen(key_context, &key) == 1);
    EVP_PKEY_CTX_free(key_context);

    X509* certificate = X509_new();
    CHECK(certificate != nullptr);
    CHECK(X509_set_version(certificate, 2) == 1);
    CHECK(ASN1_INTEGER_set(X509_get_serialNumber(certificate), serial) == 1);
    X509_gmtime_adj(X509_get_notBefore(certificate), -60);
    X509_gmtime_adj(X509_get_notAfter(certificate), 10L * 365 * 24 * 60 * 60);
    CHECK(X509_set_pubkey(certificate, key) == 1);

    X509_NAME* name = X509_get_subject_name(certificate);
    CHECK(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) == 1);
    CHECK(X509_set_issuer_name(certificate, name) == 1);

    X509V3_CTX extension_context;
    X509V3_set_ctx(&extension_context, certificate, certificate, nullptr, nullptr, 0);
    X509_EXTENSION* basic_constraints = X509V3_EXT_conf_nid(nullptr,
        &extension_context, NID_basic_constraints,
        const_cast<char*>("critical,CA:TRUE"));
    X509_EXTENSION* subject_alt_name = X509V3_EXT_conf_nid(nullptr,
        &extension_context, NID_subject_alt_name,
        const_cast<char*>("DNS:localhost,IP:127.0.0.1,IP:::1"));
    CHECK(basic_constraints && subject_alt_name);
    CHECK(X509_add_ext(certificate, basic_constraints, -1) == 1);
    CHECK(X509_add_ext(certificate, subject_alt_name, -1) == 1);
    X509_EXTENSION_free(basic_constraints);
    X509_EXTENSION_free(subject_alt_name);
    CHECK(X509_sign(certificate, key, EVP_sha256()) > 0);

    FILE* certificate_file = std::fopen(files.certificate.string().c_str(), "wb");
    FILE* key_file = std::fopen(files.private_key.string().c_str(), "wb");
    CHECK(certificate_file && key_file);
    if (certificate_file)
    {
        CHECK(PEM_write_X509(certificate_file, certificate) == 1);
        std::fclose(certificate_file);
    }
    if (key_file)
    {
        CHECK(PEM_write_PrivateKey(key_file, key, nullptr, nullptr, 0,
            nullptr, nullptr) == 1);
        std::fclose(key_file);
    }

    files.fingerprint = certificate_fingerprint(certificate);
    X509_free(certificate);
    EVP_PKEY_free(key);
    return files;
}

void remove_file(const std::filesystem::path& path)
{
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

chatbox::tls::ServerConfig server_config(const CertificateFiles& files)
{
    return { true, files.certificate.string(), files.private_key.string() };
}

void trusted_ca_and_pin_are_accepted(const CertificateFiles& files)
{
    boost::asio::io_context server_io;
    ChatServer server(server_io, 0, "", nullptr, "", nullptr,
        server_config(files));
    std::thread server_thread([&] { server_io.run(); });

    {
        boost::asio::io_context client_io;
        chatbox::tls::ClientConfig config;
        config.ca_path = files.certificate.string();
        config.trust_on_first_use = false;
        ChatClient client(client_io, config);
        CHECK(client.connect("localhost", server.port()));
    }
    {
        boost::asio::io_context client_io;
        chatbox::tls::ClientConfig config;
        config.expected_fingerprint = files.fingerprint;
        config.trust_on_first_use = false;
        ChatClient client(client_io, config);
        CHECK(client.connect("localhost", server.port()));
    }
    {
        boost::asio::io_context client_io;
        chatbox::tls::ClientConfig config;
        config.expected_fingerprint = std::string(64, '0');
        config.trust_on_first_use = false;
        ChatClient client(client_io, config);
        CHECK(!client.connect("localhost", server.port()));
        CHECK(client.connect_error().find("fingerprint mismatch") != std::string::npos);
    }

    server.stop();
    std::this_thread::sleep_for(20ms);
    server_io.stop();
    server_thread.join();
}

void certificate_changes_are_blocked(const CertificateFiles& first,
    const CertificateFiles& second)
{
    const auto trust_store = temporary_path("trust.txt");
    std::uint16_t port = 0;
    {
        boost::asio::io_context server_io;
        ChatServer server(server_io, 0, "", nullptr, "", nullptr,
            server_config(first));
        port = server.port();
        std::thread server_thread([&] { server_io.run(); });

        boost::asio::io_context client_io;
        chatbox::tls::ClientConfig config;
        config.trust_store_path = trust_store.string();
        ChatClient client(client_io, config);
        CHECK(client.connect("localhost", port));
        CHECK(std::filesystem::exists(trust_store));

        server.stop();
        std::this_thread::sleep_for(20ms);
        server_io.stop();
        server_thread.join();
    }

    {
        boost::asio::io_context server_io;
        ChatServer server(server_io, port, "", nullptr, "", nullptr,
            server_config(second));
        std::thread server_thread([&] { server_io.run(); });

        boost::asio::io_context client_io;
        chatbox::tls::ClientConfig config;
        config.trust_store_path = trust_store.string();
        ChatClient client(client_io, config);
        CHECK(!client.connect("localhost", port));
        CHECK(client.connect_error().find("BLOCKED") != std::string::npos);
        CHECK(client.connect_error().find("changed") != std::string::npos);

        server.stop();
        std::this_thread::sleep_for(20ms);
        server_io.stop();
        server_thread.join();
    }
    remove_file(trust_store);
}

void plaintext_requires_an_explicit_client_choice()
{
    boost::asio::io_context server_io;
    ChatServer server(server_io, 0);
    std::thread server_thread([&] { server_io.run(); });

    {
        boost::asio::io_context client_io;
        chatbox::tls::ClientConfig secure;
        secure.trust_store_path = temporary_path("unused.txt").string();
        ChatClient client(client_io, secure);
        CHECK(!client.connect("localhost", server.port()));
    }
    {
        boost::asio::io_context client_io;
        chatbox::tls::ClientConfig plaintext;
        plaintext.enabled = false;
        ChatClient client(client_io, plaintext);
        CHECK(client.connect("localhost", server.port()));
    }

    server.stop();
    std::this_thread::sleep_for(20ms);
    server_io.stop();
    server_thread.join();
}

void browser_traffic_uses_tls(const CertificateFiles& files)
{
    boost::asio::io_context browser_io;
    ServerBrowser browser(browser_io, 0, server_config(files));
    std::thread browser_thread([&] { browser_io.run(); });

    chatbox::tls::ClientConfig client_config;
    client_config.ca_path = files.certificate.string();
    client_config.trust_on_first_use = false;

    BrowserEntry published;
    published.name = "TLS room";
    published.host = "localhost";
    published.port = 4242;
    published.uses_tls = true;
    std::string error;
    CHECK(ServerBrowserClient::register_server("localhost", browser.port(),
        published, error, client_config));
    std::vector<BrowserEntry> entries;
    CHECK(ServerBrowserClient::list_servers("localhost", browser.port(), entries,
        error, client_config));
    CHECK(entries.size() == 1);
    CHECK(entries.size() == 1 && entries[0].uses_tls);

    chatbox::tls::ClientConfig wrong_pin = client_config;
    wrong_pin.expected_fingerprint = std::string(64, '0');
    entries.clear();
    CHECK(!ServerBrowserClient::list_servers("localhost", browser.port(), entries,
        error, wrong_pin));
    CHECK(error.find("fingerprint mismatch") != std::string::npos);

    browser.stop();
    std::this_thread::sleep_for(20ms);
    browser_io.stop();
    browser_thread.join();
}
}

int main()
{
    if (sodium_init() < 0)
        return EXIT_FAILURE;

    const auto first = create_certificate(1);
    const auto second = create_certificate(2);
    trusted_ca_and_pin_are_accepted(first);
    certificate_changes_are_blocked(first, second);
    plaintext_requires_an_explicit_client_choice();
    browser_traffic_uses_tls(first);

    remove_file(first.certificate);
    remove_file(first.private_key);
    remove_file(second.certificate);
    remove_file(second.private_key);

    if (failures != 0)
    {
        std::cerr << failures << " TLS test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "TLS tests passed\n";
    return EXIT_SUCCESS;
}
