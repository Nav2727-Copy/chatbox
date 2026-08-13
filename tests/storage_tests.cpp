#include "storage.h"
#include "utils.h"

#include <sqlite3.h>
#include <sodium.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace storage = chatbox::storage;

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

std::filesystem::path temporary_path(const std::string& suffix)
{
    const auto value = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("chatbox_" + std::to_string(value) + "_" + suffix);
}

void remove_database(const std::filesystem::path& path)
{
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + "-wal", ignored);
    std::filesystem::remove(path.string() + "-shm", ignored);
}

void persistence_and_pagination()
{
    const auto path = temporary_path("storage.db");
    {
        storage::SQLiteStorage database(path.string());
        CHECK(database.schema_version() == storage::SQLiteStorage::SCHEMA_VERSION);
        const auto initial_rooms = database.load_rooms();
        CHECK(initial_rooms.size() == 1);
        CHECK(initial_rooms[0].name == "lobby");

        CHECK(database.store_room({ 2, "engineering", "build things", "alice", 10 }));
        CHECK(database.store_identity("alice", std::string(64, 'a')));
        CHECK(database.store_ban({ "mallory", "spam", 11, std::nullopt }));
        for (int index = 1; index <= 5; ++index)
        {
            storage::StoredMessage message;
            message.id = "message-" + std::to_string(index);
            message.room_id = 2;
            message.nickname = "alice";
            message.body = "body-" + std::to_string(index);
            message.sent_at = 100 + index;
            CHECK(database.store_message(message));
        }
        database.flush();
        CHECK(database.last_error().empty());

        auto first = database.load_history(2, 2);
        CHECK(first.messages.size() == 2);
        CHECK(first.messages[0].body == "body-5");
        CHECK(first.messages[1].body == "body-4");
        CHECK(first.next.has_value());
        auto second = database.load_history(2, 2, first.next);
        CHECK(second.messages.size() == 2);
        CHECK(second.messages[0].body == "body-3");
        CHECK(second.messages[1].body == "body-2");
        CHECK(second.next.has_value());
        auto third = database.load_history(2, 2, second.next);
        CHECK(third.messages.size() == 1);
        CHECK(third.messages[0].body == "body-1");
        CHECK(!third.next.has_value());
    }

    {
        storage::SQLiteStorage database(path.string());
        const auto rooms = database.load_rooms();
        CHECK(rooms.size() == 2);
        CHECK(rooms[1].name == "engineering");
        CHECK(rooms[1].owner_nickname == "alice");
        CHECK(database.load_identities().size() == 1);
        CHECK(database.load_bans(0).size() == 1);
        CHECK(database.load_history(2, 100).messages.size() == 5);
        CHECK(database.remove_ban("mallory"));
        database.flush();
        CHECK(database.load_bans(0).empty());
    }
    remove_database(path);
}

void legacy_files_are_imported_once()
{
    const auto database_path = temporary_path("legacy.db");
    const auto bans_path = temporary_path("bans.txt");
    const auto identities_path = temporary_path("identities.txt");
    {
        std::ofstream bans(bans_path);
        bans << "mallory\ninvalid nickname\n";
        std::ofstream identities(identities_path);
        identities << "alice|" << std::string(64, 'b') << "\n";
    }

    {
        storage::SQLiteStorage database(database_path.string());
        const auto first = database.import_legacy_files(
            bans_path.string(), identities_path.string());
        CHECK(first.bans == 1);
        CHECK(first.identities == 1);
        const auto second = database.import_legacy_files(
            bans_path.string(), identities_path.string());
        CHECK(second.bans_already_imported);
        CHECK(second.identities_already_imported);
        CHECK(database.load_bans(0).size() == 1);
        CHECK(database.load_identities().size() == 1);
    }

    remove_database(database_path);
    std::error_code ignored;
    std::filesystem::remove(bans_path, ignored);
    std::filesystem::remove(identities_path, ignored);
}

void execute(sqlite3* db, const char* sql)
{
    char* detail = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &detail);
    if (result != SQLITE_OK)
    {
        std::cerr << "test setup SQL failed: " << (detail ? detail : "unknown") << '\n';
        sqlite3_free(detail);
        ++failures;
    }
}

void schema_upgrade_is_transactional()
{
    const auto path = temporary_path("migration.db");
    sqlite3* db = nullptr;
    CHECK(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    execute(db,
        "CREATE TABLE schema_version(version INTEGER NOT NULL);"
        "INSERT INTO schema_version VALUES(1);"
        "CREATE TABLE rooms(id INTEGER PRIMARY KEY,name TEXT UNIQUE NOT NULL,"
        "topic TEXT NOT NULL DEFAULT '',created_at INTEGER NOT NULL);"
        "CREATE TABLE identities(nickname TEXT PRIMARY KEY,public_key TEXT NOT NULL,"
        "created_at INTEGER NOT NULL);"
        "CREATE TABLE bans(nickname TEXT PRIMARY KEY,reason TEXT NOT NULL DEFAULT '',"
        "created_at INTEGER NOT NULL,expires_at INTEGER);"
        "CREATE TABLE messages(id TEXT PRIMARY KEY,room_id INTEGER NOT NULL REFERENCES rooms(id),"
        "nickname TEXT NOT NULL,body TEXT NOT NULL,sent_at INTEGER NOT NULL,"
        "signature TEXT,public_key TEXT);"
        "INSERT INTO rooms VALUES(1,'lobby','old topic',1);"
    );
    sqlite3_close(db);

    {
        storage::SQLiteStorage database(path.string());
        CHECK(database.schema_version() == storage::SQLiteStorage::SCHEMA_VERSION);
        const auto rooms = database.load_rooms();
        CHECK(rooms.size() == 1);
        CHECK(rooms[0].topic == "old topic");
        CHECK(rooms[0].owner_nickname.empty());
    }
    remove_database(path);
}
}

int main()
{
    if (sodium_init() < 0)
        return EXIT_FAILURE;

    persistence_and_pagination();
    legacy_files_are_imported_once();
    schema_upgrade_is_transactional();
    if (failures != 0)
    {
        std::cerr << failures << " storage test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "storage tests passed\n";
    return EXIT_SUCCESS;
}
