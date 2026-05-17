# chatbox

A terminal-based P2P chat application written in C++. One peer hosts a server, others join directly — no central server required. Supports multiple simultaneous clients, private messages, a live user list, and automatic port forwarding via UPnP.

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![License: CC BY-NC-SA 4.0](https://img.shields.io/badge/License-CC--BY--NC--SA--4-green
)

---

## Features

- **Host or join** — one peer hosts, any number of others connect directly
- **Automatic port forwarding** — UPnP opens the port on your router and displays your external IP automatically
- **Live user list** — sidebar panel updates in real time as people join and leave
- **Private messages** — `/whisper <nick> <message>` for direct messages
- **Timestamps** — every message is timestamped `[HH:MM:SS]`
- **Terminal UI** — split-pane ncurses interface (chat, users, input)
- **Cross-platform** — Linux, macOS, and Windows (with PDCurses)

---

## Dependencies

| Library | Purpose |
|---|---|
| [Boost.Asio](https://www.boost.org/doc/libs/release/libs/asio/) | Async TCP networking |
| [ncurses](https://invisible-island.net/ncurses/) / PDCurses (Windows) | Terminal UI |
| [miniupnpc](https://miniupnp.tuxfamily.org/) | UPnP automatic port mapping |

---

## Building

### 1. Install vcpkg (if you haven't)

```bash
git clone https://github.com/microsoft/vcpkg
cd vcpkg && ./bootstrap-vcpkg.sh   # or bootstrap-vcpkg.bat on Windows
export VCPKG_ROOT=$(pwd)
```

### 2. Configure and build

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

vcpkg will automatically install all dependencies from `vcpkg.json` on the first run.

### CMakePresets (optional)

If you add a `CMakePresets.json`, you can shorten this to:

```bash
cmake --preset default
cmake --build build
```

---

## Usage

Run the binary:

```bash
./build/chatbox
```

You'll be prompted for a nickname, then:

- Press **H** to host — enter a port number. UPnP will attempt to open it on your router and print your external IP:port to share with your peer.
- Press **J** to join — enter the host's IP and port.

### In chat

| Key | Action |
|---|---|
| `Enter` | Send message |
| `Backspace` | Delete character |
| `Escape` | Quit |

### Commands

| Command | Description |
|---|---|
| `/help` | Show all commands |
| `/users` | List connected users |
| `/whisper <nick> <msg>` | Send a private message (client only) |
| `/clear` | Clear message history |
| `/time` | Show current time |
| `/exit` | Quit |

---

## UPnP / Port Forwarding

When hosting, chatbox will:

1. Discover your router via UPnP
2. Open the chosen port (TCP + UDP) automatically
3. Print your external IP:port in the chat log — share this with whoever is connecting

If UPnP is unavailable or disabled on your router, the app will tell you and fall back to showing your LAN addresses. In that case you'll need to forward the port manually in your router settings.

> **Note:** UPnP only works on your local network — it talks to your router, not the internet. If your router doesn't support it, you'll see a "UPnP unavailable" message. Most home routers have it enabled by default.

---

## Security

Messages are currently encoded with Base64, which is **not encryption** — anyone who can intercept the TCP stream can read the traffic trivially. This is fine for LAN use between trusted peers, but should not be used for anything sensitive over the internet.

A future improvement would be to wrap the connection in TLS using `boost::asio::ssl` with a self-signed certificate pinned on both ends.

---

## Project Structure

```
chatbox/
├── chatbox.cpp      # Everything — UPnP, server, client, UI, commands
├── vcpkg.json       # Dependency manifest
├── CMakeLists.txt   # Build configuration
└── README.md
```

---

## Known Limitations

- Whisper messages are broadcast to all connected clients by the server (no true end-to-end routing yet)
- No message history persistence — chat log is lost on exit
- No encryption
- Only one active connection per client instance
