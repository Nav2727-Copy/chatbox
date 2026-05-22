#include "ui.h"

#include "app_state.h"

void draw_ui(const std::string& input)
{
    std::lock_guard window_lock(g_window_mutex);

    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    if (rows != g_last_rows || cols != g_last_cols || !g_users_win)
    {
        if (g_users_win) delwin(g_users_win);
        if (g_chat_win)  delwin(g_chat_win);
        if (g_input_win) delwin(g_input_win);

        g_users_win = newwin(rows - INPUT_WINDOW_HEIGHT, USERS_WINDOW_WIDTH, 0, 0);
        g_chat_win = newwin(rows - INPUT_WINDOW_HEIGHT, cols - USERS_WINDOW_WIDTH, 0, USERS_WINDOW_WIDTH);
        g_input_win = newwin(INPUT_WINDOW_HEIGHT, cols, rows - INPUT_WINDOW_HEIGHT, 0);

        g_last_rows = rows;
        g_last_cols = cols;

        box(g_users_win, 0, 0);
        box(g_chat_win, 0, 0);
        box(g_input_win, 0, 0);
    }
    else
    {
        wclear(g_users_win);
        wclear(g_chat_win);
        wclear(g_input_win);

        box(g_users_win, 0, 0);
        box(g_chat_win, 0, 0);
        box(g_input_win, 0, 0);
    }

    wattron(g_users_win, COLOR_PAIR(1)); mvwprintw(g_users_win, 0, 2, " Users "); wattroff(g_users_win, COLOR_PAIR(1));
    wattron(g_chat_win, COLOR_PAIR(2)); mvwprintw(g_chat_win, 0, 2, " Chat ");  wattroff(g_chat_win, COLOR_PAIR(2));
    wattron(g_input_win, COLOR_PAIR(3)); mvwprintw(g_input_win, 0, 2, " Input "); wattroff(g_input_win, COLOR_PAIR(3));

    {
        std::lock_guard lock(g_mutex);

        int y = 1;
        for (const auto& user : g_users)
        {
            if (y >= rows - INPUT_WINDOW_HEIGHT - 1) break;
            mvwprintw(g_users_win, y++, 2, "%s", user.c_str());
        }

        int max_chat_lines = rows - INPUT_WINDOW_HEIGHT - 2;
        int start = std::max(0, (int)g_messages.size() - max_chat_lines);
        y = 1;
        for (int i = start; i < (int)g_messages.size(); ++i)
            mvwprintw(g_chat_win, y++, 2, "%s", g_messages[i].c_str());
    }

    mvwprintw(g_input_win, 1, 2, "> %s", input.c_str());

    wnoutrefresh(g_users_win);
    wnoutrefresh(g_chat_win);
    wnoutrefresh(g_input_win);
    doupdate();
}
