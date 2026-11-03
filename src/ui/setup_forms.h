#pragma once

#include "ui/curses_ui.h"
#include "common/utils.h"
#include "network/tls.h"
#include "ui/display_text.h"
#include <filesystem>
#include <curses.h>

namespace chatbox::ui
{
inline chatbox::tls::ClientConfig interactive_tls()
{
    chatbox::tls::ClientConfig config;
    config.trust_on_first_use = false;
    config.confirm_certificate = [](const std::string& address, const std::string& fingerprint, const std::string& previous) {
        show_curses_notice(previous.empty() ? "Verify host certificate" : "Certificate changed — blocked",
            "Server: " + address + "\nSHA-256: " + fingerprint
            + (previous.empty() ? "" : "\nPreviously trusted: " + previous)
            + "\nCompare the full fingerprint with the host through a trusted channel. TLS protects transport, not end-to-end chat.");
        const int choice = choose_curses_menu("Certificate decision", "Accept only after verifying with the host.", {
            {'r', "Reject", "Return without trusting this certificate."},
            {'a', previous.empty() ? "Accept certificate" : "Review replacement", "Save this fingerprint for this address."}
        });
        if (choice != 1) return false;
        if (!previous.empty()) {
            std::vector<CursesField> fields{{"Type REPLACE to reset trust", "", 7}};
            try { edit_curses_form("Reset certificate trust", "Only after verifying the new fingerprint with the host.", fields); }
            catch (const CursesFormCancelled&) { return false; }
            return fields[0].value == "REPLACE";
        }
        return true;
    };
    config.continue_waiting = [] {
        show_curses_notice("Contacting directory", "Resolving, connecting, or reading the directory. Esc cancels.", false);
        timeout(25); const int key = getch(); timeout(-1); return key != 27;
    };
    return config;
}

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
    const int choice = choose_curses_menu("Secure hosting",
        "TLS protects transport, not end-to-end chat. A password separately controls access.", {
            {'g', "Generate or reuse a certificate", "Store a private key and certificate in your Chatbox data folder."},
            {'e', "Use existing certificate files", "Choose a PEM certificate and matching private key."}
        });
    if (choice < 0) throw CursesFormCancelled{};
    if (choice == 0) {
        config = chatbox::tls::ensure_host_certificate(
            (std::filesystem::path(chatbox_data_directory()) / "tls").string());
        show_curses_notice("Hosting certificate", "Certificate: " + config.certificate_path
            + "\nPrivate key (keep secret): " + config.private_key_path);
        show_curses_notice("Share this fingerprint", "Guests can compare this SHA-256 fingerprint through a trusted channel: "
            + chatbox::tls::certificate_sha256_fingerprint(config.certificate_path));
        return;
    }
    std::vector<CursesField> fields = {
        required_field(ui_text::certificate_path, config.certificate_path, LOGFILE_MAX_LEN),
        required_field(ui_text::private_key_path, config.private_key_path, LOGFILE_MAX_LEN)
    };
    edit_curses_form(ui_text::tls_certificate, ui_text::tls_description, fields);
    config.certificate_path = fields[0].value;
    config.private_key_path = fields[1].value;
    boost::asio::ssl::context context(boost::asio::ssl::context::tls_server);
    chatbox::tls::configure_server_context(context, config);
}
struct PublicationSettings
{
    std::string host;
    std::string name;
    std::string public_host;
    bool tls = true;
    std::string ca_path, fingerprint;
};
inline PublicationSettings configure_publication(const PublicationSettings& previous = {})
{
    std::vector<CursesField> fields = {
        { ui_text::browser_host, previous.host, HOST_MAX_LEN, false, false, [](const std::string& value) {
            std::string host;
            return parse_browser_address(value, host) ? std::string{} : ui_text::browser_host_error;
        } },
        { ui_text::listing_name, previous.name, HOST_MAX_LEN },
        { ui_text::public_address, previous.public_host, HOST_MAX_LEN },
        { "Directory TLS (no = insecure)", previous.tls ? "yes" : "no", 3, false, true },
        { "Directory CA file (optional)", previous.ca_path, LOGFILE_MAX_LEN },
        { "Directory SHA-256 (optional)", previous.fingerprint, 95 }
    };
    edit_curses_form(ui_text::publish_server, ui_text::publish_description, fields);
    PublicationSettings settings;
    parse_browser_address(fields[0].value, settings.host);
    settings.name = fields[1].value;
    settings.public_host = fields[2].value;
    settings.tls = fields[3].value == "yes"; settings.ca_path = fields[4].value; settings.fingerprint = fields[5].value;
    return settings;
}
}
