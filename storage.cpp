#include "storage.h"

#include "utils.h"

#include <sqlite3.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <variant>

namespace chatbox::storage
{
namespace
{
std::int64_t unix_seconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void throw_sqlite(sqlite3* db, const std::string& action, int code)
{
    throw std::runtime_error(action + ": "
        + (db ? sqlite3_errmsg(db) : sqlite3_errstr(code)));
}

void exec_sql(sqlite3* db, const char* sql)
{
    char* detail = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &detail);
    if (result == SQLITE_OK)
        return;

    const std::string message = detail ? detail : sqlite3_errmsg(db);
    sqlite3_free(detail);
    throw std::runtime_error("SQLite statement failed: " + message);
}

class Statement
{
public:
    Statement(sqlite3* db, const char* sql) : db_(db)
    {
        const int result = sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr);
        if (result != SQLITE_OK)
            throw_sqlite(db, "Could not prepare SQLite statement", result);
    }

    ~Statement()
    {
        sqlite3_finalize(statement_);
    }

    sqlite3_stmt* get() const { return statement_; }

    void bind(int index, std::int64_t value)
    {
        check(sqlite3_bind_int64(statement_, index, value));
    }

    void bind(int index, const std::string& value)
    {
        check(sqlite3_bind_text(statement_, index, value.c_str(),
            static_cast<int>(value.size()), SQLITE_TRANSIENT));
    }

    void bind_null(int index)
    {
        check(sqlite3_bind_null(statement_, index));
    }

    bool step_row()
    {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW)
            return true;
        if (result == SQLITE_DONE)
            return false;
        throw_sqlite(db_, "Could not execute SQLite query", result);
        return false;
    }

    void execute()
    {
        const int result = sqlite3_step(statement_);
        if (result != SQLITE_DONE)
            throw_sqlite(db_, "Could not execute SQLite write", result);
    }

private:
    void check(int result)
    {
        if (result != SQLITE_OK)
            throw_sqlite(db_, "Could not bind SQLite value", result);
    }

    sqlite3* db_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

std::string column_text(sqlite3_stmt* statement, int column)
{
    const auto* text = sqlite3_column_text(statement, column);
    return text ? reinterpret_cast<const char*>(text) : std::string{};
}

