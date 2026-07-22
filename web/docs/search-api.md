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

`{entity}` is `users`, `groups` or `files` — each shares the same grammar with
its own field registry (an unknown entity returns **404**). Requires a logged-in
session (the shared `AuthFilter`); an unauthenticated request is redirected (302)
to `/login`, exactly like the other `/v1` routes.

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

## Groups field registry

| key | label | type | kind | operators |
|-----|-------|------|------|-----------|
| `id` | Group ID | int | C | `= != < > <= >=` |
| `title` | Title | text | C | `= != LIKE NOT LIKE` |
| `description` | Description | text | C | `= != LIKE NOT LIKE` |
| `type` | Type | enum | C | `= !=` — values `basic_group,supergroup,channel` |
| `msg_count` | Messages | int | C | `= != < > <= >=` |
| `has_photo` | Has photo | bool | C | `IS NULL IS NOT NULL` |
| `username` | Username (current) | text | E | `= LIKE` (+`!=`/`NOT LIKE` = never) |
| `created_at` | Created | datetime | C | `= != < > <= >=` |
| `updated_at` | Updated | datetime | C | `= != < > <= >=` |
| `hist_title` | Title (ever) | text | E | `= LIKE` |
| `hist_description` | Description (ever) | text | E | `= LIKE` |
| `hist_username` | Username (ever) | text | E | `= LIKE` (+never) |

Group ids are negative Telegram chat ids. Sortable keys: `id, title, type,
created_at, updated_at, msg_count`.

## Files field registry

The content-addressed file store. Files have no profile page: on the `/files`
page the id and thumbnail cells link to the tokenized media download
(`/files/<token>`) instead.

| key | label | type | kind | operators |
|-----|-------|------|------|-----------|
| `id` | File ID | int | C | `= != < > <= >=` |
| `file_type` | Type | enum | C | `= !=` — values `photo,video,document,audio,voice,sticker,animation,unknown` |
| `name` | Name | text | C | `= != LIKE NOT LIKE` — the original Telegram file name (may be empty) |
| `ext` | Extension | text | C | `= != LIKE NOT LIKE` |
| `size` | Size | int | C | `= != < > <= >=` — bytes (displayed human-readable) |
| `hits` | Hits | int | C | `= != < > <= >=` — times this content (SHA-256) was seen |
| `stored` | Stored | bool | C | `= !=` — `0` = metadata-only (too large), re-downloadable |
| `tg_file_id` | TG file id | text | C | `= != LIKE NOT LIKE` |
| `sha256` | SHA-256 | text | C | `= !=` — full hex digest (upper/lower); matched as `sha256 = UNHEX(?)` so it uses the unique index. Exact only (no prefix); invalid hex matches nothing |
| `created_at` | Created | datetime | C | `= != < > <= >=` |
| `updated_at` | Updated | datetime | C | `= != < > <= >=` |

Sortable keys: `id, file_type, name, ext, size, hits, created_at`.

The table also shows two identifier columns, both searchable (above): the
`tg_file_id` (its full value is truncated in the table — click the cell to see
the full id in a modal) and the `sha256` content digest (an uppercase hex
string). The thumbnail is display-only. Neither identifier column is sortable;
`sha256` searches an exact digest through the unique index (`= UNHEX(?)`).

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
