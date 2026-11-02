#include "common/utils.h"

#include <cstdlib>
#include <filesystem>

#ifndef _WIN32
#include <sys/stat.h>
#endif

std::string bytes_to_hex(const unsigned char* data, size_t len)
{
    std::string hex(len * 2 + 1, '\0');
    sodium_bin2hex(hex.data(), hex.size(), data, len);
    hex.pop_back();
    return hex;
}

bool hex_to_bytes(const std::string& hex, unsigned char* out, size_t out_len)
{
    size_t actual_len = 0;
    return sodium_hex2bin(
        out,
        out_len,
        hex.c_str(),
        hex.size(),
        nullptr,
        &actual_len,
        nullptr) == 0 && actual_len == out_len;
}

bool is_hex_of_len(const std::string& text, size_t bytes)
{
    if (text.size() != bytes * 2)
        return false;

    for (unsigned char ch : text)
    {
        if (!std::isxdigit(ch))
            return false;
    }

    return true;
}

std::string hex_from_text(const std::string& text)
{
    return bytes_to_hex(
        reinterpret_cast<const unsigned char*>(text.data()),
        text.size());
}

std::string random_hex(size_t bytes)
{
    std::vector<unsigned char> data(bytes);
    randombytes_buf(data.data(), data.size());
    return bytes_to_hex(data.data(), data.size());
}

std::string chatbox_data_directory()
{
    namespace fs = std::filesystem;
    const char* base = nullptr;
#ifdef _WIN32
    base = std::getenv("APPDATA");
    if (base && *base)
        return (fs::path(base) / "chatbox").string();
    base = std::getenv("USERPROFILE");
    if (base && *base)
        return (fs::path(base) / "AppData" / "Roaming" / "chatbox").string();
#else
    base = std::getenv("XDG_DATA_HOME");
    if (base && *base)
        return (fs::path(base) / "chatbox").string();
    base = std::getenv("HOME");
    if (base && *base)
        return (fs::path(base) / ".local" / "share" / "chatbox").string();
#endif
    return ".";
}

std::string chatbox_database_path()
{
    namespace fs = std::filesystem;
    const fs::path directory(chatbox_data_directory());
    const fs::path database = directory / "chatbox.db";
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec)
        return "chatbox.db";

    // Move the former working-directory database on first use so existing
    // local server data continues to be used after changing the default path.
    if (!fs::exists(database, ec) && !ec && fs::exists("chatbox.db", ec) && !ec)
    {
        fs::rename("chatbox.db", database, ec);
        if (!ec)
        {
            for (const char* suffix : {"-wal", "-shm"})
            {
                const fs::path old_sidecar = std::string("chatbox.db") + suffix;
                if (fs::exists(old_sidecar, ec) && !ec)
                    fs::rename(old_sidecar, database.string() + suffix, ec);
                ec.clear();
            }
        }
    }
    return database.string();
}


std::string identity_file_for_nickname(const std::string& nick)
{
    namespace fs = std::filesystem;
    return (fs::path(chatbox_data_directory()) /
        ("chatbox_identity_" + hex_from_text(nick) + ".key")).string();
}

bool load_identity_file(const std::string& path, ClientIdentity& identity)
{
    std::ifstream file(path);
    if (!file.is_open())
        return false;

    std::string public_hex;
    std::string secret_hex;
    std::getline(file, public_hex);
    std::getline(file, secret_hex);

    if (!is_hex_of_len(public_hex, crypto_sign_PUBLICKEYBYTES) ||
        !is_hex_of_len(secret_hex, crypto_sign_SECRETKEYBYTES))
    {
        return false;
    }

    if (!hex_to_bytes(public_hex, identity.public_key.data(), identity.public_key.size()) ||
        !hex_to_bytes(secret_hex, identity.secret_key.data(), identity.secret_key.size()))
    {
        return false;
    }

    identity.public_key_hex = public_hex;
    return true;
}

