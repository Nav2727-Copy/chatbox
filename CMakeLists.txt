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
find_package(unofficial-pdcurses CONFIG REQUIRED)
find_package(miniupnpc CONFIG REQUIRED)

# =====================================================
# chatbox  (full client + embedded host, uses ncurses)
# =====================================================
add_executable(chatbox "chatbox.cpp")

target_link_libraries(chatbox PRIVATE Boost::asio)
target_link_libraries(chatbox PRIVATE unofficial::pdcurses::pdcurses)
target_link_libraries(chatbox PRIVATE miniupnpc::miniupnpc)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET chatbox PROPERTY CXX_STANDARD 20)
endif()

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET chatbox_server PROPERTY CXX_STANDARD 20)
endif()