#include "utils.h"

namespace
{
constexpr const char* BASE64_CHARS =
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}
std::string encode_base64(const std::string& input)
{
    std::string output;
    int i = 0;

    while (i < static_cast<int>(input.size()))
    {
        int bytes = 1;
        uint32_t b = (static_cast<unsigned char>(input[i++]) & 0xFF) << 16;
        if (i < static_cast<int>(input.size()))
        {
            b |= (static_cast<unsigned char>(input[i++]) & 0xFF) << 8;
            ++bytes;
        }
        if (i < static_cast<int>(input.size()))
        {
            b |= (static_cast<unsigned char>(input[i++]) & 0xFF);
            ++bytes;
        }

        output += BASE64_CHARS[(b >> 18) & 0x3F];
        output += BASE64_CHARS[(b >> 12) & 0x3F];
        output += (bytes >= 2) ? BASE64_CHARS[(b >> 6) & 0x3F] : '=';
        output += (bytes == 3) ? BASE64_CHARS[b & 0x3F] : '=';
    }

    return output;
}

bool try_decode_base64(const std::string& input, std::string& output)
{
    output.clear();

    if (input.empty() || input.size() % 4 != 0)
        return false;

    auto char_to_val = [](char c) -> int
        {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
        };

    for (size_t i = 0; i < input.size(); i += 4)
    {
        const bool final_group = i + 4 == input.size();
        int v0 = char_to_val(input[i]);
        int v1 = char_to_val(input[i + 1]);
        int v2 = char_to_val(input[i + 2]);
        int v3 = char_to_val(input[i + 3]);

        if (v0 < 0 || v1 < 0)
            return false;

        output += static_cast<char>((v0 << 2) | (v1 >> 4));

        if (input[i + 2] != '=')
        {
            if (v2 < 0)
                return false;

            output += static_cast<char>((v1 << 4) | (v2 >> 2));
            if (input[i + 3] != '=')
            {
                if (v3 < 0)
                    return false;
                output += static_cast<char>((v2 << 6) | v3);
            }
            else if (!final_group)
            {
                return false;
            }
        }
        else
        {
            if (input[i + 3] != '=' || !final_group)
                return false;
        }
    }

    return true;
}

std::string decode_base64(const std::string& input)
{
    std::string output;
    if (!try_decode_base64(input, output))
        return "";
    return output;
}

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


std::string identity_file_for_nickname(const std::string& nick)
{
    return "chatbox_identity_" + hex_from_text(nick) + ".key";
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
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open())
        return false;

    file << identity.public_key_hex << "\n";
    file << bytes_to_hex(identity.secret_key.data(), identity.secret_key.size()) << "\n";
    return true;
}

bool load_or_create_identity(const std::string& nick, ClientIdentity& identity, bool& created)
{
    const std::string path = identity_file_for_nickname(nick);
    created = false;

    if (load_identity_file(path, identity))
        return true;

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

void strip_wire_newline(std::string& line)
{
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
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
