#include "ui/curses_ui.h"

#include "app/app_state.h"
#include "app/commands.h"
#include "chat/chat_client.h"
#include "common/constants.h"
#include "ui/terminal_model.h"
#include "ui/display_text.h"

#include <algorithm>
#include <clocale>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <curses.h>

namespace
{
using namespace chatbox::ui;
namespace display = chatbox::ui::ui_text;
bool initialized = false;
bool colors = false;
enum Palette { Accent = 1, Muted, Good, Warning, Selected, Bar };
int style(int pair) { return colors ? COLOR_PAIR(pair) : (pair == Selected ? A_REVERSE : A_NORMAL); }

void put(int y, int x, const std::string& text, int width, int attributes = A_NORMAL)
{
    if (y < 0 || y >= LINES || x < 0 || x >= COLS) return;
    const auto clipped = fit_text(text, std::min(width, COLS - x - 1));
    attrset(attributes);
    mvaddnstr(y, x, clipped.c_str(), static_cast<int>(clipped.size()));
    attrset(A_NORMAL);
}
void rule(int y, int x, int width)
{
    if (y < 0 || y >= LINES || x < 0 || x >= COLS || width < 1) return;
    attrset(style(Muted));
    mvhline(y, x, ACS_HLINE, std::min(width, COLS - x - 1));
    attrset(A_NORMAL);
}
bool too_small()
{
    if (LINES >= 12 && COLS >= 40) return false;
    erase();
    put(0, 1, display::app_name, COLS - 2, style(Accent) | A_BOLD);
    put(2, 1, display::resize_terminal, COLS - 2);
    put(3, 1, display::draft_preserved, COLS - 2, style(Muted));
    put(LINES - 1, 1, display::back, COLS - 2, style(Muted));
    curs_set(0);
    refresh();
    return true;
}
void page(const std::string& title, const std::string& subtitle, const std::string& footer)
{
    erase();
    put(1, 3, display::app_name, COLS - 6, style(Accent) | A_BOLD);
    rule(3, 3, COLS - 6);
    put(5, 3, title, COLS - 6, A_BOLD);
    put(6, 3, subtitle, COLS - 6, style(Muted));
    put(LINES - 2, 3, footer, COLS - 6, style(Muted));
}
void edit_key(LineEditor& editor, int ch, std::size_t limit)
{
    switch (ch)
    {
    case KEY_LEFT: editor.left(); break;
    case KEY_RIGHT: editor.right(); break;
    case KEY_HOME: case 1: editor.cursor = 0; break;
    case KEY_END: case 5: editor.cursor = editor.text.size(); break;
    case KEY_BACKSPACE: case 127: case 8: editor.backspace(); break;
    case KEY_DC: case 4: editor.erase_forward(); break;
    case 23: editor.erase_word(); break;
    case 21: editor.text.erase(0, editor.cursor); editor.cursor = 0; break;
    case 11: editor.text.erase(editor.cursor); break;
    default:
        if (ch >= 32 && ch <= UCHAR_MAX && ch != 127)
            editor.insert(std::string(1, static_cast<char>(ch)), limit);
        break;
    }
}
// Keep the cursor inside the field, including for long paths and wide glyphs.
void draw_editor(const LineEditor& editor, int y, int x, int width, bool secret = false)
{
    std::string text = secret ? std::string(editor.text.size(), '*') : editor.text;
    std::size_t start = 0;
    while (start < editor.cursor && text_width(std::string_view(text).substr(start, editor.cursor - start)) >= width - 1)
    {
        ++start;
        while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) ++start;
    }
    put(y, x, text.substr(start), width);
    const int cursor_x = x + text_width(std::string_view(text).substr(start, editor.cursor - start));
    move(y, std::min(COLS - 2, cursor_x));
    curs_set(1);
}

