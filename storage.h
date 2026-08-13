#pragma once

#include "protocol.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace chatbox::storage
{
struct StoredRoom
{
    protocol::RoomId id = 0;
    std::string name;
    std::string topic;
    std::string owner_nickname;
    std::int64_t created_at = 0;
};

struct StoredBan
{
    std::string nickname;
    std::string reason;
    std::int64_t created_at = 0;
    std::optional<std::int64_t> expires_at;
};

struct StoredMessage
{
    std::string id;
    protocol::RoomId room_id = 0;
    std::string nickname;
    std::string body;
    std::int64_t sent_at = 0;
    std::optional<std::string> signature;
    std::optional<std::string> public_key;
};

struct HistoryCursor
{
    std::int64_t sent_at = 0;
    std::string id;
};

struct HistoryPage
{
    std::vector<StoredMessage> messages;
    std::optional<HistoryCursor> next;
};

struct ImportResult
{
    std::size_t bans = 0;
    std::size_t identities = 0;
    bool bans_already_imported = false;
    bool identities_already_imported = false;
};

class ServerStorage
{
public:
    virtual ~ServerStorage() = default;

    virtual std::vector<StoredRoom> load_rooms() = 0;
    virtual std::vector<std::pair<std::string, std::string>> load_identities() = 0;
    virtual std::vector<StoredBan> load_bans(std::int64_t now) = 0;
    virtual HistoryPage load_history(protocol::RoomId room_id,
        std::size_t limit,
        std::optional<HistoryCursor> before = std::nullopt) = 0;

    virtual bool store_room(const StoredRoom& room) = 0;
    virtual bool update_room_topic(protocol::RoomId room_id,
        const std::string& topic) = 0;
    virtual bool store_identity(const std::string& nickname,
        const std::string& public_key) = 0;
    virtual bool store_ban(const StoredBan& ban) = 0;
    virtual bool remove_ban(const std::string& nickname) = 0;
    virtual bool store_message(const StoredMessage& message) = 0;

    virtual ImportResult import_legacy_files(const std::string& bans_path,
        const std::string& identities_path) = 0;
    virtual void flush() = 0;
    virtual std::string last_error() const = 0;
};

class SQLiteStorage final : public ServerStorage
{
public:
    static constexpr int SCHEMA_VERSION = 3;
    static constexpr std::size_t MAX_PENDING_WRITES = 4096;
    static constexpr std::size_t MAX_HISTORY_PAGE = 500;

    explicit SQLiteStorage(std::string path);
    ~SQLiteStorage() override;

    SQLiteStorage(const SQLiteStorage&) = delete;
    SQLiteStorage& operator=(const SQLiteStorage&) = delete;

    std::vector<StoredRoom> load_rooms() override;
    std::vector<std::pair<std::string, std::string>> load_identities() override;
    std::vector<StoredBan> load_bans(std::int64_t now) override;
    HistoryPage load_history(protocol::RoomId room_id,
        std::size_t limit,
        std::optional<HistoryCursor> before = std::nullopt) override;

    bool store_room(const StoredRoom& room) override;
    bool update_room_topic(protocol::RoomId room_id,
        const std::string& topic) override;
    bool store_identity(const std::string& nickname,
        const std::string& public_key) override;
    bool store_ban(const StoredBan& ban) override;
    bool remove_ban(const std::string& nickname) override;
    bool store_message(const StoredMessage& message) override;

    ImportResult import_legacy_files(const std::string& bans_path,
        const std::string& identities_path) override;
    void flush() override;
    std::string last_error() const override;

    int schema_version() const;
    const std::string& path() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
