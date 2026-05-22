/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: CC BY-NC-SA 4.0 (https://creativecommons.org/licenses/by-nc-sa/4.0/)
*/
#pragma once

#include <curses.h>
#include <boost/asio.hpp>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <ctime>

using boost::asio::ip::tcp;

constexpr int MAX_MESSAGES = 200;
constexpr int SERVER_HISTORY_LIMIT = 100;
constexpr int RATE_LIMIT_MESSAGES = 20;
constexpr int RATE_LIMIT_WINDOW_SECONDS = 60;
constexpr int MAX_WIRE_LINE_LENGTH = 4096;
constexpr int MAX_CHAT_MESSAGE_LEN = 1000;
constexpr int USERS_WINDOW_WIDTH = 24;
constexpr int INPUT_WINDOW_HEIGHT = 3;
constexpr int NICKNAME_BUF_SIZE = 32;
constexpr int HOST_BUF_SIZE = 64;
constexpr int PORT_BUF_SIZE = 16;
constexpr int PASS_BUF_SIZE = 64;
constexpr int LOGFILE_BUF_SIZE = 260;
constexpr int NICK_MAX_LEN = 31;
constexpr int HOST_MAX_LEN = 63;
constexpr int PORT_MAX_LEN = 15;
constexpr int PASS_MAX_LEN = 63;
constexpr int LOGFILE_MAX_LEN = 259;
constexpr int IDENTITY_CHALLENGE_BYTES = 32;
constexpr int BROWSER_PORT = 2727;
constexpr int BROWSER_NAME_MAX_LEN = 48;
constexpr int BROWSER_ENTRY_TTL_SECONDS = 180;
constexpr int BROWSER_PUBLISH_INTERVAL_SECONDS = 60;
constexpr int BROWSER_CONNECT_TIMEOUT_SECONDS = 5;
