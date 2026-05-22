/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: CC BY-NC-SA 4.0 (https://creativecommons.org/licenses/by-nc-sa/4.0/)
*/
#pragma once

#include "common.h"

void initialize_curses_ui();
void shutdown_curses_ui();
void destroy_curses_windows();

void draw_curses_chat_ui(const std::string& input);
std::string prompt_curses_password(int row, int col, const char* label);
bool prompt_curses_yes_no(int row, int col, const char* label, bool default_yes = true);
