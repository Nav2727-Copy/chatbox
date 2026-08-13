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
  "protocol.cpp"
  "protocol.h"
)
target_include_directories(chatbox_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(chatbox_protocol PUBLIC cxx_std_20)

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
  "room.h"
  "dedicated_server.cpp"
  "dedicated_server.h"
  "server_browser.cpp"
  "server_browser.h"
  "storage.cpp"
  "storage.h"
  "tls.cpp"
  "tls.h"
  "transport.h"
  "upnp_mapper.h"
  "utils.cpp"
  "utils.h"
)

add_library(chatbox_core STATIC ${CHATBOX_CORE_SOURCES})
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
    ${CHATBOX_CURSES_TARGET}
)
target_compile_features(chatbox_curses_frontend PUBLIC cxx_std_20)

# =====================================================
# chatbox  (app launcher + terminal frontend)
# =====================================================
add_executable(chatbox "chatbox.cpp")

target_link_libraries(chatbox
  PRIVATE
    chatbox_core
    chatbox_curses_frontend
)
target_compile_features(chatbox PRIVATE cxx_std_20)

if (BUILD_TESTING)
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
