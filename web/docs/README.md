# tgloggerd web — documentation

Developer docs for the tgloggerd web UI (Drogon + MySQL).

- [db-naming.md](db-naming.md) — database table naming convention: the
  per-platform prefixes (`telegram_`, `discord_`), what is and isn't prefixed,
  and the rule to follow when adding tables for a new messaging platform.
- [routes-api.md](routes-api.md) — the `/routes` admin page and its endpoints:
  managing the Discord → Telegram forwarding routes `discordd` reads, how bot
  tokens are kept write-only, the extra `GRANT`s the page needs, and why a used
  route is disabled rather than deleted.
- [search-api.md](search-api.md) — the advanced-search JSON API
  (`/v1/search/...`): the `[{c,o,v,n}]` condition grammar, operators, the users
  field registry (including history filters), response shape, limits and worked
  examples.
