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
expects JSON. `/mcp` answers a plain **401**.

### Why the 401 carries no `WWW-Authenticate` header

This looks like a bug and is not. OAuth is **optional** for MCP servers, but the header is
not decorative: a 401 carrying `WWW-Authenticate` is the signal that the server is an OAuth
protected resource, and clients *"MUST parse `WWW-Authenticate` headers and respond
appropriately"* — by fetching `/.well-known/oauth-protected-resource`, discovering an
authorization server, and attempting Dynamic Client Registration.

This server implements none of that, so sending the header advertised a flow that does not
exist. Claude Desktop duly probed the discovery endpoints, got 404s, and failed with
*"Couldn't register with tgloggerd's sign-in service… add an OAuth Client ID"* — never
reaching the token it had already been given.

**If OAuth is added later, add the discovery endpoints and the header together — never the
header alone.**

### A caveat on `?key=`

The authorization spec says access tokens **"MUST NOT be included in the URI query
string."** That rule is about OAuth access tokens and this server is not an OAuth resource
server, so it is not violating a rule it is subject to — but the practical consequence
stands: **query-string credentials are outside what MCP standardises, so no client is
obliged to support them.** Some pass the URL through verbatim and it works; others may
not. A client that can set a header should always use one.

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
| Missing/revoked token | `401`, **no** `WWW-Authenticate` (see below) |
| Unsupported `MCP-Protocol-Version` | `400` |
| `Origin` not in `MCP_ALLOWED_ORIGINS` (when set) | `403` |

Protocol version `2025-06-18`; `2025-03-26` is accepted and assumed when the header is
absent.

### `Origin`

The spec asks servers to validate `Origin` as a DNS-rebinding defence. That guidance is
written for the common case of an MCP server on localhost with no authentication; this
endpoint is neither, and two stronger things already stand in a hostile page's way:

1. **A bearer token is required** — an attacker's page does not have one.
2. **No CORS headers are ever sent**, so a cross-origin page cannot *read* a response even
   if it manages to send a request; and the `application/json` content type forces a
   preflight this server does not answer, so it usually cannot send one either.

So **the default is to allow**. Set `MCP_ALLOWED_ORIGINS` to a comma-separated list (or
`*`) to restore strict checking — worth doing if this endpoint is ever exposed without a
token.

An earlier version refused any request carrying an `Origin` unless an allowlist was
configured. That rejected every legitimate browser-based and Electron client — including
Claude Desktop — with a **403 raised before authentication was even considered**, which
surfaced to the user as an unexplained sign-in failure. It broke real clients to defend
against an attack the token already prevents.

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
| `telegram_list_group_admins` | A group's admins, owner first, with the privileges each holds. |
| `telegram_list_group_senders` | Everyone who has ever posted in a group, busiest first. |
| `telegram_get_user` | One user's full record by id: names, usernames, bio, phone, birthday, flags. |
| `telegram_get_user_history` | How a profile changed over time: names, usernames, bios, phones, photos. |
| `telegram_get_group` | One group's full record: usernames, photo, counts, participant count, message span. |
| `telegram_get_group_history` | Titles, descriptions, usernames, photos and admin changes over time. |

`telegram_list_group_admins` reports a **snapshot**: Telegram does not push admin changes to
a regular account, so the list is refreshed by polling and can lag a very recent promotion.
Only privileges actually held are listed — 17 booleans, mostly false, are noise.

`telegram_list_group_senders` measures **participation, not membership**. It can only see
people who have posted, so lurkers never appear; counts include messages later deleted; and
channel posts and anonymous-admin messages are excluded, since those are sent by the chat
rather than by a user.

`telegram_get_user` **omits fields the archive never saw** rather than returning them empty.
`telegram_user_extra_info` is sparse — 56k rows for 298k users — so "this user has no bio"
and "we never learned one" are different answers, and blank strings would conflate them.

Fetchable URLs appear wherever the archive holds a file: profile photos
(`profile_photo_url`), group photos (`photo_url`), message attachments
(`media.url`), and every photo entry in both history tools.

A URL is emitted **only when the bytes are actually stored**. Files at or above
`TG_MAX_STORE_FILE_SIZE` are recorded but not kept, and `/files/<token>` answers 404 for
those — so a link would promise something the archive cannot deliver. Message media carries
`stored: false` in that case, with the metadata (type, size, filename) still present,
because "this attachment existed and was a 2 GB video" is worth knowing even when the bytes
are not there.

Both user tools attach a URL alongside a profile photo's `file_id`:
`profile_photo_url` on the current photo, `url` on each history entry. These are the same
`/files/<token>` links the web UI uses — no session needed, so an MCP client can retrieve
the image directly.

Be aware what that means: **a file URL is a bearer credential for that one file**, minted
under `WEB_APP_KEY` and independent of the MCP token that produced it. Revoking an MCP
token does not invalidate links already handed out; only rotating `WEB_APP_KEY` does, and
that signs every web session out too. The links are unguessable and scoped to a single
file, but they are shareable once emitted.

The base URL comes from `WEB_PUBLIC_URL`, falling back to `TG_DISCORD_PUBLIC_URL` since the
daemon already uses that to build these very links. With neither set the URL field is
omitted rather than emitted relative, because a relative path is useless to a client that
is not a browser on this site.

`telegram_get_user_history` timestamps are **when a change was observed**, not when it was
made: the daemon polls, so a value changed and reverted between observations leaves no
trace, and the oldest entry of each kind is usually the value at first sight rather than a
change. `kinds` narrows which categories are fetched.

The two group tools return an empty list for a group that is not exposed, rather than an error — refusing
explicitly would confirm the group exists, which is itself something the allowlist withholds.

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

**Listing a group's senders needs `(chat_id, sender_user_id)`.** Without it, "who has ever
posted here?" reads every message in the group and de-duplicates — 2.3s over one group's
236,490 messages to produce 292 names, and proportional to the group's size rather than the
answer's, so it degrades exactly on the busiest groups. Migration `000023` adds the index,
making the scan covering: 143ms, at about 8% growth in index size.

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
