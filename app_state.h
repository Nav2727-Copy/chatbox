/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: CC BY-NC-SA 4.0 (https://creativecommons.org/licenses/by-nc-sa/4.0/)
*/
#pragma once

#include "common.h"

extern std::mutex g_mutex;

extern std::deque<std::string> g_messages;
extern std::vector<std::string> g_users;
extern std::string g_nickname;
extern std::vector<chatbox::protocol::RoomSummary> g_rooms;
extern chatbox::protocol::RoomId g_active_room_id;
extern std::string g_active_room_name;
extern std::string g_active_room_topic;

extern std::atomic<bool> g_shutdown_requested;
extern std::atomic<bool> g_kicked;

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
