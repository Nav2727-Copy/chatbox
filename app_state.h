/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: CC BY-NC-SA 4.0 (https://creativecommons.org/licenses/by-nc-sa/4.0/)
*/
#pragma once

#include "common.h"

extern std::mutex g_mutex;
extern std::mutex g_window_mutex;

extern std::deque<std::string> g_messages;
extern std::vector<std::string> g_users;
extern std::string g_nickname;

extern WINDOW* g_users_win;
extern WINDOW* g_chat_win;
extern WINDOW* g_input_win;

extern int g_last_rows;
extern int g_last_cols;

extern std::atomic<bool> g_shutdown_requested;
extern std::atomic<bool> g_kicked;

void push_message(const std::string& msg);
void add_user(const std::string& user);
void remove_user(const std::string& user);
std::vector<std::string> connected_users();
