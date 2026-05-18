# chatbox

`chatbox.cpp` is a C++20 terminal chat application with a curses-style UI, TCP networking through Boost.Asio, and automatic UPnP port mapping through miniupnpc. It can run as an interactive chat client, an embedded host, or a dedicated server.

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Boost.Asio](https://img.shields.io/badge/networking-Boost.Asio-blue)
![PDCurses](https://img.shields.io/badge/UI-PDCurses-green)
![miniupnpc](https://img.shields.io/badge/UPnP-miniupnpc-orange)
![License: CC BY-NC-SA 4.0](https://img.shields.io/badge/License-CC--BY--NC--SA--4.0-green)

## Features

- Host or join a TCP chat room from the terminal
- Run a headless dedicated server from the menu or command line
- Optional room password on hosted and dedicated rooms
- Public-key nickname identity: first use registers a nickname key, future joins must prove the same private key
- Live user list in a split-pane terminal interface
- Public messages and labeled private-message commands
- Private messages are routed only to the sender and recipient
- New clients receive the most recent server-side chat history on join
- Basic server-side rate limiting for public and private messages
- Host and dedicated-server moderation commands: kick, ban, unban, and list bans
- Dedicated-server logging to `chatlog.txt` by default
- Dedicated-server logging can be disabled or sent to stdout only
- Dedicated-server ban persistence in `bans.txt`
- Optional UPnP port mapping with fallback LAN address display
- Base64 message framing for simple line-safe transport

## Dependencies

| Library | Purpose |
| --- | --- |
| [Boost.Asio](https://www.boost.org/doc/libs/release/doc/html/boost_asio.html) | TCP client/server networking |
| [PDCurses](https://pdcurses.org/) | Terminal UI on Windows |
| [miniupnpc](https://miniupnp.tuxfamily.org/) | UPnP router discovery and port mapping |
| [libsodium](https://libsodium.gitbook.io/doc/) | Public-key signatures for nickname identity |

The current CMake project is set up for vcpkg and PDCurses. The source uses a curses-style API, but the checked-in build configuration is Windows-focused.

## Building

Install vcpkg first if it is not already available:

```powershell
git clone https://github.com/microsoft/vcpkg "$env:USERPROFILE\vcpkg"
& "$env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat"
$env:VCPKG_ROOT = "$env:USERPROFILE\vcpkg"
```

Then configure and build with one of the checked-in presets:

```powershell
cmake --preset x64-debug
cmake --build out/build/x64-debug
```

The first CMake configure uses `vcpkg.json` to restore the required packages. The checked-in CMake file has a Windows default vcpkg path; if your checkout is somewhere else, pass the toolchain file explicitly:

```powershell
cmake -S . -B out/build/manual -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --build out/build/manual
```

The main target is `chatbox`, built from `chatbox.cpp`. The repository also includes a separate `chatbox_server` target built from `chatbox_server.cpp`.

## Running

Start the full terminal app:

```powershell
.\out\build\x64-debug\chatbox.exe
```

At startup:

| Key | Mode |
| --- | --- |
| `C` | Chat mode: choose whether to host or join |
| `D` | Dedicated server mode |
| `Q` | Quit |

In chat mode, enter a nickname first. Then:

| Key | Action |
| --- | --- |
| `H` | Host a room on a port and optionally set a room password |
| `J` | Join an existing host by address and port |
| `Q` | Quit before connecting |

When hosting, chatbox attempts UPnP port mapping, prints an external address when available, and also lists LAN addresses. If UPnP is unavailable, manually forward the selected port for internet clients.

In chat:

| Key | Action |
| --- | --- |
| `Enter` | Send the current message or command |
| `Backspace` | Delete one character |
| `Escape` | Leave the current chat session |

## Dedicated Server

You can start dedicated mode from the startup menu or directly from the command line:

```powershell
.\out\build\x64-debug\chatbox.exe --server <port> [password] [logfile]
.\out\build\x64-debug\chatbox.exe --dedicated <port> [password] [logfile]
.\out\build\x64-debug\chatbox.exe -s <port> [password] [logfile]
```

If no log file is supplied, the server writes to `chatlog.txt`. Bans are persisted in `bans.txt`.
Nickname identity bindings are persisted in `identities.txt`.

Dedicated-server options can be mixed with the positional form:

```powershell
.\out\build\x64-debug\chatbox.exe --server <port> --password <password> --log <file>
.\out\build\x64-debug\chatbox.exe --server <port> --identities identities.txt
.\out\build\x64-debug\chatbox.exe --server <port> --no-upnp --no-log
.\out\build\x64-debug\chatbox.exe --server <port> --log-stdout
```

Use `--identities <file>` to choose where the dedicated server stores nickname-to-public-key bindings.

Dedicated-server console commands:

| Command | Description |
| --- | --- |
| `/help` | Show dedicated-server help |
| `/users` | List connected users |
| `/kick <nick> [reason]` | Disconnect a user |
| `/ban <nick> [reason]` | Ban a nickname and persist it |
| `/unban <nick>` | Remove a persisted ban |
| `/bans` | List banned nicknames |
| `/broadcast <message>` | Send a server announcement |
| `/quit` or `/exit` | Shut down the server |

## Chat Commands

Commands available to connected chat users:

| Command | Description |
| --- | --- |
| `/help` | Show available commands |
| `/users` | List connected users |
| `/whisper <nick> <message>` | Send a private message |
| `/clear` | Clear the local message window |
| `/time` | Show the current local time |
| `/exit` | Leave the room |

Additional commands available when you are the interactive host:

| Command | Description |
| --- | --- |
| `/kick <nick> [reason]` | Disconnect a user |
| `/ban <nick> [reason]` | Ban a nickname for the current host session |
| `/unban <nick>` | Remove a nickname from the current host session ban list |
| `/bans` | Show the current host session ban list |

## Security Notes

Messages and protocol frames are Base64-encoded before being sent over TCP. Base64 is not encryption. Room passwords are also transported inside that same encoded protocol, so this is suitable for trusted LANs or casual testing, not sensitive communication over untrusted networks.

Nickname identity is based on libsodium Ed25519 signatures. On first use, the client creates a local signing key named `chatbox_identity_<nickname-hex>.key`; the server stores that nickname's public key in `identities.txt`. Later joins must answer a random server challenge with the matching private key before the nickname is accepted.

For real privacy, the networking layer would need authenticated encryption such as TLS or a libsodium-style key exchange and message encryption scheme.

## Project Structure

```text
chatbox/
|-- chatbox.cpp         # Full terminal app: client, embedded host, dedicated mode, UI, UPnP
|-- chatbox_server.cpp  # Separate headless server target
|-- CMakeLists.txt      # Build configuration
|-- CMakePresets.json   # Windows/Linux/macOS configure presets
|-- vcpkg.json          # Dependency manifest
`-- readme.md
```

## Known Limitations

- `/whisper` messages are routed privately by the server, but they are not encrypted end-to-end.
- Public-key identity proves control of a local key, but it does not encrypt messages.
- If a user's local identity key file is lost, the server will reject that nickname until the server identity binding is reset.
- Interactive-host bans are session-only; dedicated-server bans persist in `bans.txt`.
- Message history is in-memory only and resets when the server exits.
- The checked-in CMake setup is primarily configured for Windows with PDCurses.

## License

CC BY-NC-SA 4.0. See the header in `chatbox.cpp` for the current project notice.