struct ChatView
{
    LineEditor editor;
    InputHistory history;
    Scrollback scroll;
    std::vector<TranscriptLine> lines;
    int height = 1;
    std::string nickname;
    std::string hint;
    bool host = false;
    bool help = false;
    bool leaving = false;
    chatbox::protocol::RoomId room = 0;
};

void render_chat(ChatView& view)
{
    if (too_small()) return;
    erase();
    const auto state = chat_state_snapshot();
    if (view.room != state.active_room_id || state.messages.empty()) view.scroll.latest();
    view.room = state.active_room_id;
    const bool sidebar = COLS >= 88;
    const int side_width = sidebar ? 25 : 0;
    const int x = side_width + 3;
    const int width = COLS - x - 3;
    const int top = 6;
    view.height = std::max(1, LINES - top - 5);

    put(1, 3, display::app_name, 20, style(Accent) | A_BOLD);
    const std::string identity = view.nickname + (view.host ? display::host_badge : "");
    put(1, std::max(16, COLS - text_width(identity) - 3), identity, COLS - 19, style(Muted));
    rule(2, 3, COLS - 6);
    put(3, x, "# " + (state.active_room_name.empty() ? display::lobby : state.active_room_name),
        width, style(Accent) | A_BOLD);
    put(4, x, state.active_room_topic.empty() ? display::default_topic : state.active_room_topic,
        width, style(Muted));

    if (sidebar)
    {
        attrset(style(Muted));
        mvvline(3, side_width, ACS_VLINE, LINES - 8);
        attrset(A_NORMAL);
        put(3, 3, std::string(display::rooms) + std::to_string(state.rooms.size()), 20, style(Muted) | A_BOLD);
        const int room_slots = std::max(1, (LINES - 12) / 2);
        std::size_t active = 0;
        for (std::size_t i = 0; i < state.rooms.size(); ++i)
            if (state.rooms[i].id == state.active_room_id) active = i;
        const auto first = active >= static_cast<std::size_t>(room_slots)
            ? active - room_slots + 1 : 0;
        int y = 5;
        for (std::size_t i = first; i < state.rooms.size() && y < 5 + room_slots; ++i, ++y)
        {
            const auto& room = state.rooms[i];
            const bool selected = room.id == state.active_room_id;
            auto label = fit_text((selected ? "> " : "  ") + room.name, 16, true);
            label += std::string(std::max(0, 17 - text_width(label)), ' ') + std::to_string(room.user_count);
            put(y, 3, label, 20, selected ? style(Selected) | A_BOLD : A_NORMAL);
        }
        if (state.rooms.empty()) put(y++, 3, std::string("  # ") + display::lobby, 20, style(Selected));
        put(y++, 3, display::switch_room, 20, style(Muted));
        ++y;
        put(y++, 3, std::string(display::people) + std::to_string(state.users.size()), 20, style(Muted) | A_BOLD);
        ++y;
        for (std::size_t i = 0; i < state.users.size() && y < LINES - 6; ++i, ++y)
        {
            if (y == LINES - 7 && i + 1 < state.users.size())
            {
                put(y, 3, "+ " + std::to_string(state.users.size() - i) + display::more_users, 20, style(Muted));
                break;
            }
            put(y, 3, "  " + state.users[i] + (state.users[i] == view.nickname ? display::you : ""),
                20, state.users[i] == view.nickname ? style(Good) : A_NORMAL);
        }
    }

    view.lines.clear();
    for (std::size_t i = 0; i < state.messages.size(); ++i)
    {
        const auto wrapped = wrap_text(state.messages[i], width - 2);
        for (std::size_t part = 0; part < wrapped.size(); ++part)
            view.lines.push_back({ state.message_sequence - state.messages.size() + i,
                part, wrapped[part], part != 0 });
    }
    const auto start = view.scroll.start(view.lines, view.height);
    if (view.lines.empty())
    {
        put(top + 1, x, display::fresh_conversation, width, A_BOLD);
        put(top + 3, x, display::empty_chat_hint, width, style(Muted));
    }
    for (int row = 0; row < view.height && start + row < view.lines.size(); ++row)
    {
        const auto& line = view.lines[start + row];
        const auto message_index = line.message - (state.message_sequence - state.messages.size());
        const auto& original = state.messages[message_index];
        int attributes = A_NORMAL;
        if (original.starts_with("[system]")) attributes = style(Muted);
        else if (original.starts_with("[security]")) attributes = style(Warning);
        else if (original.find("[whisper") != std::string::npos) attributes = style(Accent);
        put(top + row, x + (line.continuation ? 2 : 0), line.text, width - (line.continuation ? 2 : 0), attributes);
        // Give timestamp and author a clear visual hierarchy without changing the wire format.
        if (!line.continuation && attributes == A_NORMAL && line.text.starts_with("["))
        {
            const auto stamp = line.text.find("] ");
            const auto name = line.text.find(": ", stamp == std::string::npos ? 0 : stamp + 2);
            if (stamp != std::string::npos && name != std::string::npos)
            {
                put(top + row, x, line.text.substr(0, stamp + 1), width, style(Muted));
                put(top + row, x + text_width(line.text.substr(0, stamp + 2)),
                    line.text.substr(stamp + 2, name - stamp), width - static_cast<int>(stamp) - 2,
                    style(Accent) | A_BOLD);
            }
        }
    }

    rule(LINES - 5, 3, COLS - 6);
    std::string status = view.hint;
    if (!view.scroll.following())
        status = std::string(display::reading_history) + std::to_string(view.lines.size() - std::min(view.lines.size(), start + view.height))
            + display::lines_below_latest;
    if (!view.scroll.following() && COLS < 88) status = display::compact_history;
    if (status.empty()) status = std::string(display::message_label) + state.active_room_name;
    put(LINES - 4, 3, status, COLS - 18, style(Muted));
    put(LINES - 4, COLS - 13, std::to_string(view.editor.text.size()) + "/" + std::to_string(MAX_CHAT_MESSAGE_LEN), 10, style(Muted));
    put(LINES - 3, 3, ">", 2, style(Accent) | A_BOLD);
    if (view.editor.text.empty()) put(LINES - 3, 5, display::message_placeholder, COLS - 8, style(Muted));
    put(LINES - 1, 3, COLS >= 88
        ? display::chat_keys_wide
        : display::chat_keys_narrow, COLS - 6, style(Muted));
    draw_editor(view.editor, LINES - 3, 5, COLS - 8);

    if (view.help || view.leaving)
    {
        const int panel_width = std::min(66, COLS - 6);
        const int left = (COLS - panel_width) / 2;
        const std::vector<std::string> help = {
            display::keyboard_shortcuts, "", display::send_message, display::move_cursor,
            display::input_edges, display::sent_history,
            display::complete_input, display::scroll_messages,
            display::latest_messages, display::delete_word,
            display::delete_to_cursor, display::switch_rooms_help,
            display::chat_commands_help, "", display::close_panel
        };
        const std::vector<std::string> leave = { display::leave_question, "", display::leave_chat, display::keep_chatting };
        const auto& content = view.leaving ? leave : help;
        const int count = std::min(static_cast<int>(content.size()), LINES - 4);
        const int panel_top = (LINES - count - 2) / 2;
        for (int y = panel_top; y < panel_top + count + 2; ++y)
            put(y, left, std::string(panel_width, ' '), panel_width, style(Bar));
        for (int i = 0; i < count; ++i)
            put(panel_top + 1 + i, left + 2, content[i], panel_width - 4,
                style(Bar) | (i == 0 ? A_BOLD : A_NORMAL));
        if (count < static_cast<int>(content.size()))
            put(panel_top + count, left + 2, display::close_help, panel_width - 4, style(Bar));
        curs_set(0);
    }
    wnoutrefresh(stdscr);
    doupdate();
}