sqlite3* open_database(const std::string& path)
{
    sqlite3* db = nullptr;
    const int result = sqlite3_open_v2(path.c_str(), &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (result != SQLITE_OK)
    {
        const std::string detail = db ? sqlite3_errmsg(db) : sqlite3_errstr(result);
        if (db)
            sqlite3_close(db);
        throw std::runtime_error("Could not open SQLite database '" + path
            + "': " + detail);
    }
    sqlite3_busy_timeout(db, 5000);
    exec_sql(db, "PRAGMA foreign_keys = ON;");
    exec_sql(db, "PRAGMA journal_mode = WAL;");
    exec_sql(db, "PRAGMA synchronous = NORMAL;");
    return db;
}

bool table_exists(sqlite3* db, const std::string& name)
{
    Statement statement(db,
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?;");
    statement.bind(1, name);
    return statement.step_row();
}

int read_schema_version(sqlite3* db)
{
    if (!table_exists(db, "schema_version"))
        return 0;
    Statement statement(db, "SELECT version FROM schema_version LIMIT 1;");
    if (!statement.step_row())
        throw std::runtime_error("schema_version exists but contains no version");
    return sqlite3_column_int(statement.get(), 0);
}

void migrate(sqlite3* db)
{
    int version = read_schema_version(db);
    if (version > SQLiteStorage::SCHEMA_VERSION)
        throw std::runtime_error("Database schema version " + std::to_string(version)
            + " is newer than this chatbox build supports ("
            + std::to_string(SQLiteStorage::SCHEMA_VERSION) + ")");

    exec_sql(db, "BEGIN IMMEDIATE;");
    try
    {
        if (version < 1)
        {
            exec_sql(db,
                "CREATE TABLE schema_version (version INTEGER NOT NULL);"
                "INSERT INTO schema_version(version) VALUES (1);"
                "CREATE TABLE rooms ("
                " id INTEGER PRIMARY KEY,"
                " name TEXT NOT NULL COLLATE NOCASE UNIQUE,"
                " topic TEXT NOT NULL DEFAULT '',"
                " created_at INTEGER NOT NULL"
                ");"
                "CREATE TABLE identities ("
                " nickname TEXT PRIMARY KEY,"
                " public_key TEXT NOT NULL,"
                " created_at INTEGER NOT NULL"
                ");"
                "CREATE TABLE bans ("
                " nickname TEXT PRIMARY KEY,"
                " reason TEXT NOT NULL DEFAULT '',"
                " created_at INTEGER NOT NULL,"
                " expires_at INTEGER"
                ");"
                "CREATE TABLE messages ("
                " id TEXT PRIMARY KEY,"
                " room_id INTEGER NOT NULL REFERENCES rooms(id),"
                " nickname TEXT NOT NULL,"
                " body TEXT NOT NULL,"
                " sent_at INTEGER NOT NULL,"
                " signature TEXT,"
                " public_key TEXT"
                ");"
                "CREATE INDEX messages_room_history"
                " ON messages(room_id, sent_at DESC, id DESC);"
                "CREATE INDEX bans_expiration ON bans(expires_at);"
                "INSERT INTO rooms(id, name, topic, created_at)"
                " VALUES (1, 'lobby', 'Welcome to the lobby', strftime('%s','now'));"
            );
            version = 1;
        }

        if (version < 2)
        {
            exec_sql(db,
                "ALTER TABLE rooms ADD COLUMN owner_nickname TEXT NOT NULL DEFAULT '';"
                "UPDATE schema_version SET version = 2;"
            );
            version = 2;
        }

        if (version < 3)
        {
            exec_sql(db,
                "CREATE TABLE legacy_imports ("
                " kind TEXT PRIMARY KEY,"
                " imported_at INTEGER NOT NULL"
                ");"
                "UPDATE schema_version SET version = 3;"
            );
        }
        exec_sql(db, "COMMIT;");
    }
    catch (...)
    {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

struct TopicWrite
{
    protocol::RoomId room_id = 0;
    std::string topic;
};

struct IdentityWrite
{
    std::string nickname;
    std::string public_key;
};

struct BanRemove
{
    std::string nickname;
};

using Write = std::variant<StoredRoom, TopicWrite, IdentityWrite, StoredBan,
    BanRemove, StoredMessage>;

void execute_write(sqlite3* db, const Write& write)
{
    std::visit([&](const auto& item)
        {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, StoredRoom>)
            {
                Statement statement(db,
                    "INSERT INTO rooms(id,name,topic,created_at,owner_nickname)"
                    " VALUES(?,?,?,?,?)"
                    " ON CONFLICT(id) DO UPDATE SET name=excluded.name,"
                    " topic=excluded.topic, owner_nickname=excluded.owner_nickname;");
                statement.bind(1, static_cast<std::int64_t>(item.id));
                statement.bind(2, item.name);
                statement.bind(3, item.topic);
                statement.bind(4, item.created_at);
                statement.bind(5, item.owner_nickname);
                statement.execute();
            }
            else if constexpr (std::is_same_v<T, TopicWrite>)
            {
                Statement statement(db, "UPDATE rooms SET topic=? WHERE id=?;");
                statement.bind(1, item.topic);
                statement.bind(2, static_cast<std::int64_t>(item.room_id));
                statement.execute();
            }
            else if constexpr (std::is_same_v<T, IdentityWrite>)
            {
                Statement statement(db,
                    "INSERT INTO identities(nickname,public_key,created_at) VALUES(?,?,?)"
                    " ON CONFLICT(nickname) DO UPDATE SET public_key=excluded.public_key;");
                statement.bind(1, item.nickname);
                statement.bind(2, item.public_key);
                statement.bind(3, unix_seconds());
                statement.execute();
            }
            else if constexpr (std::is_same_v<T, StoredBan>)
            {
                Statement statement(db,
                    "INSERT INTO bans(nickname,reason,created_at,expires_at) VALUES(?,?,?,?)"
                    " ON CONFLICT(nickname) DO UPDATE SET reason=excluded.reason,"
                    " created_at=excluded.created_at, expires_at=excluded.expires_at;");
                statement.bind(1, item.nickname);
                statement.bind(2, item.reason);
                statement.bind(3, item.created_at);
                if (item.expires_at) statement.bind(4, *item.expires_at);
                else statement.bind_null(4);
                statement.execute();
            }
            else if constexpr (std::is_same_v<T, BanRemove>)
            {
                Statement statement(db, "DELETE FROM bans WHERE nickname=?;");
                statement.bind(1, item.nickname);
                statement.execute();
            }
            else if constexpr (std::is_same_v<T, StoredMessage>)
            {
                Statement statement(db,
                    "INSERT OR IGNORE INTO messages"
                    "(id,room_id,nickname,body,sent_at,signature,public_key)"
                    " VALUES(?,?,?,?,?,?,?);");
                statement.bind(1, item.id);
                statement.bind(2, static_cast<std::int64_t>(item.room_id));
                statement.bind(3, item.nickname);
                statement.bind(4, item.body);
                statement.bind(5, item.sent_at);
                if (item.signature) statement.bind(6, *item.signature);
                else statement.bind_null(6);
                if (item.public_key) statement.bind(7, *item.public_key);
                else statement.bind_null(7);
                statement.execute();
            }
        }, write);
}
}

class SQLiteStorage::Impl
{
public:
    explicit Impl(std::string database_path) : path(std::move(database_path))
    {
        read_db = open_database(path);
        try
        {
            migrate(read_db);
            write_db = open_database(path);
            worker = std::thread([this] { run(); });
        }
        catch (...)
        {
            sqlite3_close(read_db);
            read_db = nullptr;
            throw;
        }
    }

    ~Impl()
    {
        flush();
        {
            std::lock_guard lock(queue_mutex);
            stopping = true;
        }
        queue_ready.notify_one();
        if (worker.joinable())
            worker.join();
        sqlite3_close(write_db);
        sqlite3_close(read_db);
    }

    bool enqueue(Write write)
    {
        std::lock_guard lock(queue_mutex);
        if (stopping || queue.size() >= SQLiteStorage::MAX_PENDING_WRITES)
        {
            error = "SQLite write queue is full";
            return false;
        }
        queue.push_back(std::move(write));
        queue_ready.notify_one();
        return true;
    }

    void flush()
    {
        std::unique_lock lock(queue_mutex);
        drained.wait(lock, [this] { return queue.empty() && !writing; });
    }

    void set_error(const std::string& detail)
    {
        std::lock_guard lock(queue_mutex);
        error = detail;
    }

    std::string get_error() const
    {
        std::lock_guard lock(queue_mutex);
        return error;
    }

    void run()
    {
        while (true)
        {
            std::vector<Write> batch;
            {
                std::unique_lock lock(queue_mutex);
                queue_ready.wait(lock, [this] { return stopping || !queue.empty(); });
                if (stopping && queue.empty())
                    return;
                writing = true;
                const std::size_t count = std::min<std::size_t>(queue.size(), 64);
                batch.reserve(count);
                for (std::size_t index = 0; index < count; ++index)
                {
                    batch.push_back(std::move(queue.front()));
                    queue.pop_front();
                }
            }

            try
            {
                exec_sql(write_db, "BEGIN IMMEDIATE;");
                for (const auto& write : batch)
                    execute_write(write_db, write);
                exec_sql(write_db, "COMMIT;");
            }
            catch (const std::exception& ex)
            {
                sqlite3_exec(write_db, "ROLLBACK;", nullptr, nullptr, nullptr);
                set_error(ex.what());
            }

            {
                std::lock_guard lock(queue_mutex);
                writing = false;
                if (queue.empty())
                    drained.notify_all();
            }
        }
    }

    std::string path;
    sqlite3* read_db = nullptr;
    sqlite3* write_db = nullptr;
    mutable std::mutex read_mutex;
    mutable std::mutex queue_mutex;
    std::condition_variable queue_ready;
    std::condition_variable drained;
    std::deque<Write> queue;
    bool writing = false;
    bool stopping = false;
    std::string error;
    std::thread worker;
};

SQLiteStorage::SQLiteStorage(std::string path)
    : impl_(std::make_unique<Impl>(std::move(path)))
{}

SQLiteStorage::~SQLiteStorage() = default;

std::vector<StoredRoom> SQLiteStorage::load_rooms()
{
    std::lock_guard lock(impl_->read_mutex);
    Statement statement(impl_->read_db,
        "SELECT id,name,topic,owner_nickname,created_at FROM rooms ORDER BY id;");
    std::vector<StoredRoom> rooms;
    while (statement.step_row())
    {
        rooms.push_back({
            static_cast<protocol::RoomId>(sqlite3_column_int64(statement.get(), 0)),
            column_text(statement.get(), 1),
            column_text(statement.get(), 2),
            column_text(statement.get(), 3),
            sqlite3_column_int64(statement.get(), 4)
        });
    }
    return rooms;
}

std::vector<std::pair<std::string, std::string>> SQLiteStorage::load_identities()
{
    std::lock_guard lock(impl_->read_mutex);
    Statement statement(impl_->read_db,
        "SELECT nickname,public_key FROM identities ORDER BY nickname;");
    std::vector<std::pair<std::string, std::string>> identities;
    while (statement.step_row())
        identities.emplace_back(column_text(statement.get(), 0),
            column_text(statement.get(), 1));
    return identities;
}

std::vector<StoredBan> SQLiteStorage::load_bans(std::int64_t now)
{
    std::lock_guard lock(impl_->read_mutex);
    Statement cleanup(impl_->read_db,
        "DELETE FROM bans WHERE expires_at IS NOT NULL AND expires_at <= ?;");
    cleanup.bind(1, now);
    cleanup.execute();

    Statement statement(impl_->read_db,
        "SELECT nickname,reason,created_at,expires_at FROM bans ORDER BY nickname;");
    std::vector<StoredBan> bans;
    while (statement.step_row())
    {
        StoredBan ban;
        ban.nickname = column_text(statement.get(), 0);
        ban.reason = column_text(statement.get(), 1);
        ban.created_at = sqlite3_column_int64(statement.get(), 2);
        if (sqlite3_column_type(statement.get(), 3) != SQLITE_NULL)
            ban.expires_at = sqlite3_column_int64(statement.get(), 3);
        bans.push_back(std::move(ban));
    }
    return bans;
}

HistoryPage SQLiteStorage::load_history(protocol::RoomId room_id,
    std::size_t limit,
    std::optional<HistoryCursor> before)
{
    limit = std::clamp<std::size_t>(limit, 1, MAX_HISTORY_PAGE);
    std::lock_guard lock(impl_->read_mutex);
    const char* sql = before
        ? "SELECT id,room_id,nickname,body,sent_at,signature,public_key FROM messages"
          " WHERE room_id=? AND (sent_at < ? OR (sent_at = ? AND id < ?))"
          " ORDER BY sent_at DESC,id DESC LIMIT ?;"
        : "SELECT id,room_id,nickname,body,sent_at,signature,public_key FROM messages"
          " WHERE room_id=? ORDER BY sent_at DESC,id DESC LIMIT ?;";
    Statement statement(impl_->read_db, sql);
    statement.bind(1, static_cast<std::int64_t>(room_id));
    int limit_index = 2;
    if (before)
    {
        statement.bind(2, before->sent_at);
        statement.bind(3, before->sent_at);
        statement.bind(4, before->id);
        limit_index = 5;
    }
    statement.bind(limit_index, static_cast<std::int64_t>(limit + 1));

    HistoryPage page;
    while (statement.step_row())
    {
        StoredMessage message;
        message.id = column_text(statement.get(), 0);
        message.room_id = static_cast<protocol::RoomId>(
            sqlite3_column_int64(statement.get(), 1));
        message.nickname = column_text(statement.get(), 2);
        message.body = column_text(statement.get(), 3);
        message.sent_at = sqlite3_column_int64(statement.get(), 4);
        if (sqlite3_column_type(statement.get(), 5) != SQLITE_NULL)
            message.signature = column_text(statement.get(), 5);
        if (sqlite3_column_type(statement.get(), 6) != SQLITE_NULL)
            message.public_key = column_text(statement.get(), 6);
        page.messages.push_back(std::move(message));
    }
    if (page.messages.size() > limit)
    {
        page.messages.pop_back();
        const auto& last = page.messages.back();
        page.next = HistoryCursor{ last.sent_at, last.id };
    }
    return page;
}

bool SQLiteStorage::store_room(const StoredRoom& room)
{
    return impl_->enqueue(room);
}

bool SQLiteStorage::update_room_topic(protocol::RoomId room_id,
    const std::string& topic)
{
    return impl_->enqueue(TopicWrite{ room_id, topic });
}

bool SQLiteStorage::store_identity(const std::string& nickname,
    const std::string& public_key)
{
    return impl_->enqueue(IdentityWrite{ nickname, public_key });
}

bool SQLiteStorage::store_ban(const StoredBan& ban)
{
    return impl_->enqueue(ban);
}

bool SQLiteStorage::remove_ban(const std::string& nickname)
{
    return impl_->enqueue(BanRemove{ nickname });
}

bool SQLiteStorage::store_message(const StoredMessage& message)
{
    return impl_->enqueue(message);
}

ImportResult SQLiteStorage::import_legacy_files(const std::string& bans_path,
    const std::string& identities_path)
{
    flush();
    std::lock_guard lock(impl_->read_mutex);
    ImportResult result;

    auto imported = [&](const std::string& kind)
        {
            Statement statement(impl_->read_db,
                "SELECT 1 FROM legacy_imports WHERE kind=?;");
            statement.bind(1, kind);
            return statement.step_row();
        };

    exec_sql(impl_->read_db, "BEGIN IMMEDIATE;");
    try
    {
        if (bans_path.empty())
        {
            // This import kind was not requested.
        }
        else if (imported("bans"))
        {
            result.bans_already_imported = true;
        }
        else
        {
            std::ifstream file(bans_path);
            if (file)
            {
                std::string nickname;
                Statement insert(impl_->read_db,
                    "INSERT OR IGNORE INTO bans(nickname,reason,created_at,expires_at)"
                    " VALUES(?, '', ?, NULL);");
                while (std::getline(file, nickname))
                {
                    if (!is_valid_nickname(nickname))
                        continue;
                    sqlite3_reset(insert.get());
                    sqlite3_clear_bindings(insert.get());
                    insert.bind(1, nickname);
                    insert.bind(2, unix_seconds());
                    insert.execute();
                    result.bans += static_cast<std::size_t>(
                        sqlite3_changes(impl_->read_db));
                }
                Statement mark(impl_->read_db,
                    "INSERT INTO legacy_imports(kind,imported_at) VALUES('bans',?);");
                mark.bind(1, unix_seconds());
                mark.execute();
            }
        }

        if (identities_path.empty())
        {
            // This import kind was not requested.
        }
        else if (imported("identities"))
        {
            result.identities_already_imported = true;
        }
        else
        {
            std::ifstream file(identities_path);
            if (file)
            {
                std::string line;
                Statement insert(impl_->read_db,
                    "INSERT OR IGNORE INTO identities(nickname,public_key,created_at)"
                    " VALUES(?,?,?);");
                while (std::getline(file, line))
                {
                    auto parts = split(line, '|');
                    if (parts.size() != 2 || !is_valid_nickname(parts[0]) ||
                        !is_hex_of_len(parts[1], crypto_sign_PUBLICKEYBYTES))
                        continue;
                    sqlite3_reset(insert.get());
                    sqlite3_clear_bindings(insert.get());
                    insert.bind(1, parts[0]);
                    insert.bind(2, parts[1]);
                    insert.bind(3, unix_seconds());
                    insert.execute();
                    result.identities += static_cast<std::size_t>(
                        sqlite3_changes(impl_->read_db));
                }
                Statement mark(impl_->read_db,
                    "INSERT INTO legacy_imports(kind,imported_at) VALUES('identities',?);");
                mark.bind(1, unix_seconds());
                mark.execute();
            }
        }
        exec_sql(impl_->read_db, "COMMIT;");
    }
    catch (...)
    {
        sqlite3_exec(impl_->read_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
    return result;
}

void SQLiteStorage::flush()
{
    impl_->flush();
}

std::string SQLiteStorage::last_error() const
{
    return impl_->get_error();
}

int SQLiteStorage::schema_version() const
{
    std::lock_guard lock(impl_->read_mutex);
    return read_schema_version(impl_->read_db);
}

const std::string& SQLiteStorage::path() const
{
    return impl_->path;
}
}
