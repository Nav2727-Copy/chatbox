# UX Improvement Roadmap

This roadmap focuses on making Chatbox easier to understand and use without changing it into a graphical application.

## Priority 1: Help users reach a conversation

### Guided first-run flow
- Make the primary choices clear: join a server, host a private chat, or discover servers.
- Offer sensible defaults and explain unfamiliar settings in plain language.
- Let users know what information they need before they begin.
- Provide a short first-run walkthrough that can be revisited later.

### Join by invite
- Define a simple invite format containing the server address and port, plus any other connection details users need.
- Make it easy to enter or share an invite from the terminal.
- Explain which details are safe to share and whether a password is required.

### Recent servers and preferences
- Remember the user's nickname, recent servers, and relevant preferences.
- Allow users to review and remove saved servers.
- Make returning to a recent conversation quicker than entering connection details again.

## Priority 2: Make connection and hosting problems solvable

### Actionable connection errors
- Distinguish common causes such as DNS lookup failure, connection timeout, TLS certificate problems, password rejection, and nickname conflicts.
- For each error, explain what happened and give a concrete next step.
- Preserve entered connection details when returning from an error so users do not have to start over.

### Hosting diagnostics
- Show the local address and port where the server is listening.
- Show whether UPnP port forwarding succeeded and, when available, the public address.
- If automatic forwarding fails, explain how to forward the port manually and identify which port and protocol are needed.
- Clearly indicate when an address is only reachable on the local network.

### Clear security choices
- Explain what TLS protects and what it does not protect.
- Explain the purpose of a server password and when people joining will need it.
- Describe certificate warnings in plain language, with a clear way to verify or reject a certificate.
- Label plaintext connections as insecure before the user enables them.

## Priority 3: Improve discovery and in-chat guidance

### Server discovery
- Show useful details in listings, such as server name, address, room or user counts, password requirement, and TLS status.
- Explain when no servers are available and how to check the directory connection or enter a server manually.
- Keep listing information readable in narrow terminals.

### Helpful empty states
- In an empty conversation, suggest how to invite someone or send the first message.
- When no servers are found, explain what the user can do next.
- When a room has no other users, make that clear and offer a relevant next step.

### Discoverable help and commands
- Keep keyboard shortcuts visible where practical and provide an in-app shortcut reference.
- Organize command help by task, with short descriptions and examples.
- Add command completion for available rooms and nicknames where relevant.
- Make it easy to return to help without losing a message draft or conversation state.

## Priority 4: Fit more terminal setups

### Small and varied terminals
- Keep menus, forms, and chat usable at narrow and short terminal sizes.
- Make resize behavior predictable and preserve drafts, form values, and scroll position.
- Check text clipping and alignment with long nicknames, room names, and server addresses.
- Keep important status and error information visible in compact layouts.

### Optional mouse support
- Where the terminal and curses implementation support it, allow mouse selection for menus, scrolling, and room switching.
- Keep keyboard controls fully usable so mouse support remains optional.

## Suggested delivery sequence

1. Improve first-run guidance and add clear explanations to join and host forms.
2. Make connection errors specific and preserve form input after failures.
3. Add hosting diagnostics for local address, UPnP, and manual port forwarding.
4. Save nickname, recent servers, and preferences.
5. Improve server listings, empty states, and command help.
6. Review small-terminal behavior and consider optional mouse support.

## How to evaluate progress

For each change, check whether a new user can complete the relevant task without consulting the README. Useful tasks include joining a server, hosting a chat for someone on the same network, diagnosing a failed connection, and returning to a recent server. Record where users hesitate or need outside help, then refine those screens and messages.