void complete(ChatView& view)
{
    auto& editor = view.editor;
    const auto space = editor.cursor == 0 ? std::string::npos : editor.text.rfind(' ', editor.cursor - 1);
    const auto start = space == std::string::npos ? 0 : space + 1;
    const auto prefix = editor.text.substr(start, editor.cursor - start);
    std::vector<std::string> candidates;
    const auto state = chat_state_snapshot();
    if (start == 0 && prefix.starts_with('/'))
    {
        candidates = { "/help", "/rooms", "/create", "/join", "/leave", "/topic", "/users",
            "/whisper", "/kick", "/clear", "/time", "/exit" };
        if (view.host) candidates.insert(candidates.end(), { "/ban", "/unban", "/bans" });
    }
    else if (editor.text.starts_with("/join "))
        for (const auto& room : state.rooms) candidates.push_back(room.name);
    else candidates = state.users;
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const auto& candidate) {
        return !candidate.starts_with(prefix);
    }), candidates.end());
    std::sort(candidates.begin(), candidates.end());
    if (candidates.empty()) { view.hint = display::no_completions; return; }
    std::string common = candidates.front();
    for (const auto& candidate : candidates)
        while (!candidate.starts_with(common)) common.pop_back();
    // A shared byte prefix can end inside a UTF-8 character.
    while (!common.empty() && common.size() < candidates.front().size()
        && (static_cast<unsigned char>(candidates.front()[common.size()]) & 0xc0) == 0x80)
        common.pop_back();
    const std::string replacement = common + (candidates.size() == 1 ? " " : "");
    if (editor.text.size() - prefix.size() + replacement.size() <= MAX_CHAT_MESSAGE_LEN)
    {
        editor.text.replace(start, prefix.size(), replacement);
        editor.cursor = start + replacement.size();
    }
    view.hint.clear();
    if (candidates.size() > 1)
        for (const auto& candidate : candidates) view.hint += candidate + "  ";
}
}

