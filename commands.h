/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: CC BY-NC-SA 4.0 (https://creativecommons.org/licenses/by-nc-sa/4.0/)
*/
#pragma once

#include "common.h"
struct Command
{
    std::string name;
    std::vector<std::string> args;
};
std::optional<Command> parse_command(const std::string& input);
void show_command_help();
