# Planned Additions

This document describes the planned addition of multiple chat rooms, SQLite persistence, TLS transport security, and cryptographic message signatures to `chatbox`.

The work should be delivered incrementally. Each milestone should leave the terminal application buildable and usable.

## Goals

- Allow one server to host multiple independently moderated rooms.
- Persist rooms, messages, bans, and identity bindings across restarts.
- Encrypt client/server and server-browser traffic with TLS.
- Authenticate user messages with the Ed25519 identity keys already used for nickname ownership.
- Preserve terminal, interactive-host, dedicated-server, and server-browser modes.
- Add protocol and integration tests for the new behavior.

## Non-goals for the first release

- End-to-end encryption of public room messages.
- Federation between independently operated chat servers.
- Multiple simultaneous room memberships on one connection.
- File transfer or image sharing.
- Full OpenPGP key management and web-of-trust support.

For the first release, a client has one active room at a time. This keeps the protocol and UI manageable while leaving room tabs and simultaneous memberships as a later enhancement.

## Recommended delivery order

| Phase | Addition | Estimated MVP effort |
| --- | --- | --- |
| 0 | Protocol foundation and tests | 3-5 days |
| 1 | Room domain model and commands | 1-3 weeks |
| 2 | SQLite persistence | 2-7 days |
| 3 | TLS transport | 3-7 days |
| 4 | Ed25519 message signatures | 3-7 days |
| 5 | UI polish, migration, and hardening | 1-2 weeks |

These estimates assume one developer familiar with modern C++ and Boost.Asio. Production hardening, packaging, and security review may extend the schedule.

## Phase 0: Protocol foundation

The current protocol uses Base64-encoded, pipe-delimited strings. Before adding more state, centralize protocol parsing and serialization so clients and servers do not construct frames independently.

### Planned work

- Add a protocol version handshake such as `HELLO|2`.
- Define typed client and server message structures.
- Put frame encoding, decoding, validation, and size limits in one module.
- Include stable message IDs and explicit room IDs where applicable.
- Reject unsupported versions and malformed frames with useful errors.
- Keep protocol version 1 support temporarily if backward compatibility is desired.
- Add a dedicated test target through CTest.

### Minimum tests

- Valid and malformed frame parsing.
- Oversized frames and chat messages.
- Password authentication success and failure.
- Identity challenge success and failure.
- Duplicate nicknames.
- Rate limiting.
- Kick, ban, and private-message routing.
- History delivery and reconnect behavior.

### Completion criteria

- Protocol frames are no longer assembled ad hoc throughout the application.
- Invalid input cannot terminate the client or server.
- Tests run locally through `ctest` and in CI on supported platforms.

### Implementation status

The initial Phase 0 slice was completed on 2026-08-12:

- Protocol v2 uses a required `HELLO|2` handshake and rejects unsupported versions.
- Client and server chat messages use typed structures with centralized serialization and validation.
- Chat and server-browser traffic share one strict Base64 frame codec and size limit.
- The `protocol` CTest covers round trips, malformed input, unsupported versions, and size limits.

Stable message and room IDs, broader integration coverage, backward compatibility, and CI remain open Phase 0 work.

## Phase 1: Multiple rooms

Introduce a `Room` domain object and make `ChatServer` responsible for managing rooms instead of treating the entire server as one global room.

### Suggested model

```cpp
struct Room {
    RoomId id;
    std::string name;
    std::string topic;
    std::deque<ChatMessage> recent_messages;
    std::set<std::shared_ptr<ClientSession>> members;
};
```

Each session records its active room. Broadcasts, user lists, history, moderation, and rate limits must use the appropriate room context.

### Initial commands

| Command | Purpose |
| --- | --- |
| `/rooms` | List available rooms |
| `/create <name>` | Create a room when permitted |
| `/join <name>` | Move to another room |
| `/leave` | Return to the default room |
| `/topic [text]` | View or change the current topic |

Existing `/users`, `/whisper`, `/kick`, `/ban`, and `/bans` behavior must be explicitly scoped. The initial implementation should make user lists and kicks room-local, while server-wide bans remain global.

### UI work

- Display the active room and topic.
- Add room selection to the curses frontend.
- Clear or replace the visible transcript when switching rooms.
- Display join, leave, and error events consistently.

### Completion criteria

- A server can host several named rooms.
- Users can list and switch rooms without reconnecting.
- Messages and user lists never leak between rooms.
- Room names, topics, and permissions are validated server-side.

## Phase 2: SQLite persistence

Replace in-memory-only history and the flat identity and ban files with a versioned SQLite database.

### Proposed schema

```sql
CREATE TABLE schema_version (
    version INTEGER NOT NULL
);

CREATE TABLE rooms (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    topic TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL
);

CREATE TABLE identities (
    nickname TEXT PRIMARY KEY,
    public_key TEXT NOT NULL,
    created_at INTEGER NOT NULL
);

CREATE TABLE bans (
    nickname TEXT PRIMARY KEY,
    reason TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    expires_at INTEGER
);

CREATE TABLE messages (
    id TEXT PRIMARY KEY,
    room_id INTEGER NOT NULL REFERENCES rooms(id),
    nickname TEXT NOT NULL,
    body TEXT NOT NULL,
    sent_at INTEGER NOT NULL,
    signature TEXT,
    public_key TEXT
);
```

The final schema may add indexes, moderation metadata, or separate server-event records.

### Implementation requirements

