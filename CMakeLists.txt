# CMakeList.txt : CMake project for chatbox
cmake_minimum_required (VERSION 3.10)

# Setup vcpkg integration with platform-specific paths
if(WIN32)
  set(CMAKE_TOOLCHAIN_FILE "C:/Users/Nisha/vcpkg/scripts/buildsystems/vcpkg.cmake"
    CACHE STRING "Vcpkg toolchain file")
elseif(UNIX)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake"
    CACHE STRING "Vcpkg toolchain file")
endif()

if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

set(VCPKG_TARGET_TRIPLET x64-windows-static)
project ("chatbox")

if (WIN32)
  add_definitions(-D_WIN32_WINNT=0x0601)
endif()

# ---- Packages ----
find_package(Boost REQUIRED COMPONENTS asio)
find_package(FLTK CONFIG REQUIRED)
find_package(unofficial-pdcurses CONFIG REQUIRED)
find_package(miniupnpc CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)

set(CHATBOX_CORE_SOURCES
  "app_state.cpp"
  "app_state.h"
  "chat_client.h"
  "chat_log.h"
  "chat_server.h"
  "client_session.cpp"
  "commands.cpp"
  "commands.h"
  "common.h"
  "dedicated_server.cpp"
  "dedicated_server.h"
  "server_browser.cpp"
  "server_browser.h"
  "upnp_mapper.h"
  "utils.cpp"
  "utils.h"
)

add_library(chatbox_core STATIC ${CHATBOX_CORE_SOURCES})
target_link_libraries(chatbox_core
  PUBLIC
    Boost::asio
    miniupnpc::miniupnpc
    unofficial-sodium::sodium
)
target_compile_features(chatbox_core PUBLIC cxx_std_20)

add_library(chatbox_curses_frontend STATIC
  "curses_app.h"
  "curses_ui.cpp"
  "curses_ui.h"
  "interactive_app.cpp"
  "interactive_app.h"
)
target_link_libraries(chatbox_curses_frontend
  PUBLIC
    chatbox_core
  PRIVATE
    unofficial::pdcurses::pdcurses
)
target_compile_features(chatbox_curses_frontend PUBLIC cxx_std_20)

add_library(chatbox_gui_frontend STATIC
  "gui_app.cpp"
  "gui_app.h"
)
target_link_libraries(chatbox_gui_frontend PUBLIC chatbox_core)
target_link_libraries(chatbox_gui_frontend PRIVATE fltk)
target_compile_features(chatbox_gui_frontend PUBLIC cxx_std_20)

# =====================================================
# chatbox  (app launcher + selectable frontend)
# =====================================================
add_executable(chatbox "chatbox.cpp")

target_link_libraries(chatbox
  PRIVATE
    chatbox_core
    chatbox_curses_frontend
    chatbox_gui_frontend
)
target_compile_features(chatbox PRIVATE cxx_std_20)
