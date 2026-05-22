#include "server_browser.h"

std::string sanitize_browser_field(const std::string& text, size_t max_len)
{
    std::string out;
    for (unsigned char ch : text)
    {
        if (out.size() >= max_len)
            break;
        if (std::iscntrl(ch) || ch == '|')
            continue;
        out += static_cast<char>(ch);
    }
    return out;
}

std::string browser_entry_key(const std::string& host, uint16_t port, const std::string& name)
{
    return host + "|" + std::to_string(port) + "|" + name;
}

bool parse_u16_field(const std::string& text, uint16_t& out)
{
    try
    {
        size_t consumed = 0;
        int value = std::stoi(text, &consumed);
        if (consumed != text.size() || value < 0 || value > 65535)
            return false;
        out = static_cast<uint16_t>(value);
        return true;
    }
    catch (...)
    {
        return false;
    }
}
