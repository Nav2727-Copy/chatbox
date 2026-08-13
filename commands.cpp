#include "commands.h"

#include "app_state.h"
#include "chat_client.h"
#include "chat_server.h"
#include "utils.h"

namespace
{
bool extract_whisper(const std::string& input, std::string& target, std::string& message)
{
    const size_t command_end = input.find(' ');
    if (command_end == std::string::npos)
        return false;

    const size_t target_start = input.find_first_not_of(' ', command_end);
    if (target_start == std::string::npos)
        return false;

    const size_t target_end = input.find(' ', target_start);
    if (target_end == std::string::npos)
        return false;

    const size_t message_start = input.find_first_not_of(' ', target_end);
    if (message_start == std::string::npos)
        return false;

    target = input.substr(target_start, target_end - target_start);
    message = input.substr(message_start);
    return !target.empty() && !message.empty();
}

void show_host_command_help()
{
    push_message("[system] /ban  <nick> [reason] - Ban a user server-wide (host only)");
    push_message("[system] /unban <nick>         - Unban a user (host only)");
    push_message("[system] /bans                 - List server-wide bans");
}
}

std::optional<Command> parse_command(const std::string& input)
{
    if (input.empty() || input[0] != '/')
        return std::nullopt;

    auto parts = split(input.substr(1), ' ');
    if (parts.empty())
        return std::nullopt;

    Command cmd;
    cmd.name = parts[0];
    for (size_t i = 1; i < parts.size(); ++i)
        if (!parts[i].empty()) cmd.args.push_back(parts[i]);

    return cmd;
}

void show_command_help()
{
    push_message("[system] === Available Commands ===");
    push_message("[system] /help                    - Show this help message");
    push_message("[system] /rooms                   - List available rooms");
    push_message("[system] /create <name>           - Create and enter a room");
    push_message("[system] /join <name>             - Switch to a room");
    push_message("[system] /leave                   - Return to the lobby");
    push_message("[system] /topic [text]            - View or set the room topic");
    push_message("[system] /users                   - List users in this room");
    push_message("[system] /clear                   - Clear message history");
    push_message("[system] /time                    - Show current time");
    push_message("[system] /whisper <nick> <msg>    - Whisper within this room");
    push_message("[system] /kick <nick> [reason]    - Kick a room member (room creator/host)");
    push_message("[system] /exit                    - Exit the application");
    push_message("[system] === End Help ===");
}

