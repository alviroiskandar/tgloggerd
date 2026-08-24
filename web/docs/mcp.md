# MCP server (`/mcp`)

A [Model Context Protocol](https://modelcontextprotocol.io) endpoint that lets a language
model query the Telegram archive. Read-only, bearer-authenticated, and restricted to
groups an administrator has explicitly exposed.

The protocol engine is [`src/gwmcp`](../../src/gwmcp/README.md), a self-contained library
with no tgloggerd dependency. This document covers the parts that live in the web app: the
HTTP transport, authentication, the filter grammar, and the tools.

## Exposure — the part that matters

**Nothing is readable until an admin adds it.** `/mcp-admin/groups` manages
`telegram_public_groups`; every message query is ANDed with

```sql
AND m.chat_id IN (SELECT group_id FROM telegram_public_groups)
```

Consequences, all verified against the running server:

- With an empty allowlist, every message tool returns zero rows.
- Naming a private group in a filter returns **zero rows, not an error** — the gate is a
  conjunction, so it cannot be argued with.
- An `or` that names a private group beside an allowed one still yields only the allowed
  one's messages.
- `telegram_private_messages` — direct messages — is not referenced by any file under
  `web/src/mcp/`. That is a grep-checkable invariant, not a convention.

Publicness is **curated, not derived**. A group having an active public username is shown
in the admin UI as a hint and is never acted on; see `migrations/000022` for why.

`telegram_get_users` is deliberately *not* gated: it describes accounts, not
conversations, and returns the same profile data the web UI already shows — including
phone numbers where the archive has them.

## Authentication

`Authorization: Bearer tgmcp_<64 hex>`, minted at `/mcp-admin/tokens`.

A token may also be passed in the query string, for clients that accept only a URL and
offer no way to set a header:

```
POST /mcp?key=tgmcp_<64 hex>
```

The header is preferred whenever both are present. **The query string is genuinely
weaker** and it is worth being deliberate about: unlike a header, it is recorded in
reverse-proxy and CDN access logs, kept in browser history, and leaked in the `Referer`
of any outbound link. Behind Cloudflare, assume the token reaches their request logs.

What limits the damage is that tokens are per-client, named, individually revocable, and
stored only as a hash. So the practical advice is: **mint a separate token for
query-string use**, name it accordingly, and revoke it on its own if you ever need to —
without disturbing clients that authenticate properly.

Shown once and stored only as a SHA-256, so a database dump grants nobody access. Lose one
and you revoke it and mint another. Revoking stamps `revoked_at` rather than deleting, so
the audit trail survives; deactivating an account disables its tokens too.

This route carries no session filters. Every other route redirects to `/login` when the
cookie is missing, which is useless to a machine client — it would receive a 302 where it
expects JSON. `/mcp` answers **401** with `WWW-Authenticate: Bearer`.

## Transport

Streamable HTTP, in its minimal legal form. The spec requires one endpoint serving POST
and GET, but a server that never initiates messages may answer every POST with
`application/json` and decline GET — so there is **no SSE, no session store, no
streaming**.

| Request | Response |
|---|---|
| `POST` with a JSON-RPC request | `200`, `application/json` |
| `POST` with a notification | `202`, **empty body** |
| `GET` | `405` + `Allow: POST` (no SSE stream offered) |
| `DELETE` | `405` (stateless; no session to end) |
| Missing/revoked token | `401` + `WWW-Authenticate: Bearer` |
| Unsupported `MCP-Protocol-Version` | `400` |
| Disallowed `Origin` | `403` |

Protocol version `2025-06-18`; `2025-03-26` is accepted and assumed when the header is
absent.

`Origin` is validated because the spec requires it (DNS-rebinding defence). A request with
no `Origin` — any non-browser client — is unaffected. A browser request is refused unless
its origin is listed in `MCP_ALLOWED_ORIGINS`.

## Filter grammar

A node is exactly one of:

| Form | Meaning |
|---|---|
| `{"field": …, "op": …, "value": …}` | a condition |
| `{"and": [node, …]}` | all must hold |
| `{"or": [node, …]}` | any must hold |
| `{"not": node}` | negation |

Groups nest, so `(A OR B) AND NOT C` is expressible — which the flat grammar behind
[`/v1/search`](search-api.md) cannot do at all.

```json
{"and": [
  {"field": "text", "op": "match", "value": "kernel panic"},
  {"or": [{"field": "group_id", "op": "=", "value": -1001483770714},
          {"field": "group_id", "op": "=", "value": -1001062351210}]},
  {"not": {"field": "content_type", "op": "=", "value": "photo"}},
  {"field": "date", "op": "between", "value": ["2026-01-01", "2026-06-30"]}
]}
```

**Operators:** `=` `!=` `<` `>` `<=` `>=` `contains` `not_contains` `starts_with` `in`
`between` `is_null` `is_not_null` `match`.

**Guards:** depth 8, 64 nodes, 512-byte values, 100 items in an `in`. A filter arrives
from a model and may be arbitrarily shaped by accident.

**Dates** accept `YYYY-MM-DD`, `YYYY-MM-DDTHH:MM:SS`, or an integer unix timestamp;
`m.date` is a unix BIGINT internally. An unparseable date is an error rather than a silent
`0`, because `0` is a legitimate stored value and would quietly match those rows.

**`match`** is MySQL boolean-mode full text: `+must -exclude "phrase"` all work. Words
shorter than three characters are ignored by the index (`innodb_ft_min_token_size`).

## Tools

| Tool | Notes |
|---|---|
| `telegram_list_groups` | Start here — the others take a `group_id`, and only these are queryable. |
| `telegram_list_recent_messages` | Newest first, optionally one group. |
| `telegram_search_messages` | Full filter tree. `include_total` is off by default. |
| `telegram_get_users` | By `user_id`, `username`, `phone_number` or name. Requires a filter. |

All are annotated `readOnlyHint: true` and none can write.

Output is **not HTML-escaped**, unlike `dao::search`. That layer escapes because its rows
land in a browser; these land in a model, where `it&#39;s &lt;b&gt;` is wrong and
unescaping later would be lossy.

Errors use MCP's two channels correctly: an unknown tool or malformed params is a JSON-RPC
error, while a filter the caller can fix is a result with `isError: true` and a message
written to be self-correcting (`unknown field "nope"`, `"banana" is not valid for field
"content_type"; expected one of: …`).

## Performance notes

Both of these were plan problems, found only against the real 4.6M-row table.

**Full-text search is split in two.** With the display joins present, MySQL cannot use the
fulltext index order for `ORDER BY`, so it materialises every hit and sorts — 65,558 rows
and 11.5s to return three. Selecting ids from the message table *alone* lets the index
supply rows in relevance order so `LIMIT` stops early; the joins then decorate at most
`limit` rows. 18 rows examined, 0.10s end to end.

**Recent messages fan out per group.** Ordering by date across the gated set materialises
every message in every exposed group, and there is no `(chat_id, date)` index to avoid it:
2.6s. Within one group `message_id` is monotonic and covered by
`uq_group_messages_chat_msg`, so a reverse index walk stops at `limit` — 2.6ms. No single
query does that across groups, so one indexed walk per exposed group is the plan, merged
in C++. Affordable precisely because the allowlist is curated. 2.6s → 0.01s.

Tools run on their own event-loop pool (`MCP_THREADS`, default 2), not on an HTTP thread:
they use the blocking database API, and a slow query must delay only another MCP call.

## Configuration

| Variable | Meaning |
|---|---|
| `MCP_THREADS` | Threads for tool execution (default 2, max 16). |
| `MCP_ALLOWED_ORIGINS` | Comma-separated browser origins, or `*`. Unset refuses all browser requests; non-browser clients are unaffected. |

No grants beyond those in `migrations/000022` and `web/migrations/000003` are needed.

## Trying it

```bash
curl -s -X POST http://10.0.88.4:8080/mcp \
  -H 'Authorization: Bearer tgmcp_…' \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize",
       "params":{"protocolVersion":"2025-06-18","capabilities":{},
                 "clientInfo":{"name":"curl","version":"1"}}}'
```

Then `tools/list`, then `tools/call`.
