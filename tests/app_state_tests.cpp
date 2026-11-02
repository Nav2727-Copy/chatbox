#include "app/app_state.h"
#include "common/constants.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace
{
int failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (condition) return;
    std::cerr << "line " << line << ": check failed: " << expression << '\n';
    ++failures;
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void room_changes_and_snapshots()
{
    reset_chat_state();
    activate_room(1, "lobby", "welcome");
    update_room_list({ { 1, "lobby", 1 }, { 2, "games", 0 } });
    update_room_users(1, { "alice" });
    push_message("hello");
    const auto before = chat_state_snapshot();

    activate_room(2, "games", "play");
    update_room_users(1, { "stale user" });
    update_room_topic(1, "stale topic");
    auto after = chat_state_snapshot();
    CHECK(after.active_room_id == 2);
    CHECK(after.active_room_name == "games");
    CHECK(after.active_room_topic == "play");
    CHECK(after.messages.empty());
    CHECK(after.users.empty());
    CHECK(after.rooms.size() == 2);
    CHECK(before.active_room_id == 1);
    CHECK(before.messages == std::vector<std::string>{ "hello" });
    CHECK(after.message_sequence == before.message_sequence);
    CHECK(before.users == std::vector<std::string>{ "alice" });

    update_room_users(2, { "bob" });
    update_room_topic(2, "new topic");
    CHECK(user_snapshot() == std::vector<std::string>{ "bob" });
    CHECK(active_room_topic() == "new topic");
    push_message("new message");
    clear_messages();
    CHECK(message_snapshot().empty());
    CHECK(chat_state_snapshot().message_sequence == before.message_sequence + 1);
    CHECK(user_snapshot().size() == 1);
    CHECK(active_room_id() == 2);
}

void history_and_reset()
{
    reset_chat_state();
    for (int i = 0; i < MAX_MESSAGES + 5; ++i)
        push_message(std::to_string(i));
    const auto messages = message_snapshot();
    CHECK(messages.size() == MAX_MESSAGES);
    CHECK(messages.front() == "5");
    CHECK(messages.back() == std::to_string(MAX_MESSAGES + 4));
    activate_room(1, "lobby", "topic");
    update_room_users(1, { "alice" });
    update_room_list({ { 1, "lobby", 1 } });
    mark_kicked();
    CHECK(was_kicked());
    reset_chat_state();
    const auto state = chat_state_snapshot();
    CHECK(state.messages.empty());
    CHECK(state.users.empty());
    CHECK(state.rooms.empty());
    CHECK(state.active_room_id == 0);
    CHECK(state.active_room_name.empty());
    CHECK(state.active_room_topic.empty());
    CHECK(!was_kicked());
}

void concurrent_room_snapshots()
{
    reset_chat_state();
    activate_room(1, "1", "1");
    std::atomic<bool> start{ false };
    std::thread writer([&] {
        while (!start.load()) std::this_thread::yield();
        for (int i = 1; i <= 5000; ++i)
            activate_room(i, std::to_string(i), std::to_string(i));
    });
    start = true;
    for (int i = 0; i < 5000; ++i)
    {
        const auto state = chat_state_snapshot();
        CHECK(state.active_room_name == std::to_string(state.active_room_id));
        CHECK(state.active_room_topic == state.active_room_name);
    }
    writer.join();
}
}

int main()
{
    room_changes_and_snapshots();
    history_and_reset();
    concurrent_room_snapshots();
    if (failures) return EXIT_FAILURE;
    std::cout << "app state tests passed\n";
}
