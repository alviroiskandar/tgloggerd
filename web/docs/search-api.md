# Advanced Search API

A read-only JSON API for querying the logged entities with multi-condition
filters, JOINs and history lookups. It backs the `/users`, `/groups` and
`/files` pages, but can be called directly.

Modeled on a flat `[{c,o,v,n}]` condition grammar. Every SQL token except the
bound `?` values comes from a server-side allowlist (the per-entity **field
registry**), so the endpoint is injection-safe by construction.

## Endpoint

```
GET /v1/search/{entity}
```

`{entity}` is `users`, `groups`, `files`, `private_messages` or
`group_messages` — each shares the same grammar with
its own field registry (an unknown entity returns **404**). Requires a logged-in
session (the shared `AuthFilter`); an unauthenticated request is redirected (302)
to `/login`, exactly like the other `/v1` routes.

## In the Go service

The Go web service (webapi/) serves the same search at

```
GET /api/search/{entity}
```

with the same parameters, the same registries and the same answer,
cell for cell (webapi/internal/logdb/search.go ports this DAO; the
tests in webapi/internal/server/search_test.go check its answers
against the ones this code gives). It lives under `/api/`, which nginx
sends to the Go service, while the Drogon pages keep calling their own
`/v1/search` until they move. Its contract is `searchEntity` in
webapi/openapi.yaml. Where the two differ:

- Without a session it answers **401** `{"error":"unauthenticated"}`
  rather than redirecting to `/login`.
- Errors are the Go API's `{"error": <code>, "message": <sentence>}`:
  `bad_search` with this page's messages for a refused search,
  `search_timeout` for one that ran out of time (both 400), and
  `not_found` (404) for an unknown or hidden entity. A `matches` value
  MySQL's boolean mode cannot parse (a lone `+`, `-` or `~`, an
  unbalanced parenthesis, a stray `@`) is a refused search too:
  `bad_search` with "invalid full-text search: " and MySQL's message,
  and on a list page the whole list with that error in its state. Any
  other database error that is not a timeout is a 500, not the timeout
  message.
- Everything is read through the request scope (docs/design/
  web-accounts.md section 9). `private_messages` does not exist (404)
  for a viewer who may not see private chats. The rows of the others
  are limited in the SQL itself, ANDed with the conditions, so rows
  and totals count only what the viewer may see: a member sees the
  users of its visible groups (senders and admins there) by id, name,
  username, type and photo only, and `fields` and `cols` list only
  those; it sees the files sent in the messages it may see, and the
  messages of its visible groups. A sender chat the viewer may not see
  is shown as `{kind: "none", name: "(hidden)"}`. The table's row
  estimate (`total_exact` false on a browse without conditions) is
  only given to a viewer who sees the whole table.
- `debug=1` is honoured for superadmins and admins.
- `content_type` takes the kinds daemon migration 000034 appended
  (`video_note` to `unsupported`, docs/design/message-types.md section
  4.1) besides the ten the Drogon registry lists.
