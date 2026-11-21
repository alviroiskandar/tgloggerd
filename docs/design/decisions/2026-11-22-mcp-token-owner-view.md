# Decision log: 2026-11-22, MCP tokens read only what their owner sees

Decided by the project owner on 2026-11-22, extending
[2026-11-21-mcp-token-owner-grants.md](2026-11-21-mcp-token-owner-grants.md):

- A token owned by an admin reads only the exposed groups the admin
  sees on the web: groups hidden from that admin are out of its
  reach, whatever its scope says, as a member's token stays within
  the member's grants. A hidden group's scope entry shows as inactive
  with the reason `outside_owner_grants`.
- The user tools (get_user, get_users, get_user_history) of a token
  owned by a member or an admin describe only the users that owner
  may see on the web; any other user answers as an unknown user does.
- Superadmins' tokens and tokens with no owner record are unchanged.
- Reason: a token is a way into the archive on its owner's behalf, so
  it must not show its owner anything the web would not.
