# mcp-token-scope

Give each MCP token its own scope: the federations, groups and (for
superadmins only) private chats it may read. The decision
([2026-11-20-mcp-token-scope.md](../design/decisions/2026-11-20-mcp-token-scope.md)),
web migration 000019 and the Go part (the scope applied at `/mcp`, the
token administration reading and writing it, OpenAPI and
web/docs/mcp.md) are done; the React part is left.

## Why

Every MCP token reads the same thing: every exposed group. A token
handed to one tool or one person cannot be limited to the chats that
tool or person needs.

## Scope

- React (frontend): the token form and list show and edit the scope
  (McpTokensProps.tokens[].scope and scope_options; written as `scope`
  on createMcpToken and renameMcpToken), inactive entries marked, and
  the 403 and 400 refusals shown with their message.

## Depends on

Nothing.

## Done when

- The token form and list show and edit the scope; React tests cover
  the form.
