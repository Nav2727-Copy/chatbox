#include "ui/terminal_model.h"

#include <clocale>
#include <cstdlib>
#include <iostream>

using namespace chatbox::ui;
namespace
{
int failures = 0;
void check(bool value, int line)
{
    if (!value) { std::cerr << "terminal check failed at line " << line << '\n'; ++failures; }
}
#define CHECK(value) check((value), __LINE__)
}
int main()
{
    std::setlocale(LC_ALL, "");
    LineEditor input;
    input.set("hello world");
    input.cursor = 5;
    CHECK(input.insert(",", 20));
    CHECK(input.text == "hello, world");
    input.left(); input.erase_forward();
    CHECK(input.text == "hello world");
    input.cursor = input.text.size(); input.erase_word();
    CHECK(input.text == "hello ");
    CHECK(!input.insert("too long", 8));
    input.set(""); input.backspace(); input.erase_forward(); input.left(); input.right();
    CHECK(input.cursor == 0);
    input.set("caf\xc3\xa9"); input.left();
    CHECK(input.cursor == 3);
    input.erase_forward(); CHECK(input.text == "caf");

    InputHistory history;
    history.remember("one"); history.remember("two"); history.remember("two");
    input.set("unfinished draft");
    history.previous(input); CHECK(input.text == "two");
    history.previous(input); CHECK(input.text == "one");
    history.previous(input); CHECK(input.text == "one");
    history.next(input); CHECK(input.text == "two");
    history.next(input); CHECK(input.text == "unfinished draft");

    CHECK(fit_text("abcdefgh", 5, true) == "ab...");
    CHECK(fit_text("abc", 0).empty());
    CHECK(display_text("hello\x1b[31m\tworld") == "hello [31m world");
    CHECK(wrap_text("", 10).size() == 1);
    CHECK(wrap_text("abc", 0).empty());
    for (int width = 1; width < 40; ++width)
        for (const auto& line : wrap_text("A long message with words and averylongunbrokenidentifier to wrap.", width))
            CHECK(text_width(line) <= width);
    if (text_width("\xc3\xa9") == 1)
    {
        CHECK(fit_text("caf\xc3\xa9", 4) == "caf\xc3\xa9");
        CHECK(wrap_text("\xc3\xa9\xc3\xa9", 1).size() == 2);
    }

    std::vector<TranscriptLine> lines;
    for (int i = 0; i < 20; ++i) lines.push_back({static_cast<std::uint64_t>(i), 0, "line", false});
    Scrollback scroll;
    CHECK(scroll.start(lines, 5) == 15);
    scroll.move_by(lines, 5, -4);
    CHECK(scroll.start(lines, 5) == 11);
    lines.push_back({20, 0, "incoming", false});
    CHECK(scroll.start(lines, 5) == 11);
    lines.erase(lines.begin());
    CHECK(scroll.start(lines, 5) == 10); // Still anchored to message 11 after eviction.
    CHECK(scroll.start(lines, 8) == 10); // A taller terminal preserves the anchor.
    scroll.move_by(lines, 5, 100);
    CHECK(scroll.following());
    scroll.move_by(lines, 5, -100);
    CHECK(scroll.start(lines, 5) == 0);
    scroll.latest(); CHECK(scroll.following());
    CHECK(scroll.start({}, 5) == 0);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