- Add SQLite through the vcpkg manifest.
- Encapsulate all SQL in a storage interface rather than calling SQLite from UI or networking code.
- Use prepared statements, transactions, foreign keys, and schema migrations.
- Enable WAL mode where appropriate.
- Avoid blocking the Asio networking thread; use a database worker queue or bounded batched writes.
- Retain a configurable in-memory history limit for delivery to newly joined clients.
- Add `--database <path>` to dedicated-server configuration.
- Provide a one-time import path for existing `bans.txt` and `identities.txt` files.

### Completion criteria

- Rooms, history, bans, and identities survive a clean server restart.
- Failed or interrupted writes do not corrupt existing data.
- History queries are bounded and paginated.
- Database schema upgrades are tested.

## Phase 3: TLS transport

TLS protects passwords, messages, identity proofs, and room metadata while they travel between a client and server. Base64 framing is not encryption and may remain only as a wire-format detail until the versioned protocol replaces it.

### Planned work

- Add OpenSSL to the build and use Boost.Asio SSL streams.
- Perform a TLS handshake before chat authentication or browser requests.
- Add dedicated-server options for certificate and private-key paths.
- Support normal CA validation for publicly trusted certificates.
- Support trust-on-first-use certificate fingerprints for self-hosted servers.
- Store trusted fingerprints per server address and warn clearly when one changes.
- Display actionable errors for expired, invalid, or mismatched certificates.
- Add TLS to server-browser traffic, or explicitly mark an initial browser connection as insecure.
- Do not silently downgrade from TLS to plaintext.

### Suggested options

```text
--tls-cert <path>
--tls-key <path>
--tls-ca <path>
--require-tls
--trust-fingerprint <sha256>
```

Private keys must never be logged or stored in the SQLite database. Password-protecting server key files may be considered after the basic TLS workflow is reliable.

### Completion criteria

- Packet capture does not reveal passwords, messages, or identity proofs.
- Clients verify the server certificate or a previously trusted fingerprint.
- Certificate changes produce a visible blocking warning.
- Plaintext compatibility, if retained, requires an explicit option.

## Phase 4: Signed messages

`chatbox` already uses Ed25519 keys to prove nickname ownership. Reuse those identities to sign messages instead of introducing OpenPGP in the initial implementation.

### Canonical signed envelope

A signature must cover an unambiguous, versioned representation of at least:

```text
protocol version
message ID
room ID
sender nickname
server identifier
client timestamp
message body
```

The exact byte representation must be documented and covered by test vectors. Concatenating fields without lengths or canonical encoding is not sufficient.

### Planned flow

1. The client creates a cryptographically random message ID.
2. The client builds the canonical message envelope.
3. The client signs the envelope with its existing Ed25519 private key.
4. The server verifies the signature against the nickname's registered public key.
5. The server rejects invalid signatures and duplicate message IDs.
6. The verified envelope and signature are stored unchanged in SQLite.
7. Receiving clients can verify the signature and display its status.

Server announcements and moderation events should use a separate event type and may later be signed by a server identity key.

### Security requirements

- Reject replayed message IDs.
- Apply reasonable timestamp skew limits without trusting the timestamp as proof of delivery time.
- Store the server-received timestamp separately from the signed client timestamp.
- Never expose private identity keys through logs, protocol errors, or database records.
- Preserve the original signature when replaying history.
- Define key-loss and identity-reset procedures.

### Completion criteria

- Modified messages fail verification.
- A signature cannot be moved to another room, server, sender, or message ID.
- Historical signatures remain verifiable after a server restart.
- The terminal frontend distinguishes verified, invalid, and unsigned messages.

## Optional: Actual OpenPGP support

Full OpenPGP should only be added if interoperability with users' existing PGP identities is a project requirement. It would require a maintained OpenPGP implementation plus UI and policy for:

- Key generation, import, export, and passphrases.
- Subkeys, expiration, and revocation.
- Trust decisions and fingerprint verification.
- Public-key discovery and changes.
- Cross-platform packaging of the selected library.

OpenPGP does not replace TLS: it can authenticate message authorship, but TLS is still needed to protect passwords, metadata, user lists, and other protocol traffic. Ed25519 message signatures provide the desired authorship guarantees with substantially less complexity and reuse the project's existing identity system.

## Phase 5: Migration and hardening

- Add CI builds and tests for Linux, Windows, and macOS where supported.
- Test upgrades from existing identity and ban files.
- Document database backup and recovery.
- Add structured security-relevant server logging without message or credential leakage.
- Fuzz protocol and database input boundaries.
- Test abrupt disconnects, database failures, slow clients, and certificate changes.
- Update the README and command help.
- Document the threat model and remaining limitations.

## Future enhancements

After the initial milestones are stable:

- Room tabs and simultaneous memberships.
- Per-room roles such as owner, moderator, member, and read-only guest.
- Private or invite-only rooms.
- Paginated history search.
- Automatic reconnect and unread counters.
- Expiring bans and audit logs.
- Signed server announcements.
- End-to-end encrypted whispers.
- Encrypted database backups.

## Definition of done

The combined project is considered complete when:

- All four additions work from the curses frontend.
- Dedicated servers can configure database and TLS paths without source changes.
- The server hosts multiple isolated rooms with persistent history.
- Clients refuse invalid TLS identities under the configured trust policy.
- User messages are signed and verifiable with existing Ed25519 identities.
- Automated tests cover protocol parsing, room isolation, persistence, TLS validation, signature verification, and migration.
- Security limitations and operational procedures are documented.
