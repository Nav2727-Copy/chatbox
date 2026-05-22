/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: CC BY-NC-SA 4.0 (https://creativecommons.org/licenses/by-nc-sa/4.0/)
*/
#pragma once

#include "common.h"

void draw_ui(const std::string& input);
std::string prompt_password(int row, int col, const char* label);
bool prompt_yes_no(int row, int col, const char* label, bool default_yes = true);
