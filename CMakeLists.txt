# CMakeList.txt : CMake project for chatbox
cmake_minimum_required (VERSION 3.16)

if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()
if (POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

project ("chatbox")

include(CTest)

if (WIN32)
  add_definitions(-D_WIN32_WINNT=0x0601)
endif()

# ---- Packages ----
find_package(Boost REQUIRED COMPONENTS asio)
find_package(miniupnpc CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

if (WIN32)
  find_package(unofficial-pdcurses CONFIG REQUIRED)
  set(CHATBOX_CURSES_TARGET unofficial::pdcurses::pdcurses)
else()
  set(CURSES_NEED_NCURSES TRUE)
  set(CURSES_NEED_WIDE TRUE)
  find_package(Curses REQUIRED)
  if (NOT TARGET Curses::Curses)
    add_library(Curses::Curses INTERFACE IMPORTED)
    set_target_properties(Curses::Curses PROPERTIES
      INTERFACE_LINK_LIBRARIES "${CURSES_LIBRARIES}"
      INTERFACE_INCLUDE_DIRECTORIES "${CURSES_INCLUDE_DIRS}"
      INTERFACE_COMPILE_OPTIONS "${CURSES_CFLAGS}"
    )
  endif()
  set(CHATBOX_CURSES_TARGET Curses::Curses)
endif()

add_library(chatbox_protocol STATIC
  "src/protocol/protocol.cpp"
  "src/protocol/protocol.h"
)
target_include_directories(chatbox_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(chatbox_protocol PUBLIC cxx_std_20)

set(CHATBOX_CORE_SOURCES
  "src/app/app_state.cpp"
  "src/app/app_state.h"
  "src/chat/chat_client.h"
  "src/storage/chat_log.h"
  "src/chat/chat_server.h"
  "src/chat/client_session.cpp"
  "src/app/commands.cpp"
  "src/app/commands.h"
  "src/common/common.h"
  "src/common/constants.h"
  "src/chat/room.h"
  "src/app/dedicated_server.cpp"
  "src/app/dedicated_server.h"
  "src/network/server_browser.cpp"
  "src/network/server_browser.h"
  "src/storage/preferences.cpp"
  "src/storage/storage.cpp"
  "src/storage/storage.h"
  "src/network/invite.cpp"
  "src/network/tls.cpp"
  "src/network/tls.h"
  "src/network/transport.h"
  "src/network/upnp_mapper.h"
  "src/common/utils.cpp"
  "src/common/utils.h"
)

add_library(chatbox_core STATIC ${CHATBOX_CORE_SOURCES})
target_include_directories(chatbox_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(chatbox_core
  PUBLIC
    chatbox_protocol
    Boost::asio
    miniupnpc::miniupnpc
    unofficial-sodium::sodium
    OpenSSL::SSL
    OpenSSL::Crypto
    SQLite::SQLite3
    Threads::Threads
)
target_compile_features(chatbox_core PUBLIC cxx_std_20)
if (WIN32)
  target_link_libraries(chatbox_core PUBLIC iphlpapi)
endif()

add_library(chatbox_terminal_model STATIC "src/ui/terminal_model.cpp")
target_include_directories(chatbox_terminal_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(chatbox_terminal_model PUBLIC cxx_std_20)

add_library(chatbox_curses_frontend STATIC
  "src/ui/curses_app.h"
  "src/ui/startup_menu.cpp"
  "src/ui/startup_menu.h"
  "src/ui/curses_ui.cpp"
  "src/ui/curses_ui.h"
  "src/ui/interactive_app.cpp"
  "src/ui/interactive_app.h"
)
target_link_libraries(chatbox_curses_frontend
  PUBLIC
    chatbox_core
    chatbox_terminal_model
    ${CHATBOX_CURSES_TARGET}  # Changed to PUBLIC so downstream binaries get the Curses layout
)
target_compile_features(chatbox_curses_frontend PUBLIC cxx_std_20)

# =====================================================
# chatbox  (app launcher + terminal frontend)
# =====================================================
add_executable(chatbox "src/app/chatbox.cpp")

target_link_libraries(chatbox
  PRIVATE
    chatbox_core
    chatbox_curses_frontend
)
target_compile_features(chatbox PRIVATE cxx_std_20)

if (BUILD_TESTING)
  add_executable(chatbox_invite_tests "tests/invite_tests.cpp")
  target_link_libraries(chatbox_invite_tests PRIVATE chatbox_core)
  add_test(NAME invites COMMAND chatbox_invite_tests)
  add_executable(chatbox_connection_tests "tests/connection_tests.cpp")
  target_link_libraries(chatbox_connection_tests PRIVATE chatbox_core)
  add_test(NAME connections COMMAND chatbox_connection_tests)
  set_tests_properties(connections PROPERTIES TIMEOUT 10)

  add_executable(chatbox_terminal_tests "tests/terminal_tests.cpp")
  target_link_libraries(chatbox_terminal_tests PRIVATE chatbox_terminal_model)
  add_test(NAME terminal COMMAND chatbox_terminal_tests)

  add_executable(chatbox_app_state_tests
    "tests/app_state_tests.cpp"
    "src/app/app_state.cpp"
  )
  target_link_libraries(chatbox_app_state_tests PRIVATE chatbox_protocol Threads::Threads)
  add_test(NAME app_state COMMAND chatbox_app_state_tests)

  add_executable(chatbox_protocol_tests "tests/protocol_tests.cpp")
  target_link_libraries(chatbox_protocol_tests PRIVATE chatbox_protocol)
  target_compile_features(chatbox_protocol_tests PRIVATE cxx_std_20)
  add_test(NAME protocol COMMAND chatbox_protocol_tests)

  add_executable(chatbox_room_integration_tests "tests/room_integration_tests.cpp")
  target_link_libraries(chatbox_room_integration_tests PRIVATE chatbox_core)
  target_compile_features(chatbox_room_integration_tests PRIVATE cxx_std_20)
  add_test(NAME rooms COMMAND chatbox_room_integration_tests)

  add_executable(chatbox_storage_tests "tests/storage_tests.cpp")
  target_link_libraries(chatbox_storage_tests PRIVATE chatbox_core)
  target_compile_features(chatbox_storage_tests PRIVATE cxx_std_20)
  add_test(NAME storage COMMAND chatbox_storage_tests)

  add_executable(chatbox_tls_tests "tests/tls_tests.cpp")
  target_link_libraries(chatbox_tls_tests PRIVATE chatbox_core)
  target_compile_features(chatbox_tls_tests PRIVATE cxx_std_20)
  add_test(NAME tls COMMAND chatbox_tls_tests)
endif()
