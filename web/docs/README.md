# tgloggerd web — documentation

Developer docs for the tgloggerd web UI (Drogon + MySQL).

- [db-naming.md](db-naming.md) — database table naming convention: the
  per-platform prefixes (`telegram_`, `discord_`), what is and isn't prefixed,
  and the rule to follow when adding tables for a new messaging platform.
- [mcp.md](mcp.md) — the `/mcp` Model Context Protocol endpoint: how exposure
  is gated to an admin-curated allowlist, bearer-token auth, the AND/OR/NOT
  filter grammar, the Telegram tools, and the two query-plan traps that make
  full-text search and recent-message listing fast.
- [platform-forwarding.md](platform-forwarding.md) — the `/platform-fwd`
  section: how the per-direction pages are laid out and named, the Discord →
  Telegram endpoints in full, how bot tokens are kept write-only, the extra
  `GRANT`s needed, and why a used route is disabled rather than deleted.
- [search-api.md](search-api.md) — the advanced-search JSON API
  (`/v1/search/...`): the `[{c,o,v,n}]` condition grammar, operators, the users
  field registry (including history filters), response shape, limits and worked
  examples.
