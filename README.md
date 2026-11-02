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
- Ninja on Windows; CMake's Unix Makefiles generator is used on Linux and macOS

Project dependencies are declared in `vcpkg.json`: Boost.Asio, miniupnpc, libsodium, OpenSSL, SQLite, and curses (PDCurses on Windows, ncurses elsewhere).

## Build

Set `VCPKG_ROOT` to your vcpkg checkout, then run the build script with the preset for your platform:

```sh
export VCPKG_ROOT="$HOME/vcpkg"  # adjust if vcpkg is installed elsewhere
./build.sh linux-debug
```

Available presets are `linux-debug`, `linux-release`, `macos-debug`, `x64-debug`, `x64-release`, `x86-debug`, and `x86-release`. Windows presets expect a Visual Studio developer environment. The script writes build output to `out/build/<preset>`.

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

Set an optional password and database path:

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