void initialize_curses_ui()
{
    std::setlocale(LC_ALL, "");
#if !defined(_WIN32)
    // A statically linked vcpkg ncurses can be built with its terminfo
    // directory pointing into the build tree. Search the host's standard
    // terminfo locations at runtime so terminal entries remain available
    // after the executable is moved. Respect an explicit user override.
    if (std::getenv("TERMINFO") == nullptr && std::getenv("TERMINFO_DIRS") == nullptr)
        setenv("TERMINFO_DIRS", "/usr/share/terminfo:/lib/terminfo:/usr/lib/terminfo:/etc/terminfo", 0);
#endif
    // Keep the terminal type selected by the user's terminal emulator. Rewriting
    // xterm-256color to xterm can fail on systems that have only the former's
    // terminfo entry installed.
    initscr();
    initialized = true;
    cbreak(); noecho(); keypad(stdscr, TRUE);
#ifndef _WIN32
    set_escdelay(25);
#endif
    colors = has_colors();
    if (colors)
    {
        start_color();
        int background = COLOR_BLACK;
#ifndef _WIN32
        if (use_default_colors() == OK) background = -1;
#endif
        const bool rich = COLORS >= 256;
        init_pair(Accent, rich ? 80 : COLOR_CYAN, background);
        init_pair(Muted, rich ? 245 : COLOR_WHITE, background);
        init_pair(Good, rich ? 114 : COLOR_GREEN, background);
        init_pair(Warning, rich ? 221 : COLOR_YELLOW, background);
        init_pair(Selected, rich ? 16 : COLOR_BLACK, rich ? 80 : COLOR_CYAN);
        init_pair(Bar, COLOR_WHITE, rich ? 238 : COLOR_BLUE);
    }
}
void destroy_curses_windows() { if (initialized) erase(); }
void shutdown_curses_ui()
{
    if (initialized) { curs_set(1); endwin(); initialized = false; }
}
int choose_curses_menu(const std::string& title, const std::string& subtitle,
    const std::vector<CursesMenuItem>& items)
{
    if (items.empty()) return -1;
    timeout(-1);
    int selected = 0;
    while (true)
    {
        if (!too_small())
        {
            const bool roomy = LINES >= 20;
            const int stride = roomy ? 3 : 1;
            const int top = roomy ? 8 : 7;
            const int slots = std::max(1, (LINES - top - 3) / stride);
            const int first = (selected / slots) * slots;
            page(title, subtitle, display::menu_navigation);
            for (int i = first; i < static_cast<int>(items.size()) && i < first + slots; ++i)
            {
                const int y = top + (i - first) * stride;
                const auto& item = items[i];
                std::string label = i == selected ? " > " : "   ";
                if (item.shortcut) label += std::string(1, static_cast<char>(std::toupper(item.shortcut))) + "  ";
                label += item.title;
                label = fit_text(label, COLS - 8);
                label += std::string(std::max(0, std::min(64, COLS - 8) - text_width(label)), ' ');
                put(y, 3, label, COLS - 6, i == selected ? style(Selected) | A_BOLD : A_NORMAL);
                if (roomy) put(y + 1, 8, item.description, COLS - 12, style(Muted));
            }
            curs_set(0); refresh();
        }
        const int ch = getch();
        if (ch == KEY_F(4)) { show_curses_notice(title, subtitle + "\n\n" + items[selected].title + "\n" + items[selected].description); continue; }
        if (ch == 27) return -1;
        if (LINES < 12 || COLS < 40) continue;
        if (ch == KEY_UP) selected = (selected + items.size() - 1) % items.size();
        else if (ch == KEY_DOWN || ch == '\t') selected = (selected + 1) % items.size();
        else if (ch == KEY_HOME) selected = 0;
        else if (ch == KEY_END) selected = static_cast<int>(items.size()) - 1;
        else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) return selected;
        else if (ch >= 0 && ch <= UCHAR_MAX)
            for (std::size_t i = 0; i < items.size(); ++i)
                if (items[i].shortcut && std::tolower(ch) == items[i].shortcut) return static_cast<int>(i);
    }
}
void edit_curses_form(const std::string& title, const std::string& subtitle,
    std::vector<CursesField>& fields)
{
    if (fields.empty()) return;
    timeout(-1);
    std::size_t selected = 0;
    LineEditor editor;
    editor.set(fields[0].value);
    std::string error;
    while (true)
    {
        fields[selected].value = editor.text;
        if (!too_small())
        {
            page(title, subtitle, display::form_navigation);
            const int slots = std::max(1, (LINES - 11) / 3);
            const auto first = (selected / slots) * slots;
            int editor_y = 9;
            for (std::size_t i = first; i < fields.size() && i < first + slots; ++i)
            {
                int y = 8 + static_cast<int>(i - first) * 3;
                if (LINES < 15) y = 7;
                put(y, 3, fields[i].label + (fields[i].toggle ? display::toggle_help : ""), COLS - 6,
                    i == selected ? style(Accent) | A_BOLD : style(Muted));
                if (i == selected) editor_y = y + 1;
                else put(y + 1, 5, fields[i].secret ? std::string(fields[i].value.size(), '*') : fields[i].value, COLS - 8);
            }
            put(LINES - 3, 3, error.empty() ? "Field " + std::to_string(selected + 1) + " of " + std::to_string(fields.size()) : error,
                COLS - 6, error.empty() ? style(Muted) : style(Warning));
            put(editor_y, 3, ">", 2, style(Accent));
            if (fields[selected].toggle)
            {
                put(editor_y, 5, editor.text == "yes" ? display::yes : display::no, COLS - 8, A_BOLD);
                curs_set(0);
            }
            else draw_editor(editor, editor_y, 5, COLS - 8, fields[selected].secret);
            refresh();
        }
        int ch = getch();
        if (ch == KEY_F(4)) { show_curses_notice(title, subtitle + "\n\n" + fields[selected].label); continue; }
        if (ch == 27) throw CursesFormCancelled{};
        if (LINES < 12 || COLS < 40) continue;
        const bool submit = ch == '\n' || ch == '\r' || ch == KEY_ENTER;
        const bool back = ch == KEY_BTAB || ch == KEY_UP;
        const bool next = ch == '\t' || ch == KEY_DOWN || submit;
        if (back || next)
        {
            fields[selected].value = editor.text;
            error.clear();
            if (submit)
            {
                if (fields[selected].validate) error = fields[selected].validate(editor.text);
                if (!error.empty()) continue;
                if (selected == fields.size() - 1)
                {
                    for (std::size_t i = 0; i < fields.size(); ++i)
                    {
                        if (fields[i].validate) error = fields[i].validate(fields[i].value);
                        if (!error.empty()) { selected = i; editor.set(fields[i].value); break; }
                    }
                    if (error.empty()) return;
                    continue;
                }
            }
            selected = back ? (selected + fields.size() - 1) % fields.size() : (selected + 1) % fields.size();
            editor.set(fields[selected].value);
        }
        else if (fields[selected].toggle)
        {
            if (ch == KEY_LEFT || ch == KEY_RIGHT || ch == ' ') editor.set(editor.text == "yes" ? "no" : "yes");
            else if (ch == 'y' || ch == 'Y') editor.set("yes");
            else if (ch == 'n' || ch == 'N') editor.set("no");
        }
        else { edit_key(editor, ch, fields[selected].limit); if (ch != ERR && ch != KEY_RESIZE) error.clear(); }
    }
}
void show_curses_notice(const std::string& title, const std::string& detail, bool wait)
{
    timeout(-1);
    int offset = 0;
    do
    {
        int max_offset = 0;
        if (!too_small())
        {
            auto lines = wrap_text(detail, COLS - 6);
            const int top = 6;
            const int available = std::max(1, LINES - top - 3);
            max_offset = std::max(0, static_cast<int>(lines.size()) - available);
            offset = std::clamp(offset, 0, max_offset);
            page(title, "", wait ? (max_offset > 0 ? "Up/Down: scroll | Enter/Esc: back" : display::continue_back)
                : display::please_wait);
            for (int row = 0; row < available && row + offset < static_cast<int>(lines.size()); ++row)
                put(top + row, 3, lines[row + offset], COLS - 6);
            curs_set(0); refresh();
        }
        if (!wait) return;
        const int ch = getch();
        if (ch == KEY_DOWN || ch == KEY_NPAGE) offset = std::min(max_offset, offset + 1);
        if (ch == KEY_UP || ch == KEY_PPAGE) offset = std::max(0, offset - 1);
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER || ch == 27) return;
    } while (true);
}
std::string prompt_curses_password(int, int, const char* label)
{
    std::vector<CursesField> fields = { {label, "", PASS_MAX_LEN, true} };
    edit_curses_form(display::server_password, display::password_required, fields);
    return fields[0].value;
}
bool prompt_curses_yes_no(int, int, const char* label, bool default_yes)
{
    std::vector<CursesField> fields = { {label, default_yes ? "yes" : "no", 3, false, true} };
    edit_curses_form("Connection settings", "", fields);
    return fields[0].value == "yes";
}
void draw_curses_chat_ui(const std::string& input)
{
    ChatView view;
    view.editor.set(input);
    render_chat(view);
}
void show_walkthrough()
{
    show_curses_notice("Chatbox guide",
        "JOIN: ask the host for an invite or an address and TCP port (usually 7777). "
        "Review invite details before joining. A fingerprint only identifies the host if you received it through a trusted channel.\n\n"
        "HOST: choose Host, keep TLS enabled, then generate a certificate. Share a LAN address with people on the same network. "
        "Internet guests need your public address and TCP forwarding; UPnP success does not verify reachability. F3 opens hosting details and invites.\n\n"
        "SECURITY: TLS encrypts traffic between each person and the server, not end-to-end between participants. "
        "A password controls access; it does not replace encryption. Compare unknown or changed fingerprints with the host.\n\n"
        "ROOMS: F2 selects a room. /create music creates one; /join music enters it; /topic Listening together changes its topic.\n\n"
        "PEOPLE: /users lists people; /whisper alex hello sends a private message in this room. "
        "If you are alone, share an invite from F3 or switch rooms.\n\n"
        "KEYBOARD: Enter sends; Tab completes; Left/Right and Home/End edit; Up/Down recall sent messages; "
        "PgUp/PgDn scroll; Ctrl+G returns to latest. F1 opens this guide; F3 shows connection/security details. "
        "Esc or /exit returns to server selection. Unsent drafts remain in memory.\n\n"
        "RECOVERY: dropped connections retry up to three times. Esc cancels. Password, ban, nickname and certificate errors need your review. "
        "Messages with uncertain delivery are never resent automatically.");
}

