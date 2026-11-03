# UX and Usability Roadmap

This roadmap combines the original UX plan with the project assessment from November 2, 2026. It focuses on making Chatbox reliable and easy to use while keeping its terminal interface. Existing features are separated from remaining work so implemented improvements are not scheduled again.

## Current baseline

Chatbox has a working chat core suitable for technical-user testing. It supports hosting and joining, rooms and topics, room-scoped whispers, dedicated-server administration, passwords, TLS, directory publishing/discovery, UPnP, and SQLite persistence for rooms, messages, identities, and bans.

The following usability improvements are already implemented, though some need refinement:

- Startup and join/host/discovery menus with explanatory text and validated forms.
- Password masking and preservation of some join/host fields across failed attempts within a session.
- Friendly explanations for several connection failures and explicit authentication/nickname errors.
- Hosting messages for the listening port, LAN addresses, UPnP results, public address when available, and manual TCP forwarding.
- Empty-chat guidance, visible keyboard shortcuts, in-app help, command/room/nickname completion, and an F2 room picker that preserves the draft.
- Input editing, sent-message history, scrollback, responsive layouts, and a minimum-terminal-size notice at 40 × 12.

Assessment validation: the Linux debug build and all six test suites passed, and the startup menu launched and exited cleanly. This does not establish Windows/macOS behavior, real-world router compatibility, or production reliability.

## Implementation progress — November 3, 2026

The Linux implementation now covers the main user-facing work across Priorities 1–4. Build and unit/integration tests pass; manual cross-platform and router validation remain release tasks.

- Chat connections and directory requests run asynchronously with deadlines, visible progress, Escape cancellation, structured connection failures, and preserved setup values. Late DNS work stays isolated from the UI. UPnP discovery is cancellable from the UI and router mappings are removed on normal shutdown.
- Dropped chat connections preserve the unsent draft, retry up to three times, allow cancellation, and restore the selected room when possible. Messages with uncertain delivery are not resent. Password, ban, nickname, and certificate failures return to server selection.
- Interactive TLS uses a per-user trust store, asks before trusting an unknown certificate, blocks changed certificates until an explicit replacement review, supports optional CA/fingerprint settings, and migrates existing launch-directory trust entries. Dedicated host certificate generation/reuse and fingerprint display are available.
- Hosting presents LAN and advertised addresses, forwarding guidance, TLS/password explanations, and versioned invites. Invites are reviewable before joining and never contain passwords or private keys. The hosting summary is available with F3.
- Preferences can remember nickname, recent chat servers, directory/publishing settings, and host certificate paths. Recent servers can be renamed, removed, or joined. Nothing is saved until remembering is enabled.
- Help is available at startup and in chat; menu and form guidance can be expanded with F4. Notices scroll at small terminal sizes. Discovery explains empty and unreachable results.

Automated coverage includes invite parsing, connection cancellation/deadlines/disconnect, explicit trust decisions, certificate generation/reuse, and preferences. A Linux PTY smoke script covers invite joining, server restart/reconnect, draft retention, resize, and guided hosting. Windows/macOS builds, representative terminal testing on those platforms, internet reachability, and real-router UPnP behavior remain unverified. Native resolver teardown can outlast cancellation, though it does not block the interface. Review `tests/terminal_smoke.py` for the local end-to-end procedure.

## Priority 1: Make connections responsive and recoverable

### Cancellable connections and discovery
- **Implemented:** move DNS resolution, TCP connection, and TLS handshake work off the blocking UI path. Directory lookup and publication also run off the UI path.
- **Implemented:** add explicit phase deadlines and responsive progress, cancellation, and terminal resizing. Native resolver teardown can outlast cancellation.
- Directory lookup and UPnP discovery are cancellable.
- Clean up pending network work when cancelling or retrying so another attempt starts reliably.

### Actionable errors and retry
- Replace fragile error-string matching with structured failure categories where practical.
- Preserve distinct explanations for DNS failure, refusal, timeout, TLS failure, incorrect password, nickname conflict, and bans.
- Show a useful next step and allow users to view underlying diagnostic details, especially certificate errors.
- Preserve relevant form values across retry, including certificate paths and directory settings; allow nickname correction without restarting the app.
- Keep passwords masked and in memory only unless an explicit secure credential-storage feature is added.

### Disconnect and reconnect flow
- Show connection state clearly and explain why a session ended when known.
- Offer retry and return-to-server-selection actions after a disconnect; leaving a chat should allow returning to the menu.
- Preserve unsent drafts and prevent sending while disconnected.
- Add bounded automatic reconnect for transient failures, with visible progress and cancellation. Authentication failures, bans, and changed certificates require user action.
- Restore room selection where possible after reconnecting; do not automatically resend messages with uncertain delivery status.

**Acceptance:** an unreachable or stalled server cannot freeze the interface indefinitely; users can cancel, correct details, retry, and recover from a dropped connection without relaunching or losing their draft.

## Priority 2: Simplify secure hosting and certificate trust

### Guided TLS setup
- **Implemented:** offer guided generation/reuse of a self-hosted certificate and private key, alongside supplying existing files. Automatic renewal remains outstanding.
- Explain where these files are stored and validate missing, unreadable, or mismatched files before starting the server.
- Explain TLS transport protection, the separate purpose of a server password, and that TLS is not end-to-end encryption between chat participants.
- Label plaintext as insecure before it is enabled and explain its implications in both join and host forms.

