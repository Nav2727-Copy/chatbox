#include "storage/preferences.h"
#include "common/utils.h"
#include <sqlite3.h>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace chatbox::storage
{
namespace
{
using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Database open(const std::string& path)
{
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    sqlite3* raw = nullptr;
    const auto status = sqlite3_open(path.c_str(), &raw);
    Database db(raw, sqlite3_close);
    if (status != SQLITE_OK) throw std::runtime_error("Could not open preferences: " + path);
    sqlite3_busy_timeout(db.get(), 1000);
    return db;
}
void execute(sqlite3* db, const char* sql)
{
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}
Statement prepare(sqlite3* db, const char* sql)
{
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
    return Statement(raw, sqlite3_finalize);
}
std::string text(sqlite3_stmt* row, int column)
{
    const auto* value = sqlite3_column_text(row, column);
    return value ? reinterpret_cast<const char*>(value) : "";
}
void bind_text(sqlite3_stmt* row, int column, const std::string& value)
{
    if (sqlite3_bind_text(row, column, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error("Could not encode preference");
}
}
std::string preferences_path()
{
    return (std::filesystem::path(chatbox_data_directory()) / "preferences.db").string();
}
void Preferences::remember_server(RecentServer server)
{
    for (auto it = recent_servers.begin(); it != recent_servers.end(); ++it)
        if (it->host == server.host && it->port == server.port && it->tls == server.tls) {
            server.name = it->name;
            recent_servers.erase(it);
            break;
        }
    recent_servers.insert(recent_servers.begin(), std::move(server));
    if (recent_servers.size() > 10) recent_servers.resize(10);
}
Preferences load_preferences(const std::string& path)
{
    Preferences result;
    if (!std::filesystem::exists(path)) return result;
    auto db = open(path);
    auto settings = prepare(db.get(), "SELECT remember, nickname FROM settings LIMIT 1");
    if (sqlite3_step(settings.get()) == SQLITE_ROW) {
        result.remember = sqlite3_column_int(settings.get(), 0) == 1;
        result.nickname = text(settings.get(), 1);
        if (!is_valid_nickname(result.nickname)) result.nickname.clear();
    }
    if (!result.remember) { result.nickname.clear(); return result; }
    auto servers = prepare(db.get(), "SELECT name,host,port,tls FROM recent ORDER BY position LIMIT 10");
    while (sqlite3_step(servers.get()) == SQLITE_ROW) {
        const int port = sqlite3_column_int(servers.get(), 2);
        auto host = text(servers.get(), 1);
        if (port < 1 || port > 65535 || host.empty() || host.size() > HOST_MAX_LEN) continue;
        result.recent_servers.push_back({text(servers.get(), 0), host,
            static_cast<std::uint16_t>(port), sqlite3_column_int(servers.get(), 3) == 1});
    }
    auto tables = prepare(db.get(), "SELECT name FROM sqlite_master WHERE type='table' AND name='options'");
    if (sqlite3_step(tables.get()) == SQLITE_ROW) {
        auto options = prepare(db.get(), "SELECT name,value FROM options");
        while (sqlite3_step(options.get()) == SQLITE_ROW)
            result.options[text(options.get(), 0)] = text(options.get(), 1);
        for (std::size_t i = 0; i < result.recent_servers.size(); ++i) {
            result.recent_servers[i].ca_path = result.options["recent.ca." + std::to_string(i)];
            result.recent_servers[i].fingerprint = result.options["recent.pin." + std::to_string(i)];
        }
    }
    return result;
}
void save_preferences(const std::string& path, const Preferences& preferences)
{
    auto db = open(path);
    execute(db.get(), "BEGIN IMMEDIATE; CREATE TABLE IF NOT EXISTS settings(remember INTEGER, nickname TEXT);"
        "CREATE TABLE IF NOT EXISTS recent(position INTEGER, name TEXT, host TEXT, port INTEGER, tls INTEGER);"
        "CREATE TABLE IF NOT EXISTS options(name TEXT PRIMARY KEY,value TEXT);"
        "DELETE FROM settings; DELETE FROM recent; DELETE FROM options;");
    auto settings = prepare(db.get(), "INSERT INTO settings VALUES(?,?)");
    sqlite3_bind_int(settings.get(), 1, preferences.remember);
    bind_text(settings.get(), 2, preferences.remember ? preferences.nickname : "");
    if (sqlite3_step(settings.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db.get()));
    if (preferences.remember) {
        auto server = prepare(db.get(), "INSERT INTO recent VALUES(?,?,?,?,?)");
        int index = 0;
        for (const auto& entry : preferences.recent_servers) {
            if (index == 10) break;
            sqlite3_reset(server.get());
            sqlite3_bind_int(server.get(), 1, index++);
            bind_text(server.get(), 2, entry.name);
            bind_text(server.get(), 3, entry.host);
            sqlite3_bind_int(server.get(), 4, entry.port);
            sqlite3_bind_int(server.get(), 5, entry.tls);
            if (sqlite3_step(server.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db.get()));
        }
    }
    if (preferences.remember) {
        auto options = preferences.options;
        for (auto it = options.begin(); it != options.end();)
            if (it->first.starts_with("recent.")) it = options.erase(it); else ++it;
        for (std::size_t i = 0; i < preferences.recent_servers.size(); ++i) {
            options["recent.ca." + std::to_string(i)] = preferences.recent_servers[i].ca_path;
            options["recent.pin." + std::to_string(i)] = preferences.recent_servers[i].fingerprint;
        }
        auto row = prepare(db.get(), "INSERT INTO options VALUES(?,?)");
        for (const auto& [name, value] : options) {
            sqlite3_reset(row.get()); bind_text(row.get(), 1, name); bind_text(row.get(), 2, value);
            if (sqlite3_step(row.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db.get()));
        }
    }
    execute(db.get(), "COMMIT");
}
}
