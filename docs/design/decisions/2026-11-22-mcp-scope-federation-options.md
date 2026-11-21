# Decision log: 2026-11-22, Federations a viewer cannot see in the token form

Decided by the project owner on 2026-11-22, for the MCP token form,
following [2026-11-22-mcp-scope-hidden-entries.md](2026-11-22-mcp-scope-hidden-entries.md):

- The federations the token form offers (`scope_options.federations`)
  leave out those the viewer cannot see, by the same rule as a
  federation entry hidden from the viewer: a federation that has at
  least one group, exposed or not, and none the viewer sees (for
  Default, the groups in no other federation). A federation with no
  group at all stays offered. Superadmins see every group, so their
  list is unchanged.
- A new federation entry the viewer cannot see by that rule is
  refused like a new entry for a hidden group (400 `bad_request`). One
  the token already holds is kept when the scope is saved, as for a
  held hidden group.
- Reason: a federation's name in the form, or a write that succeeds,
  would tell an admin that a federation of groups hidden from it
  exists; hiding a group from an admin is meant to hide it entirely.
