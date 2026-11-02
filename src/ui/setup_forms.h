#pragma once

#include "ui/curses_ui.h"
#include "common/utils.h"
#include "network/tls.h"
#include "ui/display_text.h"

namespace chatbox::ui
{
inline CursesField port_field()
{
    return { ui_text::port, "7777", 5, false, false, [](const std::string& value) {
        uint16_t port = 0;
        return parse_port(value, port) ? std::string{} : ui_text::invalid_port;
    } };
}
inline CursesField required_field(const std::string& label, const std::string& value, std::size_t limit)
{
    return { label, value, limit, false, false, [](const std::string& text) {
        return text.find_first_not_of(' ') == std::string::npos ? ui_text::required_field : std::string{};
    } };
}
inline void configure_tls(chatbox::tls::ServerConfig& config)
{
    if (!config.enabled) return;
    std::vector<CursesField> fields = {
        required_field(ui_text::certificate_path, "", LOGFILE_MAX_LEN),
        required_field(ui_text::private_key_path, "", LOGFILE_MAX_LEN)
    };
    edit_curses_form(ui_text::tls_certificate, ui_text::tls_description, fields);
    config.certificate_path = fields[0].value;
    config.private_key_path = fields[1].value;
}
struct PublicationSettings
{
    std::string host;
    std::string name;
    std::string public_host;
};
inline PublicationSettings configure_publication()
{
    std::vector<CursesField> fields = {
        { ui_text::browser_host, "", HOST_MAX_LEN, false, false, [](const std::string& value) {
            std::string host;
            return parse_browser_address(value, host) ? std::string{} : ui_text::browser_host_error;
        } },
        { ui_text::listing_name, "", HOST_MAX_LEN },
        { ui_text::public_address, "", HOST_MAX_LEN }
    };
    edit_curses_form(ui_text::publish_server, ui_text::publish_description, fields);
    PublicationSettings settings;
    parse_browser_address(fields[0].value, settings.host);
    settings.name = fields[1].value;
    settings.public_host = fields[2].value;
    return settings;
}
}
