# chatbox

`chatbox` is a C++20 terminal chat application with TLS networking through Boost.Asio, SQLite persistence, and automatic UPnP port mapping through miniupnpc. It can run as an interactive chat client, an embedded host, a dedicated server, or a lightweight server browser.

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Boost.Asio](https://img.shields.io/badge/networking-Boost.Asio-blue)
![OpenSSL](https://img.shields.io/badge/transport-TLS-green)
![SQLite](https://img.shields.io/badge/storage-SQLite-blue)
![PDCurses](https://img.shields.io/badge/UI-PDCurses-green)
![miniupnpc](https://img.shields.io/badge/UPnP-miniupnpc-orange)
![License: CC BY-NC-SA 4.0](https://img.shields.io/badge/License-CC--BY--NC--SA--4.0-green)

The protocol foundation, multi-room support, SQLite persistence, and TLS transport milestones described in [`PLANNED_ADDITIONS.md`](PLANNED_ADDITIONS.md) are implemented. Signed chat-message envelopes remain planned work.

## Features

- Host or join a multi-room TLS chat server from the terminal UI
- Create and switch rooms without reconnecting; each room has its own topic, users, recent history, moderation, and rate-limit window
- Run a headless dedicated server from the menu or command line
- Run a headless server browser that published servers can register with
- Publish hosted or dedicated servers for easier discovery
- Browse published servers and join one from the terminal UI
- Optional server password on hosted and dedicated servers
- TLS 1.2+ for chat and server-browser traffic, with CA validation, certificate pins, and trust on first use for self-hosted servers
- SQLite persistence for rooms, topics, bounded public-room history, bans, and nickname identities
- Public-key nickname identity: first use registers a nickname key, future joins must prove the same private key
- Live room and room-local user lists in the terminal split-pane interface
- Public messages and labeled private-message commands
- Private messages are routed only to the sender and recipient
- Clients receive the destination room's recent history when joining or switching rooms
- Basic server-side rate limiting for public and private messages
- Room-local kicks and topics, with server-wide ban, unban, and ban-list commands
- Dedicated-server logging to `chatlog.txt` by default
- Dedicated-server logging can be disabled or sent to stdout only
- Versioned database migrations, WAL mode, batched background writes, and bounded cursor-based history queries
- Optional UPnP port mapping with fallback LAN address display
- Base64 message framing for simple line-safe transport
- Versioned protocol v3 handshake with typed, validated, room-aware messages

## Dependencies

| Library | Purpose |
| --- | --- |
| [Boost.Asio](https://www.boost.org/doc/libs/release/doc/html/boost_asio.html) | TCP client/server networking |
| [PDCurses](https://pdcurses.org/) / [ncurses](https://invisible-island.net/ncurses/) | Terminal UI on Windows / Unix |
| [miniupnpc](https://miniupnp.tuxfamily.org/) | UPnP router discovery and port mapping |
| [libsodium](https://libsodium.gitbook.io/doc/) | Public-key signatures for nickname identity |
| [OpenSSL](https://www.openssl.org/) | TLS transport and certificate validation |
| [SQLite](https://www.sqlite.org/) | Persistent rooms, messages, bans, and identities |

The CMake project uses PDCurses on Windows and ncurses on Linux and macOS. Dependencies are restored through the checked-in vcpkg manifest.

## Building

Install vcpkg first if it is not already available.

On Debian or Ubuntu, install the compiler and development tools used by vcpkg first:

```bash
sudo apt-get install build-essential cmake curl git pkg-config zip unzip \
  autoconf autoconf-archive automake libtool
```

Then install vcpkg and build:

```bash
git clone https://github.com/microsoft/vcpkg "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh"
export VCPKG_ROOT="$HOME/vcpkg"

cmake --preset linux-debug
cmake --build out/build/linux-debug -j
```

On Windows:

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

The first CMake configure uses `vcpkg.json` to restore the required packages. For a manual build, pass the toolchain file explicitly:

```powershell
cmake -S . -B out/build/manual -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --build out/build/manual
```

The main target is `chatbox`. It links a shared core library plus the curses frontend.

Run the complete protocol, room, persistence, and TLS test suite after building with:

```bash
ctest --test-dir out/build/linux-debug --output-on-failure
```

On Windows, replace the test directory with the preset you built, such as `out/build/x64-debug`.

## Secure Quick Start

Chat and server-browser servers require TLS by default. For a public deployment, use a certificate issued for the hostname clients will enter. For local testing, you can create a self-signed certificate with a matching subject alternative name:

```bash
openssl req -x509 -newkey rsa:3072 -sha256 -days 365 -nodes \
  -keyout server.key -out server.crt \
  -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1"
chmod 600 server.key
```

Start a dedicated server with persistent state:

```bash
./out/build/linux-debug/chatbox --server 7777 \
  --database chatbox.db \
  --tls-cert server.crt \
  --tls-key server.key
```

Start the terminal UI in another shell, choose chat mode, enter a nickname, and join `localhost` on port `7777`. A self-signed certificate is enrolled in `trusted_fingerprints.txt` on first use. Compare the SHA-256 fingerprint shown by the server before relying on that trust decision.

For local compatibility testing only, both sides must explicitly select plaintext: start the server with `--allow-plaintext` and answer no when the terminal client asks whether to use TLS. Chatbox never silently downgrades a failed TLS connection.

## Running

Start the default terminal app:

```powershell
.\out\build\x64-debug\chatbox.exe
```

You can also choose the terminal frontend explicitly:

```powershell
.\out\build\x64-debug\chatbox.exe --curses
```

At startup:

| Key | Mode |
| --- | --- |
| `C` | Chat mode: choose whether to host or join |
| `D` | Dedicated server mode |
| `B` | Browser server mode |
| `Q` | Quit |

In chat mode, enter a nickname first. Then:

| Key | Action |
| --- | --- |
| `H` | Host a server with optional password, TLS, persistence, UPnP, and browser publication |
| `J` | Join an existing host by address and port, using TLS by default |
| `B` | Query a TLS server browser and choose a published server |
| `Q` | Quit before connecting |

When hosting, chatbox can require a certificate and private key, persists server state in `chatbox.db`, attempts UPnP port mapping when enabled, and lists external and LAN addresses. If UPnP is unavailable, manually forward the selected port for internet clients.

Hosted servers can optionally publish themselves to a browser server on fixed port `2727`. When prompted, enter the browser server address, listing name, and public address clients should use. If the public address is blank, chatbox uses the UPnP external address when available, then a LAN address, then `127.0.0.1`.

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

If no log file is supplied, the server writes to `chatlog.txt`. Server state is persisted in `chatbox.db` by default. When legacy `bans.txt` or `identities.txt` files are present and have not already been imported, their valid entries are copied into SQLite.

Dedicated-server options can be mixed with the positional form:

```powershell
.\out\build\x64-debug\chatbox.exe --server <port> --tls-cert server.crt --tls-key server.key
.\out\build\x64-debug\chatbox.exe --server <port> --password <password> --database server.db --history-limit 100 --tls-cert server.crt --tls-key server.key
.\out\build\x64-debug\chatbox.exe --server <port> --import-bans bans.txt --import-identities identities.txt --tls-cert server.crt --tls-key server.key
.\out\build\x64-debug\chatbox.exe --server <port> --allow-plaintext --no-upnp --no-log
.\out\build\x64-debug\chatbox.exe --server <port> --log-stdout --tls-cert server.crt --tls-key server.key
.\out\build\x64-debug\chatbox.exe --server <port> --publish <browser-host> --name "Room name" --tls-cert server.crt --tls-key server.key
```

Important dedicated-server options:

| Option | Purpose |
| --- | --- |
| `--database <path>` | Select the SQLite database; defaults to `chatbox.db` |
| `--history-limit <1-500>` | Set the bounded in-memory history delivered on room entry |
| `--tls-cert <path>` | Load a PEM certificate chain |
| `--tls-key <path>` | Load the matching PEM private key |
| `--require-tls` | Require TLS; this is the default |
| `--allow-plaintext` | Explicitly enable insecure compatibility transport |
| `--import-bans <path>` | Import a legacy flat ban file once |
| `--import-identities <path>` | Import legacy `nickname\|public-key` bindings once |
| `--password <password>` | Require a server password after the TLS handshake |
| `--log <path>` | Select the chat log file |
| `--log-stdout` | Log to standard output instead of a file |
| `--no-log` | Disable chat logging |
| `--no-upnp` | Disable automatic router port mapping |

Dedicated servers require `--tls-cert <path>` and `--tls-key <path>` by default. `--allow-plaintext` is the explicit compatibility mode; clients must also explicitly choose plaintext, and there is no automatic downgrade. `--identities <file>` remains as a compatibility alias for `--import-identities`.

Use `--publish <host>` to register the dedicated server with a browser server on fixed port `2727`. Browser publication uses TLS by default and supports `--browser-tls-ca`, `--browser-trust-fingerprint`, and `--browser-trust-store`; `--browser-plaintext` is the explicit insecure mode. Use `--public-host <host>` when the browser should advertise a specific internet-facing address instead of the auto-detected UPnP or LAN address.

Dedicated-server console commands:

| Command | Description |
| --- | --- |
| `/help` | Show dedicated-server help |
| `/rooms` | List rooms and room-local user counts |
| `/create <room>` | Create a room |
| `/users <room>` | List users in one room |
| `/topic <room> <text>` | Set a room topic |
| `/kick <room> <nick> [reason]` | Disconnect a user found in that room |
| `/ban <nick> [reason]` | Ban a nickname server-wide and persist it |
| `/unban <nick>` | Remove a persisted ban |
| `/bans` | List banned nicknames |
| `/broadcast <message>` | Send a server announcement to every room |
| `/quit` or `/exit` | Shut down the server |

## Server Browser

A server browser is a lightweight rendezvous server. Chat hosts and dedicated servers publish their listing name, connect address, port, password status, TLS status, and user count to it. Clients query it for a list and then connect directly to the selected chat server.

Run a browser server:

```powershell
.\out\build\x64-debug\chatbox.exe --browser --tls-cert browser.crt --tls-key browser.key
```

List published servers from the command line:

```powershell
.\out\build\x64-debug\chatbox.exe --browse <browser-host>
.\out\build\x64-debug\chatbox.exe --browse <browser-host> --tls-ca private-ca.crt
.\out\build\x64-debug\chatbox.exe --browse <browser-host> --trust-fingerprint <sha256>
.\out\build\x64-debug\chatbox.exe --browse <browser-host> --trust-store browser-fingerprints.txt
.\out\build\x64-debug\chatbox.exe --browse <browser-host> --plaintext
```

The browser server also requires TLS by default. Start it with `--allow-plaintext` only when intentionally operating an insecure compatibility browser, and query that browser with `--plaintext`. Self-hosted TLS certificates are saved by server address in `trusted_fingerprints.txt`; a later certificate change is blocked until the stored trust decision is deliberately replaced.

Server browser console commands:

| Command | Description |
| --- | --- |
| `/help` | Show browser help |
| `/servers` | List currently published, non-expired rooms |
| `/quit` or `/exit` | Shut down the browser server |

Published servers refresh their listing once per minute and unregister on clean shutdown. Browser entries expire after three minutes if a server stops refreshing.

## Chat Commands

Commands available to connected chat users:

| Command | Description |
| --- | --- |
| `/help` | Show available commands |
| `/rooms` | List available rooms |
| `/create <name>` | Create and enter a room; names use letters, numbers, `-`, or `_` |
| `/join <name>` | Switch to an existing room without reconnecting |
| `/leave` | Return to the default `lobby` |
| `/topic [text]` | View the active topic, or change it as the room creator/host |
| `/users` | List users in the active room |
| `/whisper <nick> <message>` | Send a private message to someone in the active room |
| `/kick <nick> [reason]` | Kick an active-room member as the room creator/host |
| `/clear` | Clear the local message window |
| `/time` | Show the current local time |
| `/exit` | Disconnect from the server |

Additional commands available when you are the interactive host:

| Command | Description |
| --- | --- |
| `/ban <nick> [reason]` | Ban a nickname server-wide and persist it in SQLite |
| `/unban <nick>` | Remove a persisted server-wide ban |
| `/bans` | Show the server-wide ban list |

## Persistence

The SQLite database owns the durable server state:

- room names, topics, owners, and creation times;
- public room messages and their stable identifiers;
- server-wide bans, reasons, and optional expiration fields;
- nickname-to-Ed25519-public-key bindings; and
- the schema version and completed legacy imports.

Each room also keeps a configurable recent-message window in memory for fast delivery when a user enters. History reads are limited to 500 records per page and use stable `(sent_at, id)` cursors. Writes are queued away from the Asio networking thread, grouped into transactions, and flushed during clean server shutdown. SQLite foreign keys and WAL mode are enabled when the database opens.

Legacy imports are idempotent: a successfully opened `bans.txt` or `identities.txt` is imported at most once per database. The source files are not deleted. Private whispers and local log files are not stored in the message-history table.

## Security Notes

TLS handshakes complete before chat authentication or browser requests, so passwords, identity proofs, messages, and room metadata are not exposed as Base64 plaintext on the network. Base64 remains only the line-safe application framing inside the authenticated TLS connection. TLS 1.0 and 1.1 are disabled.

Publicly trusted certificates use normal CA and hostname validation. Command-line browser clients and publishers can also supply a private CA or explicit SHA-256 certificate pin. The terminal chat client uses trust on first use for self-hosted certificates. TOFU protects later connections, but the first connection is only as trustworthy as the network used for enrollment; compare the displayed server fingerprint out of band when possible. A changed stored certificate is a blocking error, not a fallback to plaintext.

Private keys are read only from the configured PEM file. They are never copied into SQLite or written to application logs. Keep server key files outside shared directories and restrict their filesystem permissions.

Nickname identity is based on libsodium Ed25519 signatures. On first use, the client creates a local signing key named `chatbox_identity_<nickname-hex>.key`; the server stores that nickname's public key in SQLite. Later joins must answer a random server challenge with the matching private key before the nickname is accepted. Public room messages are protected in transit but are not end-to-end encrypted from the server.

## Project Structure

```text
chatbox/
|-- chatbox.cpp          # App launcher and mode selection
|-- app_state.*          # Shared message/user state
|-- chat_client.h        # TLS/plaintext chat client
|-- chat_server.h        # Multi-room chat server and sessions
|-- commands.*           # Shared chat command parser/handler
|-- protocol.*           # Versioned messages, validation, and wire framing
|-- interactive_app.cpp  # Curses frontend flow
|-- curses_ui.*          # Curses drawing and prompts
|-- dedicated_server.*   # Headless server and browser-server modes
|-- server_browser.*     # Published-room browser protocol
|-- storage.*            # SQLite interface, migrations, and worker queue
|-- tls.*                # Certificate validation, fingerprints, and TOFU
|-- transport.h          # Shared plaintext/TLS stream adapter
|-- tests/               # CTest test sources
|-- CMakeLists.txt       # Build configuration
|-- CMakePresets.json    # Windows/Linux/macOS configure presets
|-- vcpkg.json           # Dependency manifest
`-- readme.md
```

## Known Limitations

- `/whisper` messages are private from other users, but the server can read them; there is no end-to-end encryption.
- Public-key nickname identity proves control of a local key, but chat-message signatures are not implemented yet.
- If a user's local identity key file is lost, the server will reject that nickname until the server identity binding is reset.
- TOFU verifies certificate continuity, not ownership of a hostname; use CA validation or an independently verified pin when stronger authentication is required.
- Losing a self-hosted server private key requires distributing and deliberately trusting a new certificate fingerprint.
- TLS protects browser traffic, but published entries remain self-reported and are not health-checked beyond their refresh timeout.
- The checked-in CMake setup uses PDCurses on Windows and ncurses on Linux and macOS.

## License

CC BY-NC-SA 4.0. See the header in `chatbox.cpp` for the current project notice.
