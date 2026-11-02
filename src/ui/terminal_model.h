#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace chatbox::ui
{
// Text helpers measure terminal cells, keep UTF-8 sequences intact, and remove
// control characters before displaying untrusted chat text.
std::string display_text(std::string_view text);
int text_width(std::string_view text);
std::string fit_text(std::string_view text, int width, bool ellipsis = false);
std::vector<std::string> wrap_text(std::string_view text, int width);

class LineEditor
{
public:
    std::string text;
    std::size_t cursor = 0;
    void set(std::string value);
    void left();
    void right();
    void backspace();
    void erase_forward();
    void erase_word();
    bool insert(std::string_view value, std::size_t limit);
};

class InputHistory
{
public:
    void remember(const std::string& text);
    void previous(LineEditor& editor);
    void next(LineEditor& editor);
    void reset();
private:
    std::vector<std::string> entries_;
    std::size_t position_ = 0;
    std::string draft_;
};

struct TranscriptLine
{
    std::uint64_t message = 0;
    std::size_t part = 0;
    std::string text;
    bool continuation = false;
};

// A message/line anchor keeps incoming traffic from moving the text being read,
// including when the bounded message buffer evicts its oldest entry.
class Scrollback
{
public:
    std::size_t start(const std::vector<TranscriptLine>& lines, int height) const;
    void move_by(const std::vector<TranscriptLine>& lines, int height, int delta);
    void latest();
    bool following() const { return follow_; }
private:
    bool follow_ = true;
    std::uint64_t message_ = 0;
    std::size_t part_ = 0;
};
}