### Explicit certificate decisions
- Replace automatic first-use trust in the interactive flow with a clear certificate verification and accept/reject step.
- Display the server address and SHA-256 fingerprint, with instructions for comparing it with the host through a trusted channel.
- Keep changed certificates blocked; provide a deliberate review/reset flow rather than silently replacing trust.
- Expose trusted CA and fingerprint settings where users need them, including directory connections.
- Store trust information in a stable per-user location rather than depending on the launch directory; preserve existing trust entries during migration.

### Refine hosting diagnostics
- Consolidate existing listening-address, TLS, password, UPnP, and forwarding messages into a readable hosting summary.
- Distinguish a bound local address, a discovered public address, and verified reachability; do not imply that successful UPnP guarantees internet access.
- Keep manual TCP forwarding instructions available when automatic forwarding fails.
- Explain which address to share on a LAN versus over the internet and keep diagnostics accessible after setup.

**Acceptance:** a new user can host an encrypted LAN conversation without creating certificate files manually, verify the host when joining, and understand what to do when internet access fails.

## Priority 3: Shorten first-run and repeat visits

### Guided first-run flow
- Build on the existing menus to make joining, hosting, and discovery direct choices with minimal repeated navigation.
- Explain required information before asking for it and use sensible defaults.
- Provide a brief optional walkthrough that can be revisited from help.
- Preserve the ability to skip guidance and reach advanced settings quickly.

### Recent servers and preferences
- **Implemented:** opt-in nickname and recent chat-server persistence. **Remaining:** directory addresses and advanced preferences across launches.
- **Implemented for recent chat servers:** review, rename, remove, and opt in/out of remembering entries.
- Keep nickname selection linked to the existing local identity keys rather than silently replacing an identity.
- Separate nonsecret preferences from credentials and private keys; do not save passwords as ordinary preferences.

### Join by invite
- Define a versioned invite format with address, port, TLS mode, and optional certificate fingerprint, using the trust flow from Priority 2.
- Support pasting an invite and generating one from the hosting summary.
- State whether a password is required without embedding the password or private keys in the invite.
- Validate malformed or unsupported invites and allow users to review connection details before joining.
- Explain which information is safe to share and why a fingerprint is useful only when the invite comes from a trusted source.

**Acceptance:** a returning user can select a recent server without retyping its details; a first-time guest can join from an invite and understand any password or trust decision.

## Priority 4: Refine discovery and in-chat guidance

### Server discovery
- Review existing listings for server name, address, user counts, password requirement, and TLS status; add missing useful details without overcrowding the list.
- Distinguish an empty directory from an unreachable directory and offer retry or manual connection.
- Keep directory configuration and listing details readable in narrow terminals.

### Helpful empty states
- Extend existing empty-chat guidance with an invitation action for hosts.
- Explain when a room has no other users and suggest a useful next step.
- Provide clear next actions when a directory contains no servers.

### Discoverable help and commands
- Organize command help by task and add short examples.
- Retain visible keyboard shortcuts, the help panel, and existing command/room/nickname completion; check that suggestions stay current as users and rooms change.
- Make connection, security, and hosting help easy to revisit without losing a draft or conversation position.

**Acceptance:** users can find a room, discover a relevant command, and understand an empty result without consulting the README or losing their draft.

## Priority 5: Fit varied terminals and validate release readiness

### Small and varied terminals
- Wrap or scroll long setup guidance and error details instead of clipping them to a single line.
- Keep essential actions and security decisions accessible at the supported minimum size of 40 × 12.
- Verify resizing preserves drafts, form values, selection, and scroll position where possible.
- Check long nicknames, room names, addresses, paths, and Unicode text for clipping, editing, and alignment issues.

### Platform and network validation
- Run build and test checks on Windows and macOS as well as Linux.
- Exercise full join/host/discover flows in representative terminals, including small layouts and resizing.
- Test reconnect, server restarts, wrong passwords, changed certificates, and slow or unavailable network services.
- Exercise real LAN and internet hosting with UPnP enabled, unavailable, and disabled; record limitations instead of treating automated tests as router validation.
- Document a reproducible release smoke-test checklist and known limitations.

### Optional mouse support
- After keyboard flows are reliable, consider mouse selection, scrolling, and room switching where supported.
- Keep every action available through the keyboard.

**Acceptance:** core tasks work on each supported platform and terminal layout, with documented results and known network limitations. Mouse support is optional and does not block release readiness.

## Suggested delivery sequence

1. Deliver asynchronous, cancellable connection attempts and structured errors.
2. Add retry, return-to-menu, and reconnect flows with draft preservation.
3. Deliver guided TLS setup, explicit certificate trust, and the hosting summary.
4. Save nickname selection, recent servers, and preferences; simplify first-run navigation.
5. Add shareable invites using the established certificate-trust flow.
6. Refine discovery, empty states, and task-oriented help.
7. Complete terminal and cross-platform validation, addressing issues found before considering optional mouse support.

Treat readable errors, security prompts, and resize behavior as acceptance requirements throughout delivery, rather than postponing all compact-layout work to the final phase.

## How to evaluate progress

For each milestone, observe whether a new user can complete its acceptance tasks without consulting the README. Record time to first conversation, failed attempts, requests for help, and places where users hesitate; establish a baseline before setting numerical targets.

Use representative scenarios: join by address or invite, host an encrypted LAN chat, diagnose failed internet hosting, reject an unexpected certificate, recover from a dropped connection, return to a recent server, and complete setup in a small terminal.

Keep the existing test suites passing. Add focused automated coverage for connection deadlines/cancellation, retries, reconnect state, preference persistence, invite parsing, and trust decisions as those behaviors are implemented. Pair automated checks with manual terminal and real-network validation. Update this roadmap's baseline and remaining work as milestones are completed.
