#include "app_state.h"

std::mutex g_mutex;

std::deque<std::string> g_messages;
std::vector<std::string> g_users;

std::string g_nickname;

std::atomic<bool> g_shutdown_requested(false);
std::atomic<bool> g_kicked(false);        // set when server sends KICK to this client

void push_message(const std::string& msg)
{
    std::lock_guard lock(g_mutex);
    g_messages.push_back(msg);
    if (g_messages.size() > MAX_MESSAGES)
        g_messages.pop_front();
}

void add_user(const std::string& user)
{
    std::lock_guard lock(g_mutex);
    if (std::find(g_users.begin(), g_users.end(), user) == g_users.end())
        g_users.push_back(user);
}

void remove_user(const std::string& user)
{
    std::lock_guard lock(g_mutex);
    g_users.erase(
        std::remove(g_users.begin(), g_users.end(), user),
        g_users.end());
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
    g_shutdown_requested = false;
    g_kicked = false;
}

std::vector<std::string> connected_users()
{
    std::lock_guard lock(g_mutex);
    return g_users;
}
