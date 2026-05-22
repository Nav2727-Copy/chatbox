#include "curses_ui.h"

#include "app_state.h"

#include <curses.h>

namespace
{
std::mutex window_mutex;

WINDOW* users_win = nullptr;
WINDOW* chat_win = nullptr;
WINDOW* input_win = nullptr;

int last_rows = 0;
int last_cols = 0;

bool curses_initialized = false;

void erase_password_char(int row, int col, int index)
{
    move(row, col + index);
    addch(' ');
    move(row, col + index);
}
}

void initialize_curses_ui()
{
    initscr();
    curses_initialized = true;
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    start_color();

    init_pair(1, COLOR_CYAN, COLOR_BLACK);
    init_pair(2, COLOR_GREEN, COLOR_BLACK);
    init_pair(3, COLOR_YELLOW, COLOR_BLACK);
}

void destroy_curses_windows()
{
    std::lock_guard window_lock(window_mutex);

    if (users_win) delwin(users_win);
    if (chat_win)  delwin(chat_win);
    if (input_win) delwin(input_win);

    users_win = nullptr;
    chat_win = nullptr;
    input_win = nullptr;
    last_rows = 0;
    last_cols = 0;
}

void shutdown_curses_ui()
{
    destroy_curses_windows();

    if (curses_initialized)
    {
        endwin();
        curses_initialized = false;
    }
}

void draw_curses_chat_ui(const std::string& input)
{
    std::lock_guard window_lock(window_mutex);

    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    if (rows != last_rows || cols != last_cols || !users_win)
    {
        if (users_win) delwin(users_win);
        if (chat_win)  delwin(chat_win);
        if (input_win) delwin(input_win);

        users_win = newwin(rows - INPUT_WINDOW_HEIGHT, USERS_WINDOW_WIDTH, 0, 0);
        chat_win = newwin(rows - INPUT_WINDOW_HEIGHT, cols - USERS_WINDOW_WIDTH, 0, USERS_WINDOW_WIDTH);
        input_win = newwin(INPUT_WINDOW_HEIGHT, cols, rows - INPUT_WINDOW_HEIGHT, 0);

        last_rows = rows;
        last_cols = cols;

        box(users_win, 0, 0);
        box(chat_win, 0, 0);
        box(input_win, 0, 0);
    }
    else
    {
        wclear(users_win);
        wclear(chat_win);
        wclear(input_win);

        box(users_win, 0, 0);
        box(chat_win, 0, 0);
        box(input_win, 0, 0);
    }

    wattron(users_win, COLOR_PAIR(1)); mvwprintw(users_win, 0, 2, " Users "); wattroff(users_win, COLOR_PAIR(1));
    wattron(chat_win, COLOR_PAIR(2)); mvwprintw(chat_win, 0, 2, " Chat ");  wattroff(chat_win, COLOR_PAIR(2));
    wattron(input_win, COLOR_PAIR(3)); mvwprintw(input_win, 0, 2, " Input "); wattroff(input_win, COLOR_PAIR(3));

    {
        std::lock_guard lock(g_mutex);

        int y = 1;
        for (const auto& user : g_users)
        {
            if (y >= rows - INPUT_WINDOW_HEIGHT - 1) break;
            mvwprintw(users_win, y++, 2, "%s", user.c_str());
        }

        int max_chat_lines = rows - INPUT_WINDOW_HEIGHT - 2;
        int start = std::max(0, static_cast<int>(g_messages.size()) - max_chat_lines);
        y = 1;
        for (int i = start; i < static_cast<int>(g_messages.size()); ++i)
            mvwprintw(chat_win, y++, 2, "%s", g_messages[i].c_str());
    }

    mvwprintw(input_win, 1, 2, "> %s", input.c_str());

    wnoutrefresh(users_win);
    wnoutrefresh(chat_win);
    wnoutrefresh(input_win);
    doupdate();
}

std::string prompt_curses_password(int row, int col, const char* label)
{
    std::string password;

    mvprintw(row, col, "%s", label);
    move(row + 1, col);
    refresh();

    while (true)
    {
        int ch = getch();
        if (ch == '\n' || ch == '\r')
            break;

        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8)
        {
            if (!password.empty())
            {
                password.pop_back();
                erase_password_char(row + 1, col, static_cast<int>(password.size()));
                refresh();
            }
            continue;
        }

        if (std::isprint(ch) && password.size() < PASS_MAX_LEN)
        {
            password += static_cast<char>(ch);
            addch('*');
            refresh();
        }
    }

    return password;
}

bool prompt_curses_yes_no(int row, int col, const char* label, bool default_yes)
{
    mvprintw(row, col, "%s", label);
    refresh();

    while (true)
    {
        int ch = getch();
        if (ch == '\n' || ch == '\r')
            return default_yes;
        if (ch == 'y' || ch == 'Y')
            return true;
        if (ch == 'n' || ch == 'N')
            return false;
    }
}
