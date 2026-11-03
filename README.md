# Chatbox

Chatbox is a terminal-based chat application written in C++20. It includes an interactive curses interface, a standalone server mode, chat rooms, and an optional server directory for publishing and finding servers.

## Features

- Join or host a chat server from the interactive terminal UI.
- Create and switch between rooms, set room topics, and whisper to users.
- Run a dedicated server with password protection, logging, and administrative commands.
- Use TLS for chat server connections and optionally publish servers to a directory.
- Store server data in SQLite, with optional import of legacy bans and identities.
- Automatically request UPnP port forwarding when enabled.

## Requirements

- CMake 3.16 or newer
- A C++20 compiler
- [vcpkg](https://github.com/microsoft/vcpkg)
- Ninja on Windows; Make on Linux and macOS
- On Windows, a Visual Studio developer environment with the C++ workload

Project dependencies are declared in `vcpkg.json`: Boost.Asio, miniupnpc, libsodium, OpenSSL, SQLite, and curses (PDCurses on Windows, ncurses elsewhere).

## Build by platform

Install CMake, the platform's C++ build tools, and vcpkg. Bootstrap vcpkg if needed, then set `VCPKG_ROOT` to its checkout (the scripts default to `$HOME/vcpkg` if present):

```sh
export VCPKG_ROOT="$HOME/vcpkg" # adjust if vcpkg is installed elsewhere
```

The scripts build natively for the selected architecture. Linux executables and their vcpkg dependencies are statically linked. Windows uses the static vcpkg triplets and static MSVC runtime. The macOS ARM64 preset uses static vcpkg dependencies; macOS still requires dynamic system libraries. The native macOS preset follows the host triplet. vcpkg downloads and builds the dependencies on the first run, so the initial build can take a while.

### Linux x64 and aarch64

Install GCC, Make, CMake, ncurses development headers, and the Autotools packages needed to build the vcpkg dependencies. On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake libncurses-dev autoconf autoconf-archive automake libtool
```

Run on an x86_64 Linux machine for x64, or an aarch64 Linux machine for aarch64:

```sh
./build-linux.sh x64 debug
./build-linux.sh x64 release
./build-linux.sh aarch64 debug
./build-linux.sh aarch64 release
```

### macOS ARM64 and native architecture

Install Xcode Command Line Tools (`xcode-select --install`), CMake, and Make. On Apple Silicon, build ARM64 with:

```sh
./build-macos.sh arm64 debug
```

For a native build on either Apple Silicon or Intel macOS, use `./build-macos.sh native debug`. The current macOS script provides Debug presets.

### Windows x64 and x86

Install Visual Studio with the Desktop development with C++ workload, CMake, and Ninja. Open a Visual Studio Developer PowerShell, set `VCPKG_ROOT` if vcpkg is not in `$HOME\vcpkg`, then run:

```powershell
.\build-windows.ps1 x64 debug
.\build-windows.ps1 x64 release
.\build-windows.ps1 x86 debug
.\build-windows.ps1 x86 release
```

### Cross-compiling

These scripts and presets do not configure cross-compilation. In particular, the Linux aarch64 preset selects the ARM64 vcpkg triplet but also expects to run ARM64 GCC on an aarch64 Linux host. To build Linux aarch64 from x86_64 Linux, add and configure an aarch64 cross-compiler, target sysroot, and vcpkg cross-compilation triplet/chainload toolchain. The macOS ARM64 preset uses Apple's Clang and SDK and should be built on macOS; the current project does not provide a Linux-to-macOS toolchain. Windows builds also use Visual Studio's compiler and are intended to run from Windows.

All scripts write build output to `out/build/<preset>`.

For direct CMake use, run `cmake --preset <preset>` followed by `cmake --build out/build/<preset> --parallel`. Presets are `linux-debug`, `linux-release`, `linux-arm64-debug`, `linux-arm64-release`, `macos-debug`, `macos-arm64-debug`, `x64-debug`, `x64-release`, `x86-debug`, and `x86-release` (macOS and Windows presets are available only on those host systems).

To build directly with CMake, configure with the desired preset and build its directory:

```sh
cmake --preset linux-debug
cmake --build out/build/linux-debug --parallel
```

## Run

Launch the interactive interface (the default):

```sh
./out/build/linux-debug/chatbox
```

The startup menu lets you start chatting, configure and run a server, or start a server directory. In chat, type `/help` for available commands. Common commands include `/rooms`, `/create <room>`, `/join <room>`, `/leave`, `/topic`, `/users`, `/whisper <nickname> <message>`, and `/exit`.

### Dedicated server

Start a server on port `7777`:

```sh
./out/build/linux-debug/chatbox --server 7777
```

TLS is enabled by default. Supply a PEM certificate and private key:

```sh
./out/build/linux-debug/chatbox --server 7777 \
  --tls-cert server-cert.pem --tls-key server-key.pem
```

Set an optional password and database path. By default, Chatbox stores its SQLite database beside its identity keys in the per-user data directory (`$XDG_DATA_HOME/chatbox` on Linux/macOS, or `%APPDATA%\\chatbox` on Windows). On first use, an existing `chatbox.db` in the working directory is moved there. `--database` still accepts an explicit path.

```sh
./out/build/linux-debug/chatbox --server 7777 \
  --password 'choose-a-password' --database chatbox.db \
  --tls-cert server-cert.pem --tls-key server-key.pem
```

To explicitly run without encryption, use `--allow-plaintext`. This is intended only for networks where plaintext transport is acceptable. Add `--no-upnp` to disable automatic port forwarding. Type `/help` in the server console to see administrative commands.

Run `chatbox --help` for the full list of server options. The server browser can be started with `chatbox --browser` (TLS requires `--tls-cert` and `--tls-key`); list published servers with `chatbox --browse <browser-host>`.

## Tests

Tests are enabled by default when building with CMake. Run them from the build directory:

```sh
ctest --test-dir out/build/linux-debug --output-on-failure
```

## License

This project is licensed under the [GNU General Public License version 3](https://www.gnu.org/licenses/gpl-3.0.html).
