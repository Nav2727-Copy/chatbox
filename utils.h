/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: CC BY-NC-SA 4.0 (https://creativecommons.org/licenses/by-nc-sa/4.0/)
*/
#pragma once

#include "common.h"

struct ClientIdentity
{
    std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> public_key{};
    std::array<unsigned char, crypto_sign_SECRETKEYBYTES> secret_key{};
    std::string public_key_hex;
};

std::string encode_base64(const std::string& input);
bool try_decode_base64(const std::string& input, std::string& output);
std::string decode_base64(const std::string& input);
std::string bytes_to_hex(const unsigned char* data, size_t len);
bool hex_to_bytes(const std::string& hex, unsigned char* out, size_t out_len);
bool is_hex_of_len(const std::string& text, size_t bytes);
std::string hex_from_text(const std::string& text);
std::string random_hex(size_t bytes);
std::string identity_file_for_nickname(const std::string& nick);
bool load_identity_file(const std::string& path, ClientIdentity& identity);
bool save_identity_file(const std::string& path, const ClientIdentity& identity);
bool load_or_create_identity(const std::string& nick, ClientIdentity& identity, bool& created);
std::string sign_identity_challenge(const ClientIdentity& identity, const std::string& challenge);
bool verify_identity_signature(
    const std::string& public_key_hex,
    const std::string& challenge,
    const std::string& signature_hex);
std::string identity_fingerprint(const std::string& public_key_hex);
void strip_wire_newline(std::string& line);
bool is_valid_nickname(const std::string& nick);
bool is_valid_chat_message(const std::string& message);
std::string timestamp();
std::vector<std::string> split(const std::string& s, char delim);
std::string join_fields(const std::vector<std::string>& fields, size_t start, char delim);