- The message entities have two more fields: `tdlib_type`, the exact
  TDLib class (`messageStakeDice`, `messageLiveLocation`; NULL for rows
  logged before migration 000034, which neither `=` nor `!=` matches),
  and `service_type`, a service message's action (`pin_message`,
  `chat_add_members`, ...). Neither has an index of its own, so a
  search may use them only beside a `chat_id =` condition in a chain
  whose connectors are all AND; it then reads that chat's rows through
  a chat_id index instead of scanning the table. Otherwise it is
  refused (`bad_search`: "field 'service_type' requires a 'chat_id ='
  condition ANDed with the others"). `fields` marks them with
  `needs_chat: true`. Both are open enums: `fields` lists the values
  the daemon writes in `enum` (`service_type` its service actions,
  `tdlib_type` the TDLib classes it knows, in the daemon's order) and
  marks them `enum_open: true`, and a value not listed (a service type
  or TDLib class newer than the web) is still taken, so a search that
  worked before still does.
- Enum fields take one more operator, `in`: one of a list of values,
  whose `v` is a JSON array of strings instead of a string (1 to 128
  of them, each checked as a single value is), as
  `{"c":"content_type","o":"in","v":["photo","video"]}`. It becomes
  `col IN (?, ?)`, one placeholder per value. A list for any other
  operator, or a string for `in`, is refused ("operator '=' takes one
  value, not a list", "operator 'in' expects a list of values for
  field 'content_type'"), and so is a `v` that is neither ("'v' must
  be a string or a list of strings"). The list pages take it in their
  filters the same way, `{"field":"content_type","op":"in",
  "value":["photo","video"]}`, and echo the list back.

The React list pages (users, groups, files, group_messages,
private_messages) read their rows through the same search, from the
page data (`GET /api/page/<path>`); webapi/README describes them.

## Query parameters

| param    | type   | default | notes |
|----------|--------|---------|-------|
| `search` | string | (empty) | URL-encoded JSON array of conditions (see below). Empty = browse all. |
| `limit`  | int    | `10`    | rows per page, clamped to `1..1000`. |
| `offset` | int    | `0`     | row offset, clamped to `0..500000`. |
| `sort`   | string | (id)    | a **sortable** field key; anything else falls back to the default (`id`). |
| `order`  | string | `desc`  | `asc` or `desc`. |
| `debug`  | `1`    | off     | **admin only**: include the generated SQL, bind values and `EXPLAIN`. |

## The `search` grammar

`search` is a URL-encoded JSON **array** of condition objects, evaluated
left-to-right:

```json
[
  { "c": "is_premium", "o": "=",    "v": "1",     "n": "AND" },
  { "c": "bio",        "o": "LIKE", "v": "admin",  "n": "OR" }
]
```

| key | meaning |
|-----|---------|
| `c` | field **key** from the registry (not a raw column). |
| `o` | operator (see below); must be allowed for that field. |
| `v` | value; bound as a `?` parameter. Omitted for `IS NULL` / `IS NOT NULL`. Max 512 chars. |
| `n` | connector to the **next** condition: `AND` or `OR`. The last condition's `n` is ignored. |

### Connectors and precedence

Conditions form a flat chain. MySQL operator precedence applies: **`AND` binds
tighter than `OR`**, so `A OR B AND C` means `A OR (B AND C)`. Each condition is
parenthesized individually; there is no user-facing grouping in this version.
With `?debug=1` (admin) the response echoes the exact generated SQL.

### Operators

| token | applies to | meaning |
|-------|-----------|---------|
| `=` `!=` | all value fields | equality / inequality |
| `<` `>` `<=` `>=` | int, datetime | comparison |
| `LIKE` | text | SQL `LIKE` with the value bound **verbatim** — a plain term matches literally; add `%`/`_` yourself for wildcards (e.g. `%admin%` to match a substring) |
| `NOT LIKE` | text | negation of `LIKE` |
| `%LIKE%` | text | **contains**: the value is wrapped as `%value%` server-side (the user's own `%`/`_` are escaped, so their text matches literally). The easy "find rows containing this text" operator |
| `NOT %LIKE%` | text | negation of `%LIKE%` (does not contain) |
| `matches` | fulltext | `MATCH(col) AGAINST(value IN BOOLEAN MODE)` — full-text word search (message `text` fields; in the Go service also users' `bio` and groups' `description`). Supports Telegram-style boolean queries: `+must -exclude "a phrase"`. Words shorter than the server's min token length (3) are ignored |
| `IS NULL` `IS NOT NULL` | nullable fields | presence test (no `v`) |

**History / current-username fields** are `EXISTS` fields: `=`/`LIKE`/`%LIKE%`
mean "ever matched" and `!=`/`NOT LIKE`/`NOT %LIKE%` mean "**never** matched"
(`NOT EXISTS`), which is the useful negation — not the misleading "ever had a
value != x". They do not accept `IS NULL`.

Each field advertises the subset of operators it accepts in the `fields` part of
the response, so a client can build its UI from the registry.

## Users field registry

`C` = column field, `E` = EXISTS field.

| key | label | type | kind | operators |
|-----|-------|------|------|-----------|
| `id` | User ID | int | C | `= != < > <= >=` |
| `first_name` | First name | text | C | `= != LIKE NOT LIKE %LIKE% NOT %LIKE%` |
| `last_name` | Last name | text | C | `= != LIKE NOT LIKE %LIKE% NOT %LIKE%` |
| `any_name` | Name (own or contact) | text | C | `LIKE NOT LIKE %LIKE% NOT %LIKE%` |
| `contact_first_name` | Contact first name | text | C | `= != LIKE NOT LIKE %LIKE% NOT %LIKE%` |
| `contact_last_name` | Contact last name | text | C | `= != LIKE NOT LIKE %LIKE% NOT %LIKE%` |
| `is_contact` | Contact | bool | C | `= !=` |
| `username` | Username (current) | text | E | `= LIKE %LIKE%` (+`!=`/`NOT LIKE`/`NOT %LIKE%` = never) |
| `type` | Type | enum | C | `= !=` — values `regular,deleted,bot,unknown` |
| `msg_count` | Messages | int | C | `= != < > <= >=` |
| `has_photo` | Has photo | bool | C | `IS NULL IS NOT NULL` |
| `is_verified` | Verified | bool | C | `= !=` |
| `is_premium` | Premium | bool | C | `= !=` |
| `is_scam` | Scam | bool | C | `= !=` |
| `is_fake` | Fake | bool | C | `= !=` |
| `is_support` | Support | bool | C | `= !=` |
| `created_at` | Created | datetime | C | `= != < > <= >=` |
| `updated_at` | Updated | datetime | C | `= != < > <= >=` |
| `bio` | Bio | text | C | `= != LIKE NOT LIKE %LIKE% NOT %LIKE%`, and `matches` in the Go service |
| `phone` | Phone | text | C | `= != LIKE NOT LIKE %LIKE% NOT %LIKE%` |
| `language` | Language | text | C | `= != LIKE NOT LIKE %LIKE% NOT %LIKE%` |
| `restriction_reason` | Restriction | text | C | `= != LIKE NOT LIKE %LIKE% NOT %LIKE%` |
| `has_sensitive` | Sensitive content | bool | C | `= !=` |
| `restricts_new_chats` | Restricts new chats | bool | C | `= !=` |
| `paid_star_count` | Paid message stars | int | C | `= != < > <= >=` |
| `personal_chat_id` | Personal chat | int | C | `= != IS NULL IS NOT NULL` |
| `emoji_status` | Emoji status | int | C | `IS NULL IS NOT NULL` |
| `hist_username` | Username (ever) | text | E | `= LIKE %LIKE%` (+never) |
| `hist_first_name` | First name (ever) | text | E | `= LIKE %LIKE%` |
| `hist_last_name` | Last name (ever) | text | E | `= LIKE %LIKE%` |
| `hist_bio` | Bio (ever) | text | E | `= LIKE %LIKE%` |
| `hist_phone` | Phone (ever) | text | E | `= LIKE %LIKE%` |

`first_name`/`last_name` are the user's own name. For a contact of the
logging account, the name the account saved them under is
`contact_first_name`/`contact_last_name` (`''` for a user who is not a
contact), shown in the `contact_name` column; `any_name` matches either, so
`any_name %LIKE% moe` finds a user by what they call themselves or by what
the account calls them. Where no own name was ever seen (a contact since
before logging began), `first_name`/`last_name` hold the contact name too.

Column fields on `telegram_user_extra_info` are `COALESCE`'d to their schema default, so a
user whose extra-info row is absent (the daemon deletes all-default rows) still
matches e.g. `bio = ''`. `bio matches` (Go service) searches the bios'
FULLTEXT index; without a `sort` it orders by relevance. Sortable keys:
`id, first_name, type, created_at, updated_at, msg_count, paid_star_count`.

Datetime values accept MySQL-parseable strings (`2024-03-17`,
`2024-03-17 09:00`). Boolean values are `0`/`1`.

## Groups field registry

| key | label | type | kind | operators |
|-----|-------|------|------|-----------|
| `id` | Group ID | int | C | `= != < > <= >=` |
| `title` | Title | text | C | `= != LIKE NOT LIKE %LIKE% NOT %LIKE%` |
| `description` | Description | text | C | `= != LIKE NOT LIKE %LIKE% NOT %LIKE%`, and `matches` in the Go service |
| `type` | Type | enum | C | `= !=` — values `basic_group,supergroup,channel` |
| `msg_count` | Messages | int | C | `= != < > <= >=` |
| `member_count` | Members | int | C | `= != < > <= >=` |
| `has_photo` | Has photo | bool | C | `IS NULL IS NOT NULL` |
| `username` | Username (current) | text | E | `= LIKE %LIKE%` (+`!=`/`NOT LIKE`/`NOT %LIKE%` = never) |
| `created_at` | Created | datetime | C | `= != < > <= >=` |
| `updated_at` | Updated | datetime | C | `= != < > <= >=` |
| `hist_title` | Title (ever) | text | E | `= LIKE %LIKE%` |
| `hist_description` | Description (ever) | text | E | `= LIKE %LIKE%` |
| `hist_username` | Username (ever) | text | E | `= LIKE %LIKE%` (+never) |

Group ids are negative Telegram chat ids. `description matches` (Go service)
searches the descriptions' FULLTEXT index, by relevance without a `sort`.
Sortable keys: `id, title, type, created_at, updated_at, msg_count,
member_count`. `member_count` is Telegram's member count as the daemon
last stored it; a group without one yet matches no comparison. It is a filter and a sort, not a column
of the answer (the groups list page's rows carry it).

## Files field registry

The content-addressed file store. Files have no profile page: on the `/files`
page the id and thumbnail cells link to the tokenized media download
(`/files/<token>`) instead.

| key | label | type | kind | operators |
|-----|-------|------|------|-----------|
| `id` | File ID | int | C | `= != < > <= >=` |
| `file_type` | Type | enum | C | `= !=` — values `photo,video,document,audio,voice,sticker,animation,video_note,thumbnail,unknown` |
| `name` | Name | text | C | `= != LIKE NOT LIKE %LIKE% NOT %LIKE%` — the original Telegram file name (may be empty) |
| `ext` | Extension | text | C | `= != LIKE NOT LIKE %LIKE% NOT %LIKE%` |
| `size` | Size | int | C | `= != < > <= >=` — bytes (displayed human-readable) |
| `hits` | Hits | int | C | `= != < > <= >=` — times this content (SHA-256) was seen |
| `stored` | Stored | bool | C | `= !=` — `0` = the bytes are not in the store (evicted, never fetched, or over the per-file cap), fetchable from Telegram |
| `tg_file_id` | TG file id | text | C | `= != LIKE NOT LIKE %LIKE% NOT %LIKE%` |
| `sha256` | SHA-256 | text | C | `= !=` — full hex digest (upper/lower); matched as `sha256 = UNHEX(?)` so it uses the unique index. Exact only (no prefix); invalid hex matches nothing |
| `created_at` | Created | datetime | C | `= != < > <= >=` |
| `updated_at` | Updated | datetime | C | `= != < > <= >=` |
| `accessed_at` | Last access | datetime | C | `= != < > <= >=` — last use of the file (ingested again, served); eviction takes the least recent first |
| `role` | Role in a message | enum | E | `= != in` — values `cover,live_video,paid,preview,poll,game,invoice,chat_photo,thumbnail,rich,gift,background,preview_media,link_preview` (Go service only, see below) |

`role` is the role the file plays in a message beside being its own file:
a `telegram_message_files.role` value as stored (docs/design/message-types.md
section 4.3; `preview` is reserved and written by nothing yet), or
`link_preview` for a link preview's picture
(`telegram_link_previews.photo_file_id`). `=` and `in` match a file that plays
one of the roles in some message the viewer sees, `!=` one that plays it in
none; a file that is also another message's own file matches too. It is one
or two EXISTS over the tables' file indexes (`idx_message_files_file`,
`idx_link_previews_photo`), joined to the message for a viewer who does not see
every chat, and counts as one EXISTS condition against the limit. Such a viewer
otherwise sees only the files that are the own file of a message it sees; an
`=` or `in` role condition that every row must meet (in a chain of ANDs) shows
it the files playing the role in the messages it sees instead, since a cover or
a link preview's picture is seldom any message's own file. Beside an OR the
role condition does not decide what is visible, and the own file rule stays. On
a page in an indexed order (id, `file_type`, size, hits, `created_at`,
`updated_at`; descending, or id either way) within its first 1000 rows, the
`link_preview` EXISTS is tested row by row as the files are read in that order
(`NO_SEMIJOIN`), since a few percent of all files are previews' pictures; the
count and other pages let the optimizer read every preview. The files page's
rows (FileRow in webapi/openapi.yaml) say the role in `role`: a file that is no
message's own file names the first visible message that carries it in a role,
with `role: {role, message_kind}`, where `message_kind` is that message's
content type; a message's own file has
`role: null`. The Drogon search has no such field.

Sortable keys: `id, file_type, name, ext, size, hits, created_at, updated_at,
accessed_at`.

The table also shows two identifier columns, both searchable (above): the
`tg_file_id` (its full value is truncated in the table — click the cell to see
the full id in a modal) and the `sha256` content digest (an uppercase hex
string, empty for a file whose bytes were never seen). The thumbnail is
display-only. Neither identifier column is sortable; `sha256` searches an exact
digest through the unique index (`= UNHEX(?)`).

## Private messages field registry

`/v1/search/private_messages` over `telegram_private_messages`. Only
index-backed columns are searchable so a query never scans the table.

| key | label | type | operators |
|---|---|---|---|
| `text` | Text | fulltext | `matches` |
| `content_type` | Content type | enum | `= !=` (text,photo,video,document,audio,voice,sticker,animation,service,unknown,video_note,location,venue,contact,dice,game,poll,checklist,story,paid_media,invoice,giveaway,call,rich,widget,expired,unsupported) |
| `chat_id` | User (chat) id | int | `= != < > <= >=` |
| `tdlib_type` | TDLib type | enum (open) | `= != in`, with `chat_id =` only (Go service) |
| `service_type` | Service type | enum (open) | `= != in`, with `chat_id =` only (Go service) |
| `sender_id` | Sender id | int | `= != < > <= >=` |
| `message_id` | Message id | int | `= != < > <= >=` |
| `is_outgoing` | Outgoing | bool | `= !=` |
| `is_forwarded` | Forwarded | bool | `= !=` |
| `deleted` | Deleted | bool | `IS NULL IS NOT NULL` |
| `date` | Sent | int (unix ts) | `= != < > <= >=`, sortable (default) |

Each row shows the peer **user** as an avatar + name + username linking to
their profile, plus direction (out), text, type, forwarded, message id, sent
time and a deleted flag.

## Group messages field registry

`/v1/search/group_messages` over `telegram_group_messages` (2.6M rows), so the
searchable set is limited to indexed columns; the rest are display-only.

| key | label | type | operators |
|---|---|---|---|
| `text` | Text | fulltext | `matches` |
| `content_type` | Content type | enum | `= !=` (same values as above) |
| `chat_id` | Group id | int | `= != < > <= >=` |
| `tdlib_type` | TDLib type | enum (open) | `= != in`, with `chat_id =` only (Go service) |
| `service_type` | Service type | enum (open) | `= != in`, with `chat_id =` only (Go service) |
| `sender_user_id` | Sender user id | int | `= != < > <= >=` |
| `deleted` | Deleted | bool | `IS NULL IS NOT NULL` |
| `date` | Sent | int (unix ts) | `= != < > <= >=`, sortable (default) |

Each row shows the **group** (photo + title → group page) and the **sender**
(photo + name + username → user page) as two clickable avatars, plus type,
text, forwarded, message id, sent time and a deleted flag.

### Message search behaviour

- **Relevance ordering.** When a `text matches …` condition is present and no
  explicit `sort` is given, results are ordered by fulltext relevance (fast:
  the fulltext index yields ordered rows, so `LIMIT` stops early). Ordering a
  broad match by another column would filesort every hit. A relevance sort has
  no id tiebreak, so paging can wobble on exact ties.
- **`party` cell.** Message rows use a `party` display cell — a JSON object
  `{kind: user|group|self|none, id, name, username, photo}` (the `photo` is a
  tokenised `/files/<token>` URL) — rendered as a linked avatar + name.

## Search engine (Go service, optional)

The Go service can answer the `text` field of `group_messages` and
`private_messages`, the `bio` of `users` and the `description` of
`groups` from a search engine (OpenSearch; webapi/README, "Search
engine") instead of MySQL's FULLTEXT indexes. It is optional:
without `WEBAPI_SEARCH_ENGINE_URL` everything on this page stays as it
is, the parameters below are ignored and the answers carry none of the
fields below. The Drogon `/v1/search` never uses it.

### Capabilities

```
GET /api/search/capabilities
```

says what the backend offers, so a client offers those options only
when they work:

```json
{"engine": "opensearch",
 "entities": {
   "group_messages": {"fields": ["text"],
     "options": ["phrase", "proximity", "prefix", "fuzzy", "highlight", "facets"],
     "available": true, "fallback": null},
   "private_messages": { ... },
   "users": {"fields": ["bio"],
     "options": ["phrase", "proximity", "prefix", "fuzzy", "highlight"],
     "available": true, "fallback": null},
   "groups": {"fields": ["description"], ... }}}
```

Without an engine it is `{"engine": null, "entities": {}}`. Only the
entities the viewer may search are listed (`private_messages` not for
a viewer who may not see private chats, `users` not for a member, who
sees no bio). Users and groups offer no `facets`: their hits are in no
chat. `available` is false, with
the reason in `fallback` (`unavailable`, `behind`), while searches go
to MySQL instead.

### The `matches` value

With the engine, `text matches <value>` (and `bio` or `description`
`matches`) reads the value in the syntax
of MySQL's boolean mode, so it means the same on both backends, plus
`~` for typos:

| form | meaning |
|------|---------|
| `word` | rows with it rank higher; with no `+word`, a row needs one of these |
| `+word` / `-word` | a row must / must not have it |
| `word*` | a word starting so (prefix) |
| `"a phrase"` | the words in order, next to each other |
| `"a b" @3` | the words within 3 positions (proximity; MySQL's syntax too) |
| `word~`, `word~1`, `word~2` | the word or one within a few edits (fuzzy; engine only: MySQL, also without an engine, searches the plain word) |

Words are split on Unicode word boundaries, lowercased and folded to
ASCII (an accented letter matches the plain one). A word joined by
punctuation is also split at it, as MySQL splits it, and kept whole:
`kernel` finds `git.kernel.org`, `linux-kernel` and `/usr/src/kernel`,
and `kernel.org` finds `kernel.org` and `git.kernel.org` (its parts
next to each other), where MySQL, reading it as `kernel org`, also
finds rows with either word anywhere. MySQL's `~ < >` before a word
and parentheses are read as spaces; a value with nothing to look for
matches nothing.

### Parameters

| parameter | meaning |
|-----------|---------|
| `fuzzy=1` | every plain word matches with typos, as `word~` |
| `highlight=1` | add `highlights` |
| `facets=1` | add `facets` |
| `engine=mysql` | keep the search on MySQL (`fallback` `asked`) |

### Answer

Beside the fields of [Response](#response), with an engine configured:

- `backend`: `{"used": "engine"|"mysql", "fallback"?, "ignored"?}`.
  `fallback` says why MySQL answered a search with a `text` condition:
  `asked`, `unavailable` (the engine fails or is down; it is then
  skipped for half a minute), `timeout` (a query took longer than
  `WEBAPI_SEARCH_ENGINE_TIMEOUT`), `behind` (the backfill has not
  finished, or the index was not known in step with the archive for
  `WEBAPI_SEARCH_ENGINE_MAX_LAG`), `too_many_hits` (see below).
  `ignored` lists the options asked for that the answer does not give.
- `highlights` (with `highlight=1`): per row, aligned to `rows`, up to
  three fragments of its text (a user's bio, a group's description) with the matched words in
  `<mark>...</mark>` and the rest HTML-escaped; `[]` for a row whose
  text has no match, and always for service messages, whose stored
  text may name what their rendering for the viewer leaves out.
- `facets` (with `facets=1`, when the engine answers the whole
  search of a message entity; for users and groups `ignored`): `[{"chat_id", "name", "count"}]`, the ten chats with the
  most hits, busiest first; facets are cheap, since the engine counts
  them with the page.

The rows, cells, `total` and paging keep their shape: the engine says
which rows match and MySQL reads them, with every other condition and
the request scope applied again.

- A search whose conditions are all `text` or columns the engine's
  documents carry (`chat_id`, the sender, `date`, `content_type`,
  `deleted`, and `is_outgoing` for private messages; `id` for users
  and groups), joined by `AND`, and sorted by relevance or `date` (`id`
  for users and groups), is paged and counted by the engine
  (`total_exact` true).
- Any other search (an `OR`, another column, another sort) gets the
  rows the engine finds for each `text` condition as an id list, and
  MySQL counts and pages as usual. More than 10000 rows for one
  condition cannot be listed: MySQL answers alone (`too_many_hits`).
  `facets` are not given then (`ignored`), since the engine's per-chat
  counts would not match MySQL's total.

The message list pages (`/group_messages`, `/private_messages` and
their page data, MessagesProps) take the same options from their own
query (`fuzzy=1`, `highlight=1`, `facets=1`, `engine=mysql`) for their
search text, and their page data carries `backend`, `highlights` and
`facets` as above, plus `engine`: their entity's entry of the
capabilities, so the page can offer the options without asking for
them. The users and groups pages (UsersProps, GroupsProps) do the same
for a `bio` or `description` `matches` filter, without facets. Without
an engine none of these is present.

### The request scope

The scope is a filter of every engine query, never applied to its
answer afterwards: group messages are limited to every group, all but
an admin's hidden groups (`must_not`) or a member's granted groups
(`terms`), and private messages are searched only by a viewer who sees
private chats (others get 404, as without the engine); bios only by a
viewer who sees users in full (a member sees none, as without the
engine); group descriptions within the same groups as their messages.
Hits, `total`,
`facets` and `highlights` therefore count and show only what the
viewer may see. Signed-out visitors have no advanced search.

## Response

On success the body is a single JSON object. `cols` describes the displayed
columns once, and each entry in `rows` is a **positional array aligned to
`cols`** (api2.php-style — the column keys are not repeated on every row):

```json
{
  "fields": [ { "key": "type", "label": "Type", "type": "enum",
                "sortable": true, "operators": ["=", "!="],
                "enum": "regular,deleted,bot,unknown" }, ... ],
  "cols":   [ { "key": "photo", "label": "",     "type": "photo", "sort": ""           },
              { "key": "id",    "label": "ID",    "type": "id",    "sort": "id"         },
              { "key": "name",  "label": "Name",  "type": "name",  "sort": "first_name" },
              { "key": "msg_count", "label": "Messages", "type": "int", "sort": "msg_count" }, ... ],
  "rows":   [ [ "/files/<token>", 123, "Ada", "adalove", "regular", "42", false, ... ],
              [ "",               456, "(no name)", "", "deleted", "0", false, ... ] ],
  "total":      3029,
  "total_exact": true,
  "limit":      10,
  "offset":     0,
  "max_offset": 500000,
  "sort":       "",
  "order":      "desc"
}
```

- `fields` is the searchable registry (one entry per field): `key`, `label`,
  `type`, `sortable`, the allowed `operators`, and — for enum fields — `enum`
  (the CSV of allowed values). It drives the condition builder.
- `cols` is the ordered display list: `key`, `label`, the render `type` (`photo`,
  `id`, `name`, `username`, `bool`, `int`, `datetime`, `text`, `longtext`) and
  `sort` (the field key to `ORDER BY` when the header is clicked; `""` = not
  sortable).
- `rows[i][j]` is the value for column `cols[j]`. Cell shape by column `type`:
  `id` is a JSON number; `bool` is `true`/`false`; every other type is a string
  (**already HTML-escaped** — insert verbatim). A `photo` cell is a tokenized,
  un-enumerable `/files/<token>` media URL, or `""` when the entity has no photo.
- The **photo, id and name cells link to the entity's detail page**
  (`<base>/<id>`, where `<base>` is `/users` or `/groups`). The photo cell is a
  link whether or not a photo exists — a no-photo cell renders a silhouette
  placeholder that is itself the link.
- `total` is the full count for the current filter (drives pagination); `offset`
  is clamped to `max_offset`.
- `total_exact` says whether `total` is that count or only an **estimate**. The
  count feeds nothing but the pager, so it is never allowed to fail the request:
  - Counting has the same 3-second limit as the page query. When it runs out,
    `total` falls back to an estimate and `total_exact` is `false`.
  - A count that ran out of time is not tried again within requests: the same
    count (same filter, same viewer scope) answers the estimate at once for the
    next ten minutes, while it runs once in the background with a longer limit.
    Once that is done, `total` is its answer (`total_exact` `true`), up to ten
    minutes old and counted again in the background when older.
  - With no conditions, the estimate is the storage engine's row estimate for
    the table (InnoDB's `table_rows`, which can be off by a fair margin).
    `group_messages` — millions of rows — always answers a no-condition request
    this way, without attempting the exact count at all.
  - With conditions there is no cheap estimate, so `total` is only what the
    returned page proves: `offset` + its rows, plus one when the page is full.
    It grows as you page forward; treat it as "at least".
  The pages render an estimated total as "about N results".
- When the **page** query itself runs out of time, the response is the usual
  error envelope (`The search took too long and was stopped. …`) rather than a
  500.

### Debug (admin, `?debug=1`)

```json
"debug": {
  "sql":       "SELECT ... FROM telegram_users u LEFT JOIN ... WHERE (u.type = ?) OR (u.is_scam = ?) ORDER BY u.id DESC LIMIT 1 OFFSET 0",
  "count_sql": "SELECT ... COUNT(*) ...",
  "bind":      ["bot", "1"],
  "explain":   [ { "id": "1", "select_type": "SIMPLE", ... } ]
}
```

`bind` values are HTML-escaped (they echo user input).

## Errors

Bad input returns **HTTP 400** with `{"error": "<message>"}` — e.g. unknown
field, disallowed operator, malformed JSON, wrong value type, too many
conditions. The rewritten `/users` page treats a bad search as browse-all and
shows the message as a notice.

## Limits

| limit | value |
|-------|-------|
| conditions per query | 16 |
| `EXISTS` (history) conditions per query | 4 |
| value length | 512 chars |
| `search` JSON size | 8 KB |
| `limit` | 1000 (default 10) |
| `offset` | 500000 |
| statement timeout | 3 s (`MAX_EXECUTION_TIME`) |

A non-sargable predicate (text `LIKE`, flag equality) scans the full users
table, and `COUNT(*)` runs the same predicate — expect multi-hundred-ms queries
on large filters; the statement timeout caps the worst case. Deep `offset` is
`O(offset)`.

## Examples

Premium users:
```
/v1/search/users?search=[{"c":"is_premium","o":"=","v":"1","n":"AND"}]
```

Bots or scam-flagged accounts (note precedence — this is `bot OR (scam)`):
```
/v1/search/users?search=[{"c":"type","o":"=","v":"bot","n":"OR"},{"c":"is_scam","o":"=","v":"1","n":"AND"}]
```

Users whose bio ever contained "admin", sorted by creation date:
```
/v1/search/users?sort=created_at&order=desc&search=[{"c":"hist_bio","o":"LIKE","v":"admin","n":"AND"}]
```

Users created on or after a date:
```
/v1/search/users?search=[{"c":"created_at","o":">=","v":"2026-07-01","n":"AND"}]
```

Users with no linked personal chat:
```
/v1/search/users?search=[{"c":"personal_chat_id","o":"IS NULL","n":"AND"}]
```

(URL-encode the `search` value in real requests.)
