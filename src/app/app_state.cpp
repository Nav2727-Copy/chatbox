#include "app/app_state.h"
#include "common/constants.h"

#include <atomic>
#include <deque>
#include <mutex>

namespace
{

std::mutex g_mutex;

std::deque<std::string> g_messages;
std::uint64_t g_message_sequence = 0;
std::vector<std::string> g_users;

std::vector<chatbox::protocol::RoomSummary> g_rooms;
chatbox::protocol::RoomId g_active_room_id = 0;
std::string g_active_room_name;
std::string g_active_room_topic;

std::atomic<bool> g_kicked(false);        // set when server sends KICK to this client

}

ChatStateSnapshot chat_state_snapshot()
{
    std::lock_guard lock(g_mutex);
    return { { g_messages.begin(), g_messages.end() }, g_users, g_rooms,
        g_active_room_id, g_active_room_name, g_active_room_topic, g_message_sequence };
}

void clear_messages()
{
    std::lock_guard lock(g_mutex);
    g_messages.clear();
}

void update_room_users(chatbox::protocol::RoomId id,
    const std::vector<std::string>& users)
{
    std::lock_guard lock(g_mutex);
    if (id == g_active_room_id)
        g_users = users;
}

void mark_kicked()
{
    g_kicked = true;
}

bool was_kicked()
{
    return g_kicked.load();
}

void push_message(const std::string& msg)
{
    std::lock_guard lock(g_mutex);
    g_messages.push_back(msg);
    ++g_message_sequence;
    if (g_messages.size() > MAX_MESSAGES)
        g_messages.pop_front();
}

std::vector<std::string> message_snapshot()
{
    std::lock_guard lock(g_mutex);
    return { g_messages.begin(), g_messages.end() };
}

std::vector<std::string> user_snapshot()
{
    std::lock_guard lock(g_mutex);
    return g_users;
}

void reset_chat_state()
{
    std::lock_guard lock(g_mutex);
    g_messages.clear();
    g_users.clear();
    g_rooms.clear();
    g_active_room_id = 0;
    g_active_room_name.clear();
    g_active_room_topic.clear();
    g_kicked = false;
}

void activate_room(chatbox::protocol::RoomId id,
    const std::string& name,
    const std::string& topic)
{
    std::lock_guard lock(g_mutex);
    g_active_room_id = id;
    g_active_room_name = name;
    g_active_room_topic = topic;
    g_messages.clear();
    g_users.clear();
}

void update_room_topic(chatbox::protocol::RoomId id, const std::string& topic)
{
    std::lock_guard lock(g_mutex);
    if (g_active_room_id == id)
        g_active_room_topic = topic;
}

void update_room_list(const std::vector<chatbox::protocol::RoomSummary>& rooms)
{
    std::lock_guard lock(g_mutex);
    g_rooms = rooms;
}

chatbox::protocol::RoomId active_room_id()
{
    std::lock_guard lock(g_mutex);
    return g_active_room_id;
}

std::string active_room_name()
{
    std::lock_guard lock(g_mutex);
    return g_active_room_name;
}

std::string active_room_topic()
{
    std::lock_guard lock(g_mutex);
    return g_active_room_topic;
}