ChatInputResult handle_chat_input(const std::string& input, const ChatInputContext& context)
{
    if (input.empty())
        return ChatInputResult::Continue;

    auto cmd = parse_command(input);
    if (!cmd)
    {
        if (context.server)
        {
            context.server->broadcast(
                "[" + timestamp() + "] " + context.local_nickname + ": " + input);
        }
        else if (context.client)
        {
            context.client->send(chatbox::protocol::ClientMessage{
                chatbox::protocol::Chat{ input } });
        }
        return ChatInputResult::Continue;
    }

    if (cmd->name == "help")
    {
        show_command_help();
        if (context.host_commands_enabled)
            show_host_command_help();
    }
    else if (cmd->name == "users")
    {
        push_message("[system] Users in #" + active_room_name() + ":");
        for (const auto& user : user_snapshot())
            push_message("[system]   - " + user);
    }
    else if (cmd->name == "rooms")
    {
        if (context.server)
            context.server->show_room_list_local();
        else if (context.client)
            context.client->request_rooms();
    }
    else if (cmd->name == "create")
    {
        if (cmd->args.size() != 1)
        {
            push_message("[system] Usage: /create <name>");
        }
        else if (context.server)
        {
            std::string error;
            if (!context.server->create_local_room(cmd->args[0], error))
                push_message("[system] " + error);
        }
        else if (context.client)
        {
            context.client->send(chatbox::protocol::ClientMessage{
                chatbox::protocol::CreateRoom{ cmd->args[0] } });
        }
    }
    else if (cmd->name == "join")
    {
        if (cmd->args.size() != 1)
        {
            push_message("[system] Usage: /join <name>");
        }
        else if (context.server)
        {
            std::string error;
            if (!context.server->join_local_room(cmd->args[0], error))
                push_message("[system] " + error);
        }
        else if (context.client)
        {
            context.client->send(chatbox::protocol::ClientMessage{
                chatbox::protocol::JoinRoom{ cmd->args[0] } });
        }
    }
    else if (cmd->name == "leave")
    {
        if (!cmd->args.empty())
        {
            push_message("[system] Usage: /leave");
        }
        else if (context.server)
        {
            std::string error;
            if (!context.server->leave_local_room(error))
                push_message("[system] " + error);
        }
        else if (context.client)
        {
            context.client->send(chatbox::protocol::ClientMessage{
                chatbox::protocol::Leave{} });
        }
    }
    else if (cmd->name == "topic")
    {
        if (cmd->args.empty())
        {
            if (context.server)
                push_message("[system] Topic: "
                    + (active_room_topic().empty() ? "(none)" : active_room_topic()));
            else if (context.client)
                context.client->send(chatbox::protocol::ClientMessage{
                    chatbox::protocol::TopicRequest{ std::nullopt } });
        }
        else
        {
            const std::string topic = join_fields(cmd->args, 0, ' ');
            if (context.server)
            {
                std::string error;
                if (!context.server->set_local_topic(topic, error))
                    push_message("[system] " + error);
            }
            else if (context.client)
            {
                context.client->send(chatbox::protocol::ClientMessage{
                    chatbox::protocol::TopicRequest{ topic } });
            }
        }
    }
    else if (cmd->name == "clear")
    {
        {
            std::lock_guard lock(g_mutex);
            g_messages.clear();
        }
        push_message("[system] Message history cleared");
    }
    else if (cmd->name == "time")
    {
        push_message("[system] Current time: " + timestamp());
    }
    else if (cmd->name == "whisper")
    {
        std::string target;
        std::string message;
        if (!extract_whisper(input, target, message))
        {
            push_message("[system] Usage: /whisper <nick> <message>");
        }
        else if (context.server)
        {
            context.server->send_private(context.local_nickname, target, message, true);
        }
        else if (context.client)
        {
            context.client->send(chatbox::protocol::ClientMessage{
                chatbox::protocol::Whisper{ target, message } });
        }
    }
    else if (cmd->name == "kick" && cmd->args.size() >= 1)
    {
        const std::string nick = cmd->args[0];
        const std::string reason = join_fields(cmd->args, 1, ' ');
        if (context.server && context.host_commands_enabled)
        {
            if (!context.server->kick_user(nick, reason))
                push_message("[system] User '" + nick + "' is not in this room");
        }
        else if (context.client)
        {
            context.client->send(chatbox::protocol::ClientMessage{
                chatbox::protocol::KickRequest{ nick, reason } });
        }
    }
    else if (cmd->name == "kick")
    {
        push_message("[system] Usage: /kick <nick> [reason]");
    }
    else if (cmd->name == "ban" && context.server && context.host_commands_enabled && cmd->args.size() >= 1)
    {
        context.server->ban_user(cmd->args[0], join_fields(cmd->args, 1, ' '));
        push_message("[system] " + cmd->args[0] + " has been banned server-wide");
    }
    else if (cmd->name == "unban" && context.server && context.host_commands_enabled && cmd->args.size() >= 1)
    {
        const std::string nick = cmd->args[0];
        if (context.server->unban_user(nick))
            push_message("[system] " + nick + " has been unbanned");
        else
            push_message("[system] '" + nick + "' is not on the ban list");
    }
    else if (cmd->name == "bans" && context.server && context.host_commands_enabled)
    {
        auto bans = context.server->ban_list();
        if (bans.empty())
        {
            push_message("[system] Ban list is empty");
        }
        else
        {
            push_message("[system] Banned users:");
            for (const auto& banned : bans)
                push_message("[system]   - " + banned);
        }
    }
    else if (cmd->name == "exit")
    {
        return ChatInputResult::Exit;
    }
    else
    {
        push_message("[system] Unknown command: " + cmd->name + ". Type /help.");
    }

    return ChatInputResult::Continue;
}
