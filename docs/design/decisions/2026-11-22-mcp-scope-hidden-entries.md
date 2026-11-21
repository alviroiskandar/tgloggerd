# Decision log: 2026-11-22, Scope entries a viewer cannot see

Decided by the project owner on 2026-11-22, for the MCP token list,
following [2026-11-22-mcp-token-owner-view.md](2026-11-22-mcp-token-owner-view.md):

- A scope entry for a group the viewer cannot see tells that viewer
  nothing about the group: no name, and neither whether the entry is
  active nor why not. It is sent with `active` null and
  `inactive_reason` `hidden_from_viewer`, and the page shows it as a
  group hidden from the viewer. This closes the last bit an admin
  could learn about a hidden group (whether it is exposed) from a
  member's token that holds it. Saving a scope keeps such entries, as
  before.
- Admins' tokens keep answering the user-lookup tools for every
  account, as admins see every account on the web; a group hidden
  from an admin does not hide its members from the admin's token.
- Reason: hiding a group from an admin is meant to hide it entirely;
  the token list must not be a way around that.
