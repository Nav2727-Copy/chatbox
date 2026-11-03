#pragma once

#include <functional>
#include <string>
#include <vector>

struct ChatInputContext;

struct CursesMenuItem
{
    char shortcut;
    std::string title;
    std::string description;
};
struct CursesField
{
    std::string label;
    std::string value;
    std::size_t limit = 255;
    bool secret = false;
    bool toggle = false;
    std::function<std::string(const std::string&)> validate = {};
};
struct CursesFormCancelled {};

void initialize_curses_ui();
void shutdown_curses_ui();
void destroy_curses_windows();
enum class ChatExit { Left, Disconnected };
ChatExit run_curses_chat_loop(const ChatInputContext& context, std::string& draft);
void show_walkthrough();
void draw_curses_chat_ui(const std::string& input);
int choose_curses_menu(const std::string& title, const std::string& subtitle,
    const std::vector<CursesMenuItem>& items);
// Escape throws CursesFormCancelled so a setup flow can return to its menu.
void edit_curses_form(const std::string& title, const std::string& subtitle,
    std::vector<CursesField>& fields);
void show_curses_notice(const std::string& title, const std::string& detail, bool wait = true);
std::string prompt_curses_password(int row, int col, const char* label);
bool prompt_curses_yes_no(int row, int col, const char* label, bool default_yes = true);
