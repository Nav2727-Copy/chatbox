#include "ui/terminal_model.h"

#include <algorithm>
#include <climits>
#include <cwchar>
#include <utility>

namespace chatbox::ui
{
namespace
{
std::size_t previous_char(std::string_view text, std::size_t at)
{
    if (!at) return 0;
    --at;
    while (at && (static_cast<unsigned char>(text[at]) & 0xc0) == 0x80) --at;
    return at;
}
std::size_t next_char(std::string_view text, std::size_t at)
{
    if (at >= text.size()) return text.size();
    ++at;
    while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xc0) == 0x80) ++at;
    return at;
}
struct Glyph { std::string text; int width; };
std::vector<Glyph> glyphs(std::string_view text)
{
    std::vector<Glyph> result;
    std::mbstate_t state{};
    for (std::size_t at = 0; at < text.size();)
    {
        wchar_t character{};
        auto count = std::mbrtowc(&character, text.data() + at, text.size() - at, &state);
        if (count == static_cast<std::size_t>(-1) || count == static_cast<std::size_t>(-2))
        {
            result.push_back({ "?", 1 });
            state = {};
            ++at;
            continue;
        }
        if (count == 0) count = 1;
        if (character < 32 || (character >= 127 && character < 160))
            result.push_back({ " ", 1 });
        else
        {
#ifdef _WIN32
            const int width = (character >= 0x1100 &&
                (character <= 0x115f || (character >= 0x2e80 && character <= 0xa4cf)
                    || (character >= 0xac00 && character <= 0xd7a3)
                    || (character >= 0xf900 && character <= 0xfaff)
                    || (character >= 0xff01 && character <= 0xff60))) ? 2 : 1;
#else
            const int width = ::wcwidth(character);
#endif
            result.push_back(width < 0 ? Glyph{ "?", 1 }
                : Glyph{ std::string(text.substr(at, count)), width });
        }
        at += count;
    }
    return result;
}
}

std::string display_text(std::string_view text)
{
    std::string result;
    for (const auto& glyph : glyphs(text)) result += glyph.text;
    return result;
}
int text_width(std::string_view text)
{
    int width = 0;
    for (const auto& glyph : glyphs(text)) width += glyph.width;
    return width;
}
std::string fit_text(std::string_view text, int width, bool ellipsis)
{
    if (width <= 0) return {};
    const bool shortened = ellipsis && text_width(text) > width && width >= 3;
    const int available = shortened ? width - 3 : width;
    std::string result;
    int used = 0;
    for (const auto& glyph : glyphs(text))
    {
        if (used + glyph.width > available) break;
        result += glyph.text;
        used += glyph.width;
    }
    if (shortened) result += "...";
    return result;
}
std::vector<std::string> wrap_text(std::string_view text, int width)
{
    if (width < 1) return {};
    std::vector<std::string> lines;
    std::string line;
    int used = 0;
    for (const auto& glyph : glyphs(text))
    {
        if (used + glyph.width > width && !line.empty())
        {
            // Break at a nearby space when possible, preserving the remaining word.
            const auto space = line.rfind(' ');
            if (space != std::string::npos && text_width(line.substr(0, space)) >= width / 2)
            {
                lines.push_back(line.substr(0, space));
                line.erase(0, space + 1);
                used = text_width(line);
            }
            else
            {
                lines.push_back(line);
                line.clear();
                used = 0;
            }
        }
        if (glyph.width > width) { line += '?'; ++used; }
        else { line += glyph.text; used += glyph.width; }
    }
    if (!line.empty() || lines.empty()) lines.push_back(line);
    return lines;
}

void LineEditor::set(std::string value) { text = std::move(value); cursor = text.size(); }
void LineEditor::left() { cursor = previous_char(text, cursor); }
void LineEditor::right() { cursor = next_char(text, cursor); }
void LineEditor::backspace()
{
    const auto start = previous_char(text, cursor);
    text.erase(start, cursor - start);
    cursor = start;
}
void LineEditor::erase_forward() { text.erase(cursor, next_char(text, cursor) - cursor); }
void LineEditor::erase_word()
{
    while (cursor && text[cursor - 1] == ' ') backspace();
    while (cursor && text[cursor - 1] != ' ') backspace();
}
bool LineEditor::insert(std::string_view value, std::size_t limit)
{
    if (text.size() + value.size() > limit) return false;
    text.insert(cursor, value);
    cursor += value.size();
    return true;
}
void InputHistory::remember(const std::string& text)
{
    if (!text.empty() && (entries_.empty() || entries_.back() != text))
        entries_.push_back(text);
    if (entries_.size() > 100) entries_.erase(entries_.begin());
    reset();
}
void InputHistory::previous(LineEditor& editor)
{
    if (position_ == entries_.size()) draft_ = editor.text;
    if (position_) editor.set(entries_[--position_]);
}
void InputHistory::next(LineEditor& editor)
{
    if (position_ < entries_.size())
    {
        ++position_;
        editor.set(position_ == entries_.size() ? draft_ : entries_[position_]);
    }
}
void InputHistory::reset() { position_ = entries_.size(); draft_.clear(); }

std::size_t Scrollback::start(const std::vector<TranscriptLine>& lines, int height) const
{
    const auto page = static_cast<std::size_t>(std::max(1, height));
    const auto last = lines.size() > page ? lines.size() - page : 0;
    if (follow_) return last;
    const auto found = std::lower_bound(lines.begin(), lines.end(), std::pair{message_, part_},
        [](const TranscriptLine& line, const auto& anchor) {
            return std::pair{line.message, line.part} < anchor;
        });
    return std::min(last, static_cast<std::size_t>(found - lines.begin()));
}
void Scrollback::move_by(const std::vector<TranscriptLine>& lines, int height, int delta)
{
    if (lines.empty()) return;
    const auto last = std::max(0, static_cast<int>(lines.size()) - std::max(1, height));
    const auto target = std::clamp(static_cast<int>(start(lines, height)) + delta, 0, last);
    follow_ = target == last;
    message_ = lines[target].message;
    part_ = lines[target].part;
}
void Scrollback::latest() { follow_ = true; }
}
