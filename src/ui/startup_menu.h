#pragma once

#include <optional>

// Returns an exit code after quitting or running a headless mode; otherwise
// leaves curses active and continues into interactive chat.
std::optional<int> run_startup_menu();