ChatExit run_curses_chat_loop(const ChatInputContext& context, std::string& draft)
{
    ChatView view;
    view.editor.set(draft);
    struct SaveDraft { std::string& draft; ChatView& view; ~SaveDraft() { draft = view.editor.text; } } save{draft, view};
    view.nickname = context.local_nickname;
    view.host = context.host_commands_enabled;
    timeout(100);
    while (true)
    {
        if (context.client && (!context.client->connected() || was_kicked()))
        {
            render_chat(view);
            if (was_kicked()) show_curses_notice(display::session_ended, context.client->kick_reason());
            else push_message("[system] Connection ended: " + context.client->disconnect_reason());
            return ChatExit::Disconnected;
        }
        render_chat(view);
        const int ch = getch();
        if (ch == ERR || ch == KEY_RESIZE) continue;
        if (view.leaving)
        {
            if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) return ChatExit::Left;
            if (ch == 27) view.leaving = false;
            continue;
        }
        if (view.help)
        {
            if (ch == 27 || ch == KEY_F(1)) view.help = false;
            continue;
        }
        if (ch == 27) { view.leaving = true; continue; }
        if (LINES < 12 || COLS < 40) continue;
        view.hint.clear();
        switch (ch)
        {
        case KEY_F(1): show_walkthrough(); timeout(100); break;
        case KEY_F(3): show_curses_notice("Connection / invite", context.connection_details); timeout(100); break;
        case KEY_F(2):
        {
            const auto state = chat_state_snapshot();
            std::vector<CursesMenuItem> items;
            for (const auto& room : state.rooms)
                items.push_back({0, "# " + room.name, std::to_string(room.user_count) + " people"});
            const int choice = choose_curses_menu(display::switch_room_title, display::draft_stays, items);
            timeout(100);
            if (choice >= 0)
                handle_chat_input("/join " + state.rooms[choice].name, context);
            break;
        }
        case '\n': case '\r': case KEY_ENTER:
            if (!view.editor.text.empty())
            {
                if (handle_chat_input(view.editor.text, context) == ChatInputResult::Exit) { view.editor.set(""); return ChatExit::Left; }
                view.history.remember(view.editor.text);
                view.editor.set("");
                view.scroll.latest();
            }
            break;
        case KEY_UP: view.history.previous(view.editor); break;
        case KEY_DOWN: view.history.next(view.editor); break;
        case KEY_PPAGE: view.scroll.move_by(view.lines, view.height, -std::max(1, view.height - 1)); break;
        case KEY_NPAGE: view.scroll.move_by(view.lines, view.height, std::max(1, view.height - 1)); break;
        case 7: view.scroll.latest(); break;
        case '\t': complete(view); break;
        case 12: clearok(stdscr, TRUE); break;
        default: edit_key(view.editor, ch, MAX_CHAT_MESSAGE_LEN); break;
        }
    }
}
