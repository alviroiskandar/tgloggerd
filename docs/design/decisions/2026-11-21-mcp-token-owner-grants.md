# Decision log: 2026-11-21, MCP tokens of members follow their grants

Decided by the project owner on 2026-11-21, amending
[2026-11-20-mcp-token-scope.md](2026-11-20-mcp-token-scope.md):

- A token owned by a member reads only exposed groups the member's own
  grants let the member see, whatever its scope says: the scope
  narrows within that, never beyond it. A token with no scope entries
  owned by a member reads the exposed groups within the member's
  grants, not every exposed group.
- It follows the grants as they change: a grant withdrawn from the
  member takes its groups out of reach of the member's tokens at
  once, as a group withdrawn from MCP does; the scope entries stay and
  show as inactive.
- Superadmins' tokens and tokens with no owner record are unchanged.
- Reason: a member handed MCP tokens must not reach, through a token,
  chats the member cannot read on the web.
