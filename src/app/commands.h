/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: GNU GPLv3 (https://www.gnu.org/licenses/gpl-3.0.html)
*/
#pragma once

#include <optional>
#include <string>
#include <vector>

class ChatClient;
class ChatServer;

struct Command
{
    std::string name;
    std::vector<std::string> args;
};

struct ChatInputContext
{
    ChatClient* client = nullptr;
    ChatServer* server = nullptr;
    std::string local_nickname;
    bool host_commands_enabled = false;
    std::string connection_details;
};

enum class ChatInputResult
{
    Continue,
    Exit
};

std::optional<Command> parse_command(const std::string& input);
void show_command_help();
ChatInputResult handle_chat_input(const std::string& input, const ChatInputContext& context);