bool save_identity_file(const std::string& path, const ClientIdentity& identity)
{
    namespace fs = std::filesystem;
    const fs::path file_path(path);
    std::error_code ec;
    if (!file_path.parent_path().empty())
        fs::create_directories(file_path.parent_path(), ec);
    if (ec)
        return false;
#ifndef _WIN32
    if (!file_path.parent_path().empty())
        chmod(file_path.parent_path().c_str(), S_IRWXU);
#endif
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open())
        return false;

    file << identity.public_key_hex << "\n";
    file << bytes_to_hex(identity.secret_key.data(), identity.secret_key.size()) << "\n";
    file.close();
    if (!file)
        return false;
#ifndef _WIN32
    chmod(file_path.c_str(), S_IRUSR | S_IWUSR);
#endif
    return true;
}

bool load_or_create_identity(const std::string& nick, ClientIdentity& identity, bool& created)
{
    const std::string path = identity_file_for_nickname(nick);
    created = false;

    if (load_identity_file(path, identity))
        return true;

    // Migrate the former working-directory key so existing identities survive
    // the move to the per-user data directory.
    const std::string legacy_path = "chatbox_identity_" + hex_from_text(nick) + ".key";
    if (legacy_path != path && load_identity_file(legacy_path, identity))
        return save_identity_file(path, identity);

    crypto_sign_keypair(identity.public_key.data(), identity.secret_key.data());
    identity.public_key_hex = bytes_to_hex(identity.public_key.data(), identity.public_key.size());
    created = true;
    return save_identity_file(path, identity);
}

std::string sign_identity_challenge(const ClientIdentity& identity, const std::string& challenge)
{
    std::array<unsigned char, crypto_sign_BYTES> signature{};
    unsigned long long signature_len = 0;

    crypto_sign_detached(
        signature.data(),
        &signature_len,
        reinterpret_cast<const unsigned char*>(challenge.data()),
        static_cast<unsigned long long>(challenge.size()),
        identity.secret_key.data());

    return bytes_to_hex(signature.data(), signature_len);
}

bool verify_identity_signature(
    const std::string& public_key_hex,
    const std::string& challenge,
    const std::string& signature_hex)
{
    if (!is_hex_of_len(public_key_hex, crypto_sign_PUBLICKEYBYTES) ||
        !is_hex_of_len(signature_hex, crypto_sign_BYTES))
    {
        return false;
    }

    std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> public_key{};
    std::array<unsigned char, crypto_sign_BYTES> signature{};

    if (!hex_to_bytes(public_key_hex, public_key.data(), public_key.size()) ||
        !hex_to_bytes(signature_hex, signature.data(), signature.size()))
    {
        return false;
    }

    return crypto_sign_verify_detached(
        signature.data(),
        reinterpret_cast<const unsigned char*>(challenge.data()),
        static_cast<unsigned long long>(challenge.size()),
        public_key.data()) == 0;
}

std::string identity_fingerprint(const std::string& public_key_hex)
{
    if (public_key_hex.size() <= 16)
        return public_key_hex;
    return public_key_hex.substr(0, 16);
}

bool is_valid_nickname(const std::string& nick)
{
    if (nick.empty() || nick.size() > NICK_MAX_LEN)
        return false;

    for (unsigned char ch : nick)
    {
        if (std::iscntrl(ch) || std::isspace(ch) || ch == '|' || ch == ',')
            return false;
    }

    return true;
}

bool is_valid_chat_message(const std::string& message)
{
    if (message.empty() || message.size() > MAX_CHAT_MESSAGE_LEN)
        return false;

    for (unsigned char ch : message)
    {
        if (std::iscntrl(ch) && ch != '\t')
            return false;
    }

    return true;
}

std::string timestamp()
{
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif

    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

std::vector<std::string> split(const std::string& s, char delim)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim))
        out.push_back(item);
    return out;
}

std::string join_fields(const std::vector<std::string>& fields, size_t start, char delim)
{
    std::string out;
    for (size_t i = start; i < fields.size(); ++i)
    {
        if (i > start)
            out += delim;
        out += fields[i];
    }
    return out;
}
