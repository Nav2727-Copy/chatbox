/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: GNU GPLv3 (https://www.gnu.org/licenses/gpl-3.0.html)
*/
#pragma once

#include "protocol/protocol.h"

#include <string>
#include <vector>

// A consistent copy of everything needed to draw the chat UI. Rendering does
// not hold the state lock or block incoming network updates.
struct ChatStateSnapshot
{
    std::vector<std::string> messages;
    std::vector<std::string> users;
    std::vector<chatbox::protocol::RoomSummary> rooms;
    chatbox::protocol::RoomId active_room_id = 0;
    std::string active_room_name;
    std::string active_room_topic;
    // Monotonic across clears/room changes so scrollback can track retained messages.
    std::uint64_t message_sequence = 0;
};

ChatStateSnapshot chat_state_snapshot();
void clear_messages();
void update_room_users(chatbox::protocol::RoomId id,
    const std::vector<std::string>& users);
void mark_kicked();
bool was_kicked();

void push_message(const std::string& msg);
std::vector<std::string> message_snapshot();
std::vector<std::string> user_snapshot();
void reset_chat_state();
void activate_room(chatbox::protocol::RoomId id,
    const std::string& name,
    const std::string& topic);
void update_room_topic(chatbox::protocol::RoomId id, const std::string& topic);
void update_room_list(const std::vector<chatbox::protocol::RoomSummary>& rooms);
chatbox::protocol::RoomId active_room_id();
std::string active_room_name();
std::string active_room_topic();
