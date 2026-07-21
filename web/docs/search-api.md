# Advanced Search API

A read-only JSON API for querying the logged entities with multi-condition
filters, JOINs and history lookups. It backs the `/users` page (and, later,
`/groups` and `/files`), but can be called directly.

Modeled on a flat `[{c,o,v,n}]` condition grammar. Every SQL token except the
bound `?` values comes from a server-side allowlist (the per-entity **field
registry**), so the endpoint is injection-safe by construction.

## Endpoint

```
GET /v1/search/users
```

Requires a logged-in session (the shared `AuthFilter`); an unauthenticated
request is redirected (302) to `/login`, exactly like the other `/v1` routes.

> `GET /v1/search/groups` and `GET /v1/search/files` are planned and will share
> the same grammar with their own field registries.

## Query parameters

| param    | type   | default | notes |
|----------|--------|---------|-------|
| `search` | string | (empty) | URL-encoded JSON array of conditions (see below). Empty = browse all. |
| `limit`  | int    | `50`    | rows per page, clamped to `1..100`. |
| `offset` | int    | `0`     | row offset, clamped to `0..50000`. |
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
| `IS NULL` `IS NOT NULL` | nullable fields | presence test (no `v`) |

**History / current-username fields** are `EXISTS` fields: `=`/`LIKE` mean
"ever matched" and `!=`/`NOT LIKE` mean "**never** matched" (`NOT EXISTS`),
which is the useful negation — not the misleading "ever had a value != x". They
do not accept `IS NULL`.

Each field advertises the subset of operators it accepts in the `fields` part of
the response, so a client can build its UI from the registry.

## Users field registry

`C` = column field, `E` = EXISTS field.

| key | label | type | kind | operators |
|-----|-------|------|------|-----------|
| `id` | User ID | int | C | `= != < > <= >=` |
| `first_name` | First name | text | C | `= != LIKE NOT LIKE` |
| `last_name` | Last name | text | C | `= != LIKE NOT LIKE` |
| `username` | Username (current) | text | E | `= LIKE` (+`!=`/`NOT LIKE` = never) |
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
| `bio` | Bio | text | C | `= != LIKE NOT LIKE` |
| `phone` | Phone | text | C | `= != LIKE NOT LIKE` |
| `language` | Language | text | C | `= != LIKE NOT LIKE` |
| `restriction_reason` | Restriction | text | C | `= != LIKE NOT LIKE` |
| `has_sensitive` | Sensitive content | bool | C | `= !=` |
| `restricts_new_chats` | Restricts new chats | bool | C | `= !=` |
| `paid_star_count` | Paid message stars | int | C | `= != < > <= >=` |
| `personal_chat_id` | Personal chat | int | C | `= != IS NULL IS NOT NULL` |
| `emoji_status` | Emoji status | int | C | `IS NULL IS NOT NULL` |
| `hist_username` | Username (ever) | text | E | `= LIKE` (+never) |
| `hist_first_name` | First name (ever) | text | E | `= LIKE` |
| `hist_last_name` | Last name (ever) | text | E | `= LIKE` |
| `hist_bio` | Bio (ever) | text | E | `= LIKE` |
| `hist_phone` | Phone (ever) | text | E | `= LIKE` |

Column fields on `user_extra_info` are `COALESCE`'d to their schema default, so a
user whose extra-info row is absent (the daemon deletes all-default rows) still
matches e.g. `bio = ''`. Sortable keys: `id, first_name, type, created_at,
updated_at, msg_count, paid_star_count`.

Datetime values accept MySQL-parseable strings (`2024-03-17`,
`2024-03-17 09:00`). Boolean values are `0`/`1`.

## Response

```json
{
  "fields":  [ { "key": "...", "label": "...", "type": "...",
                 "sortable": true, "operators": ["=", "!=", ...],
                 "enum": "regular,deleted,bot,unknown" } ],
  "columns": [ { "key": "id", "label": "User ID" }, ... ],
  "rows":    [ { "id": 123, "name": "...", "username": "...", "type": "bot",
                 "is_premium": false, ..., "created_at": "2026-07-19 12:00:00",
                 "photo_file_id": 176014,
                 "_photo_url": "/files/<token>", "_href": "/users/123" } ],
  "total":   3029,
  "limit":   50, "offset": 0, "sort": "", "order": "desc"
}
```

- All string values in `rows` are **already HTML-escaped**; insert them verbatim.
- `_photo_url` is the tokenized, un-enumerable media URL (the client cannot mint
  it). `_href` is the entity's detail page.
- `columns` lists the registry's default display fields; the rows contain more
  keys than that (e.g. individual flags) for rich rendering.
- `total` is the full count for the current filter (drives pagination).

### Debug (admin, `?debug=1`)

```json
"debug": {
  "sql":       "SELECT ... FROM users u LEFT JOIN ... WHERE (u.type = ?) OR (u.is_scam = ?) ORDER BY u.id DESC LIMIT 1 OFFSET 0",
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
| `limit` | 100 (default 50) |
| `offset` | 50000 |
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
