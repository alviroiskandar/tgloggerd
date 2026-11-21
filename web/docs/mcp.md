# MCP server (`/mcp`)

A [Model Context Protocol](https://modelcontextprotocol.io) endpoint that lets a language
model query the Telegram archive. Read-only, bearer-authenticated, and restricted to
groups an administrator has explicitly exposed.

The protocol engine is [`src/gwmcp`](../../src/gwmcp/README.md), a self-contained library
with no tgloggerd dependency. This document covers the parts that live in the web app: the
HTTP transport, authentication, the filter grammar, and the tools.

Two services answer it the same way, byte for byte: the Drogon web, and the Go service
(`webapi/internal/mcp`, transport in `webapi/internal/server/mcp.go`), whose tests replay a
set of requests against the Drogon web's captured answers (`webapi/README`, MCP). nginx
decides which one serves `/mcp`; both read the same tokens, the same allowlist and the same
settings. One difference: the Go service authorises a token by its owner, the Drogon web
still by its minter (see [Authentication](#authentication)).

## Exposure — the part that matters

**Nothing is readable until an admin adds it.** `/mcp-admin/groups` manages
`telegram_public_groups`; every message query is ANDed with

```sql
AND m.chat_id IN (SELECT group_id FROM telegram_public_groups)
```

**The same allowlist also makes a group's chat public on the web.** For a listed group,
`/groups/<id>/chat` and the JSON feed behind it (`/v1/chats/group/<id>/messages`) answer
without a session, and the media they link (`/files/<token>`) was already public by
design. Nothing else opens up: `/users/<id>`, the group's detail and admin pages, search
and every list page still require signing in, and an unlisted group's chat still redirects
to `/login` -- the same answer whether or not the group exists. In the anonymous view a
reply quoting a message from *another* chat shows no preview, since that chat may not be
listed. A signed-in session takes precedence: it sees any group, as before.

Consequences, all verified against the running server:

- With an empty allowlist, every message tool returns zero rows.
- Naming a private group in a filter returns **zero rows, not an error** — the gate is a
  conjunction, so it cannot be argued with.
- An `or` that names a private group beside an allowed one still yields only the allowed
  one's messages.
- `telegram_private_messages` — direct messages — is not referenced by any file under
  `web/src/mcp/`, and in `webapi/internal/mcp/` only by `private.go`, which is read only
  for a private chat in a superadmin's token scope (below). That is a grep-checkable
  invariant, not a convention (the Go package's tests check it).

Publicness is **curated, not derived**. A group having an active public username is shown
in the admin UI as a hint and is never acted on; see `migrations/000022` for why.

`telegram_get_users` is deliberately *not* gated: it describes accounts, not
conversations, and returns the same profile data the web UI already shows — including
phone numbers where the archive has them. On a token a member or an admin owns, the
user tools (`telegram_get_users`, `telegram_get_user`, `telegram_get_user_history`)
describe only the users that owner sees on the web
([decision](../../docs/design/decisions/2026-11-22-mcp-token-owner-view.md)), by the
web's own rule (`scope.UserFilter`): an admin sees every user, a member the senders and
admins of its granted groups, exposed or not. Any other id answers exactly as an id the
archive does not hold: `telegram_get_user` the same error, `telegram_get_user_history`
every category empty, `telegram_get_users` leaves it out. Tokens of superadmins, and
tokens without an owner record, are unchanged.

### Token scopes (Go service)

A token may be narrowed to a **scope**
([decision](../../docs/design/decisions/2026-11-20-mcp-token-scope.md),
`web_mcp_token_scopes`, web migration 000019): a set of federations, groups and, on a
superadmin's token only, private chats. The exposure list still decides; the scope only
narrows it, except for private chats.

- No entries: every exposed group, as before. Every token minted before scopes is such
  a token, and its answers are unchanged, byte for byte.
- Entries: the exposed groups named by a `group` entry or in a `federation` entry
  (Default, id 0, is the groups in no other federation), plus the `private` chats. A
  group that is not exposed is never reached, whatever names it.
- A token a **member** or an **admin** owns reads only the exposed groups its owner
  sees on the web: for a member those within its own grants
  ([decision](../../docs/design/decisions/2026-11-21-mcp-token-owner-grants.md)), for
  an admin every group but those a superadmin hid from it
  ([decision](../../docs/design/decisions/2026-11-22-mcp-token-owner-view.md)). Its
  entries narrow within that, never beyond it, and no entries is every exposed group
  within it. Tokens of superadmins, and tokens without an owner record, are not bounded
  so. One function builds the owner's web scope from its account (`mcp.OwnerScope`),
  for `/mcp` and for the token list alike.
- Read on every request: a group added to or taken out of a federation, a group
  withdrawn from MCP or exposed again, a token changing owner, a grant given to or
  withdrawn from a member owner and a group hidden from or shown to an admin owner
  count from the next request on.
- An entry that reaches nothing stays (a deleted federation, a withdrawn group, a
  private chat on a token now owned by an admin or a member, an entry outside what a
  member or admin owner sees). A token whose every entry
  is inactive reads **nothing**; it never falls back to every exposed group.
- The one place in the code: `telegram.readable` (`webapi/internal/mcp/gate.go`). The
  transport resolves the token's entries into a `Reach` (`scope.go`) per request and puts
  it in the request's context; every tool asks `readable`, which applies it to the
  exposure list. A scope that cannot be read fails the request with 500 rather than read
  unnarrowed.

A private chat is read by **naming it**: pass its chat id (the peer user's id, positive)
as `group_id` to `telegram_search_messages`, `telegram_list_recent_messages`,
`telegram_get_messages`, `telegram_get_message_history`, `telegram_popular_words`,
`telegram_list_group_senders` or `telegram_count_user_messages`. The answers have the
groups' shape: `group_id` is the chat id, `group_title` the peer's name, and there are
no channel posts. A private message also carries `is_outgoing`, true for one the
logging account sent (with `compact`, only when true; `fields` may name it, though it
is not in the tools' field lists, so group answers are unchanged). Such a message has
the logging account as its sender (`sender_user_id`, `sender_name`,
`sender_username`), as the web's chat shows it, once the daemon has reported the
account (`GET /v1/status`); until then its sender is 0 with an empty name. The same
holds for `telegram_get_message_history` (`sender_user_id`, `is_outgoing`), for
`telegram_list_group_senders`, which lists the logging account among the chat's
senders, and for `telegram_count_user_messages` and a `sender_user_id` filter, which
count its messages under its id. Searching
or listing recent messages without a `group_id` covers groups only, and the group tools
(`telegram_get_group`, `telegram_get_group_history`, `telegram_list_group_admins`)
answer a private chat's id as any unreadable id. `telegram_list_groups` adds
`private_chats` (`chat_id`, `name`, `username`) and a `private_chats_note` saying how to
read them, only for a token that has any, so every other answer stays as it was.

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
the audit trail survives. A token is authorised by its current **owner**, not by the
account that minted it: it works while its owner is active and, for a member, holds
`mcp_tokens`; a token with no owner record counts as the superadmins' and works while an
active superadmin exists. Deactivating an account, or taking `mcp_tokens` from a member,
stops the tokens it owns on their next request, and a token transferred to an active
owner keeps working after its minter is deactivated. A refused token gets the same 401 as
an unknown one. This is the Go service's rule; the Drogon web still refuses a token whose
minter is deactivated and nothing else, until `/mcp` moves to the Go service.

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
| Missing/revoked token, or one whose owner may not hold it | `401`, **no** `WWW-Authenticate` (see below) |
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

### Three timestamps, and why that matters

A message carries three, and confusing them silently returns the wrong rows:

| Field | Means | Ask it for |
|---|---|---|
| `date` | when it was **sent** | "what was said last week" |
| `edit_date` | when it was **last edited** | "what changed last week" |
| `deleted_at` | when the deletion was **observed** | "what was removed last week" |

They are not interchangeable. A message sent in December 2022 and edited in December 2024
appears in **no** window on `date` — so "everything edited in December 2024", asked with a
`date` filter, misses it. Measured on this archive that exact query returns 74 messages by
send time and 75 by edit time; the one it drops is message `681387`, edited 735 days after
it was sent.

`edited` and `deleted` are the presence forms — `= true` for "it happened", `= false`
for "it did not" (`is_not_null` / `is_null` mean the same thing) — each compiled to
match how its column spells "never": `edited` reads a column that stores `0`,
`deleted` one that stores `NULL`.

Ordering on all three is index-backed (migration `000025`); see
[Performance notes](#performance-notes).

### Convenience fields

`sender_is_bot` filters on whether the author is a bot, joined from the user table, so bot
noise can be excluded without first discovering any bot's id:

```json
{"not": {"field": "sender_is_bot", "op": "=", "value": true}}
```

`text_length` compares the character count, for skipping megaposts during a scan
(`{"field": "text_length", "op": "<", "value": 500}`).

## Tools

| Tool | Notes |
|---|---|
| `telegram_list_groups` | Start here — the others take a `group_id`, and only these are queryable. |
| `telegram_list_recent_messages` | Newest first, optionally one group. |
| `telegram_search_messages` | Full filter tree. `include_total` is off by default. |
| `telegram_get_users` | By `user_id`, `username`, `phone_number` or name. Requires a filter. |
| `telegram_list_group_admins` | A group's admins, owner first, with the privileges each holds. |
| `telegram_list_group_senders` | Message-count leaderboard for a group, busiest first. Optional date range. |
| `telegram_count_user_messages` | How many messages one user sent to one group. Optional date range. |
| `telegram_get_user` | One user's full record by id: names, usernames, bio, phone, birthday, flags. |
| `telegram_get_user_history` | How a profile changed over time: names, usernames, bios, phones, photos. |
| `telegram_get_group` | One group's full record: usernames, photo, counts, participant count, message span. |
| `telegram_get_group_history` | Titles, descriptions, usernames, photos and admin changes over time. |
| `telegram_get_message_history` | ONE message's earlier versions and its deletion record. `format: "diff"` for unified diffs. |
| `telegram_get_messages` | **Batch.** Up to 100 messages of a group by id; `include_history` inlines every revision. |
| `telegram_popular_words` | A group's most-used words, noise filtered. Defaults to the last 30 days. |

Every message result carries **`is_edited` and `is_deleted`**, always present rather than
inferred from a missing field, plus `history_available_via` pointing at the tool that can
say what changed. The text in a listing is only the *latest* version, which matters most
when it is being quoted.

**Edited and recoverable are not the same thing.** Telegram marks a message edited, but an
earlier version exists only if the daemon saw the edit happen — archive-wide there are
8,547 edited messages in one group against 1,378 edit snapshots in total. When a message is
flagged edited with no captured version, `telegram_get_message_history` returns
`previous_version_count: 0` **and a `note` saying why**, because silence there would read
as "nothing changed" — the opposite of the truth.

**`edit_date` on a version means that version's own start**, not the moment it was
replaced — `0` for the original text of a message that was later edited. It is the
same quantity the live message reports, so a full history reads as the returned
versions followed by the current message, each stamped with when it came into being.
`observed_at` is separate and is when the daemon *recorded* the version, which can lag
the edit itself.

Deletion does not erase content: a deleted message keeps its text, and
`deleted_observed_at` records when the deletion was *noticed*.

`telegram_list_group_admins` reports a **snapshot**: Telegram does not push admin changes to
a regular account, so the list is refreshed by polling and can lag a very recent promotion.
Only privileges actually held are listed — 17 booleans, mostly false, are noise.

`telegram_list_group_senders` measures **participation, not membership**. It can only see
people who have posted, so lurkers never appear; counts include messages later deleted; and
channel posts and anonymous-admin messages are excluded, since those are sent by the chat
rather than by a user. The same three caveats apply to `telegram_count_user_messages`,
which is the single-user form of the same question.

Both take **`start_date` and `end_date`**, inclusive, in any format the filter grammar
accepts (`2026-01-01`, `2026-01-01T10:00:00Z`, or a unix timestamp). Omitting them counts
**all time**, and the result says so with `"range": "all time"` rather than leaving the
caller to infer it from two absent fields.

A zero from `telegram_count_user_messages` is ambiguous on its own — unknown user, wrong
group, or a group nobody exposed — so when the group is not on the allowlist it adds a
`note` saying so. That is not a leak: the caller supplied the id, and the answer is about
the allowlist, not about whether the group exists.

### Link previews

A message whose text contains a URL usually carries the preview card Telegram's
servers resolved for it, and message rows return it as `link_preview`:

```json
"link_preview": {
  "url": "https://x.com/inaho_vt/status/…", "display_url": "x.com/inaho_vt/…",
  "site_name": "X (formerly Twitter)", "title": "…", "description": "…",
  "type": "photo",
  "image": {"file_id": 1174645, "size": 601199, "stored": true, "url": "…"}
}
```

It is worth asking for because it is frequently the only surviving record of
what a link pointed at — the page can change or disappear, the card cannot.
`type` is the `LinkPreviewType` Telegram assigned (`photo`, `article`, `chat`,
`embedded_video_player`, …); Telegram tends to use `photo` when the page has a
dominant image and `article` when it does not, so an `article` with no `image`
is normal rather than a gap.

`image` follows the same rules as message media: it is omitted entirely by
`include_media: false`, and its `url` appears only when the bytes are actually
stored. `link_preview` is a valid name for the `fields` projection, so a scan
can ask for previews and nothing else.

### Scanning cheaply

A wide scan usually wants to know **which** messages match, not what they say — and the
text is nearly all of the bytes. Four controls, on every tool that returns message rows.
Each defaults to the old full row, so an existing client sees no change.

| Parameter | Effect |
|---|---|
| `fields: [...]` | Return only these keys. `message_id` is always included — it is the handle every follow-up needs, and a row without one is not a smaller answer but an unusable one. |
| `truncate_text: N` | Cut each text to N bytes (UTF-8 safe). Rows that were cut carry `text_truncated: true`. |
| `include_media: false` | Drop the media object — the largest per-row cost, rarely wanted while scanning. |
| `compact: true` | Omit `false`/empty fields, and hoist `group_id`/`group_title` to the envelope when `group_id` was a call parameter. |

Measured on 200 real rows, comparing the default response with
`fields:["message_id","sender_username","date","edit_date","text"]`, `truncate_text:80`,
`include_media:false`, `compact:true`:

| Scan | Default | Shaped | |
|---|---|---|---|
| long posts (`text_length > 500`) | 507,370 B | 37,860 B | **13.4×** |
| identity only (no `text` field) | 94,813 B | 14,560 B | **6.5×** |
| recent 200 (average text 89 chars) | 94,813 B | 24,266 B | 3.9× |

The last row is the honest floor: with an average text of 89 characters, an 80-byte
truncation has almost nothing to remove. The win scales with how much text you are not
asking for.

### Paging a live archive

`offset` is wrong for an archive that is still being written: new messages arrive between
pages, every later offset shifts by however many landed, and rows get silently duplicated
or skipped. Pass back the `next_cursor` from the previous response instead — it names the
last row (its sort value and row id), so the next page resumes exactly where the last one
stopped regardless of what was inserted meanwhile.

- The cursor is **opaque**; pass it back verbatim, never construct one.
- It is only valid for the ordering that produced it. A cursor minted under
  `order_by: date` used on an `edit_date` scan is refused with a message saying so, rather
  than silently returning plausible nonsense.
- Relevance ordering (the default when a `match` is present) has **no** cursor — a score is
  not a position. Add an explicit `order_by` to page a text search.
- A short page is the end, and carries no cursor.

`order: "asc" | "desc"` (default `desc`) and `order_by: date | edit_date | deleted_at`
control the sort. `limit: 0` with `include_total: true` returns just the count, so a job
can be sized before it is paged.

### Edits and deletions: the two-call pattern

"List everything edited in the last week, with what changed" used to mean paging the whole
week by send time, filtering client-side, then one `telegram_get_message_history` call per
edited message. On this archive that is roughly sixty calls. It is now two:

```json
// 1. what changed, and which of those actually have a recoverable revision
{"filter": {"field": "edit_date", "op": "between",
            "value": ["2026-08-17", "2026-08-25"]},
 "group_id": -1001483770714, "order_by": "edit_date", "limit": 200,
 "fields": ["message_id", "sender_username", "edit_date", "previous_version_count"],
 "include_media": false, "compact": true}

// 2. the revisions themselves, for as many as 100 messages at once
{"group_id": -1001483770714, "message_ids": [1314576, 1314540, ...],
 "include_history": true}
```

`previous_version_count` on the search row is what makes the first call sufficient:
**edited and recoverable are not the same thing.** Telegram marks a message edited, but an
earlier version exists only if the daemon was running and saw the edit happen. Without the
count on the row, discovering which edits have anything to show meant one call each — and
on the December 2024 window above, *all 75* have `previous_version_count: 0`.

`telegram_get_messages` never fails a batch because one id is unknown: those come back in
`missing_ids` while everything found comes back in `found`. An id in an unreadable group is
reported missing, for the same reason the other tools return nothing for one — saying
otherwise would confirm it exists.

`telegram_get_message_history` also takes `format: "diff"`, which returns a unified diff
between consecutive versions instead of the full text of each. A one-word correction to a
long message costs the whole message twice in `full` mode; in `diff` mode it costs the
message once plus the changed line. Measured on a 600-byte message: 1,869 B → 1,125 B.
Diffs are line-based and bounded — past 400 changed lines the entry degrades to a summary
rather than burning CPU on a large LCS.

### `telegram_get_users` in bulk

`user_id` is the primary key, so the `in` operator resolves up to 100 users in one indexed
call. Use it instead of looping:

```json
{"filter": {"field": "user_id", "op": "in", "value": [123, 456, 789]}}
```

`telegram_get_group` takes `group_ids: [...]` for the same reason, returning
`{groups, missing_ids}` instead of a single object.

### `telegram_popular_words`

The one tool with a **default window rather than all time**: with no dates it covers the
last 30 days. Word frequency is a snapshot of what is being discussed, and "ever" over a
decade-old group is both far more expensive and much less useful than "lately".

Counting reads message text, so it scans at most `max_messages` (newest first, default
50,000) and reports `messages_scanned` and `truncated`. When `truncated` is true the counts
describe that newest slice, not the whole window — stated in the result rather than left
for the caller to notice.

**Noise filtering is the whole difficulty.** Raw frequency over chat produces a ranking of
grammar, and the two vendored stopword lists are built from formal prose, which chat is
not. Four layers, each earning its place against this archive:

1. **Vendored stopwords** — `src/mcp/Stopwords.hpp` (and, generated from it,
   `webapi/internal/mcp/stopwords.go`), 885 merged entries from the
   [stopwords-iso Indonesian list][sw-id] and the [NLTK English list][sw-en]. Generated,
   and vendored rather than fetched at runtime: a tool must not depend on a third-party URL
   being reachable, and a word list that changes under you silently changes your results.
2. **Corpus noise** — `src/mcp/NoiseWords.hpp` (and its copy
   `webapi/internal/mcp/noisewords.go`; a Go test keeps the two equal), 384
   hand-maintained entries the formal
   lists have no reason to contain: chat spellings (`gak`, `yg`, `kalo`), particles
   (`aja`, `sih`, `dong`), internet shorthand (`lol`, `btw`, `imo`), interjections,
   quoted-mail scaffolding (`subject`, `wrote`) and calendar vocabulary (`jul`, `senin`).
   A hash set, not a sorted array, precisely because it is hand-maintained — an
   out-of-order insertion in a binary-searched array fails silently.
3. **Shape rules** — laughter and filler are an unbounded family (`wkwk`, `wkwkwkwk`,
   `hahahaha`), so a token built by repeating a one- or two-character unit is dropped, and
   a run of three or more identical characters collapses to one, merging `yesss` into
   `yes`. Three and not two so `coffee` survives.
4. **Structure** — URLs and e-mail addresses are dropped *whole* rather than split, and
   pure numbers are dropped. `@mentions` are kept: who gets talked about is a real answer.

The measured effect, one group, January 2026. Before: `aja`, `trump`, `gak`, `nya`, `udah`,
`update`, `sih`, `wkwk`, `pake`, `kalo`. After: `trump`, `update`, `gold`, `price`, `uring`,
`beli`, `pas`, `time`, `see`, `server`. Dropping addresses alone moved `com`, `org`, `vger`
and `gmail` out of a top six they had held purely on the strength of pasted `From:` lines.

The line held throughout is **topical vs not**. Words that could name a subject stay out of
the noise list even when frequent — `net`, `io`, `id` and `co` read as noise in a chat
corpus and as kernel subsystems in this one. Where that judgement is wrong for a particular
question, `exclude: [...]` drops further words and `include_stopwords: true` turns every
layer off and returns raw frequencies.

Only ASCII case is folded. Keeping every byte ≥ 0x80 as a word character holds a UTF-8 word
together instead of shredding it, but case-folding it would need a Unicode table that does
not belong here — so non-Latin words are counted, just not case-merged, and CJK comes out
as whole runs rather than words.

[sw-id]: https://github.com/stopwords-iso/stopwords-id
[sw-en]: https://gist.github.com/sebleier/554280

`telegram_get_user` **omits fields the archive never saw** rather than returning them empty.
`telegram_user_extra_info` is sparse — 56k rows for 298k users — so "this user has no bio"
and "we never learned one" are different answers, and blank strings would conflate them.

Fetchable URLs appear wherever the archive holds a file: profile photos
(`profile_photo_url`), group photos (`photo_url`), message attachments
(`media.url`), and every photo entry in both history tools.

A URL is emitted **only when the bytes are actually stored**. A file can be recorded
without its bytes: at or above `TG_MAX_STORE_FILE_SIZE`, from a chat the logging account is
not a member of, or evicted from the store since. `/files/<token>` then has to fetch it from
Telegram ([media.md](media.md)), which a request without a session (an MCP client's) may do
for small files only, and which fails once Telegram no longer has the file, so a link would
promise something the archive may not deliver. Message media carries `stored: false` in that
case, with the metadata (type, size, filename) still present, because "this attachment
existed and was a 2 GB video" is worth knowing even when the bytes are not there.

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

**Anything date-bounded needs `(chat_id, date, sender_user_id)`.** The existing indexes
each answer half of "one group, one time window", so a date bound turned a covering scan
into a row fetch per message and cost grew with the group's whole history however narrow
the window. A leaderboard whose `start_date` merely predates the group took **9.03s** —
past the 5-second statement timeout. Migration `000024` adds the index; the same query is
**0.22s**, a 30-day leaderboard **8ms**, and reading a month of message text for word
counting went 2.55s → **0.03s**. Word counting orders by `date`, not `message_id`, so the
same index supplies the ordering instead of a filesort over the window.

**The gate binds a literal id list, not a semi-join — and that is a performance
decision, not a stylistic one.** It used to be
`chat_id IN (SELECT group_id FROM telegram_public_groups)`. With the subquery MySQL drives
the join from the allowlist table and estimates ~90 rows per group against a real 238k, so
it never picks the `(chat_id, <timestamp>)` indexes: ordering by `edit_date` measured
**6.18s** — past the 5-second statement timeout, so the query could not complete at all —
*with the index already present*. Binding the ids as constants turns the same query into a
reverse covering range scan: **0.012s**. Every other shape improved too (date-ordered
0.138s → 0.0024s, full-text 0.136s → 0.048s), so there is no case where the old form was
better. The allowlist is curated and therefore small, which is what makes inlining it
affordable, and it still fails closed: an empty allowlist compiles to `1=0`, which matches
nothing, rather than to an empty `IN ()`, which is not valid SQL. It is produced in exactly
one function (`msgGate`), so "did we remember the gate?" is still a grep.

**Ordering on `edit_date` or `deleted_at` needs `(chat_id, edit_date)` and
`(chat_id, deleted_at)`.** Migration `000025`. Same shape as `000024` did for `date`: the
range becomes one contiguous stretch of index walked in reverse, which supplies the
`ORDER BY` for free. The two changes are a pair — reverting either one alone puts the
query back over the timeout.

**Listing a group's senders needs `(chat_id, sender_user_id)`.** Without it, "who has ever
posted here?" reads every message in the group and de-duplicates — 2.3s over one group's
236,490 messages to produce 292 names, and proportional to the group's size rather than the
answer's, so it degrades exactly on the busiest groups. Migration `000023` adds the index,
making the scan covering: 143ms, at about 8% growth in index size.

Tools run on their own event-loop pool (`MCP_THREADS`, default 2), not on an HTTP thread:
they use the blocking database API, and a slow query must delay only another MCP call. The
Go service has no such pool to keep free; it lets at most `MCP_THREADS` calls run at once
and the others wait for a turn, which bounds the read-only connections MCP holds the same
way.

## Administration through the JSON API (Go service)

The Go service serves the administration as JSON operations
(`webapi/openapi.yaml`, tag `mcp`), with the session's CSRF token in
`X-CSRF-Token`, and the pages' data (`/mcp-admin`, `/mcp-admin/groups`,
`/mcp-admin/tokens`):

    POST   /api/mcp/groups               {"group_id": -100..., "note": "..."}; 201
    DELETE /api/mcp/groups/{group_id}
    PATCH  /api/mcp/groups/{group_id}    {"note": "..."}; 204
    POST   /api/mcp/tokens               {"name": "...", "owner_id": n, "scope": [...]}; 201 {id, token}
    PATCH  /api/mcp/tokens/{id}          {"name": "...", "scope": [...]}, either or both; 204
    POST   /api/mcp/tokens/{id}/revoke
    POST   /api/mcp/tokens/{id}/transfer {"owner_id": n}; 204

- Who, for the groups: viewers with the `mcp` ability (superadmins
  and admins); to anyone else they do not exist (404).
- Who, for the tokens
  ([decision](../../docs/design/decisions/2026-11-20-mcp-tokens-to-superadmin.md)):
  viewers with `mcp_tokens`, which superadmins and admins hold by role
  and a member when an admin managing it or a superadmin gives it
  ([accounts.md](accounts.md)). A token is listed, renamed and revoked
  by its owner, by an admin when the owner is a member it manages
  (filed in a federation it administers), and by every superadmin; a
  token without an owner record counts as the superadmins'. Any other
  token is 404 like an unknown one, and the list, the index's count
  and its last use cover only the viewer's tokens. A member given
  `mcp_tokens` has the MCP menu entry with the index and the tokens,
  not the groups. Members do not get tokens without the ability; a
  member's token describes only the users of the member's granted
  groups (see the user tools above).
- Minting: `owner_id` (the viewer when left out) must be an active
  account that may hold tokens (a superadmin, an admin, or a member
  with `mcp_tokens`) whose tokens the viewer manages, so an admin mints
  for itself or its members and a member for itself only; any other is
  400. The viewer is always the minter (`web_user_id`), the account
  the token's requests are attributed to. `/mcp` authorises the token
  by its owner, by the same rule: an active account that may hold
  tokens, or for no owner record an active superadmin existing
  (see [Authentication](#authentication)); the minter's state does not
  count. The token, its owner record and
  the `owner_set` audit row are written in one transaction.
- Transfer: superadmins only, to such an account (400 otherwise; 404
  for everyone else). The token keeps its minter and works for its
  new owner. Taking `mcp_tokens` away from a member does not revoke
  its tokens: `/mcp` refuses them until the ability is given back or
  they are transferred.
- Scope ([Token scopes](#token-scopes-go-service)): `scope` is a list
  of `{"kind": "federation" | "group" | "private", "id": n}`, written
  when minting or by PATCH, which replaces it as a whole (`[]` is every
  exposed group again). Each entry once, at most 200. A federation
  must exist, a group must be one the viewer can see (it need not be
  exposed: the entry is inactive until it is), a private chat must be
  a user the archive knows, by its positive id; anything else is 400
  `bad_request` and nothing is written. A `private` entry is written
  only by a superadmin, on a token a superadmin owns or that has no
  owner record; otherwise 403 `forbidden`, with the reason in
  `message`. So an admin editing a token that still carries an
  inactive private entry from an earlier owner drops it to save. The
  tokens list gives each token's `scope` as `{kind, id, name, active,
  inactive_reason}` (federations, then groups, then private chats).
  `active` is false, and `inactive_reason` says why (null while
  active), the first that applies: `not_exposed` for a group not
  exposed, `federation_deleted` for a deleted federation,
  `private_not_superadmin` for a private chat on a token whose owner is
  not a superadmin, `user_unknown` for a private chat with a user the
  archive does not know, and `outside_owner_grants` for an entry of a
  member's or an admin's token that reaches no group its owner sees: for
  a member, a group not granted or a federation holding no granted
  group; for an admin, a group hidden from it or a federation whose
  exposed groups are all hidden from it (a federation with no exposed
  group is not out of reach because of the hiding). Then
  `scope_options`: the
  federations, the exposed groups the viewer sees, and `private`, true
  for a superadmin.
- Only groups the viewer may see are exposed, listed, counted or
  withdrawn; a hidden group is 404 like an unknown one
  (web-accounts.md section 4). Exposing a group twice is 409
  `already_exposed`. A note is trimmed and cut to 255 characters;
  PATCH rewords it in place (an empty note clears it, a body without
  `note` is 400) and leaves who exposed the group and when as they
  are. The web's database account may update that column and no
  other (`docker/mysql/web-grants.sql`).
- A token is minted as here, `tgmcp_` and 32 random bytes in hex,
  answered once and stored as its SHA-256; its name is 1 to 64
  characters (renaming cuts and refuses an empty name alike). Revoking
  stamps `revoked_at`; revoking twice is a no-op.
- Every change writes a `web_audit` row: `mcp_expose`,
  `mcp_unexpose`, `mcp_note`, `mcp_token_create` (with `for <owner
  id>` when minted for another account), `mcp_token_rename`,
  `mcp_token_revoke`, `mcp_token_transfer` (`from <id> to <id>`, 0 for
  no owner record), `mcp_token_scope` (`token <id>: <entries>`, or
  `all exposed groups`), beside the ownership rows `owner_set` and
  `owner_transfer` (with the group or the token's id and name, never
  the token; `mcp_note` and `mcp_token_rename` with `old -> new`, each
  cut to its share of the 255 characters when both do not fit). An
  unchanged note, name or scope, and a transfer to the owner a token
  has, write no row.
- An exposed group is owned by the account that exposed it, a token
  by the account it was minted for (`web_resource_relations`, see
  *Ownership* in [accounts.md](accounts.md)); withdrawing a group
  drops its owner, a revoked token keeps its own. Every token that
  existed before per-role management was given to `superadmin` by web
  migration 000015. The tokens list names each token's owner (or
  "Shared", id 0, without an owner record) and minter, with `owners`,
  who the viewer may mint for or transfer to, and `can_transfer`.
  What a token reads is the exposure list narrowed by its scope;
  ownership changes it for private chat entries, which work while a
  superadmin owns the token, and for a member or admin owner, whose web
  view bounds the token.

## Configuration

| Variable | Meaning |
|---|---|
| `MCP_THREADS` | Threads for tool execution (default 2, max 16); in the Go service, tool calls run at once. |
| `MCP_ALLOWED_ORIGINS` | Comma-separated browser origins, or `*`. Unset allows any origin (see `Origin` above); non-browser clients are unaffected. |
| `WEB_PUBLIC_URL` | Base of the absolute `/files/<token>` links; falls back to `TG_DISCORD_PUBLIC_URL`. |
| `MCP_LOG_REQUESTS` | `1` logs the shape of each request, never a credential's value. |

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
