# Discord → Telegram routes (`/routes`)

Admin page and JSON endpoints for managing the routes `discordd` reads: which
Discord channel forwards to which Telegram chat, and through which bot.

This is the mirror of [`/integrations`](../src/controllers/DiscordController.hpp),
which manages the opposite direction (Telegram → Discord, via webhooks). The two
pages are siblings and follow the same conventions; the differences below are
deliberate, not drift.

Every endpoint requires an authenticated **admin** — `AuthFilter` then
`AdminFilter` — because these rows hold bot credentials.

## Secrets: how the bot token is handled

`telegram_bots.token` is a Telegram bot token. Whoever holds it can read and
send as that bot, in every chat the bot is in. It is therefore treated as
write-only:

- **It is never SELECTed for display.** `dao::routes::listBots()` and
  `dao::routes::list()` do not name the `token` column at all.
- **It never appears in a response, page, or DOM attribute.** A bot is
  identified by `bot_label`, built from its Telegram **user id** and `@username`
  — the id half of a token (`<bot_user_id>:<secret>`) is not secret, and we
  already store it separately once `discordd` logs the bot in.
- **Editing a route never round-trips the token.** The form's token field is
  always blank; leaving it blank keeps the currently selected bot. Supplying a
  token means "use this bot instead", and interns it.
- **Tokens are interned, not per-route.** `telegram_bots.token` is `UNIQUE`, so
  re-adding the same bot reuses its row and several routes share one credential.

This deliberately departs from `/integrations`, which returns `webhook_url` in
full and prints it into the page. A webhook URL is scoped to one channel and is
further constrained there by a host allowlist; a bot token is a whole account,
so the same treatment would not be acceptable.

## Endpoints

| Method | Path             | Purpose |
|--------|------------------|---------|
| GET    | `/routes`        | SSR admin page |
| POST   | `/routes/save`   | Create or update a route |
| POST   | `/routes/delete` | Delete a route |
| GET    | `/routes/chats`  | select2 Telegram chat search |

Request bodies are **`application/x-www-form-urlencoded`**, not JSON — matching
`/integrations`. Every mutating POST carries `csrf`, whose value the page
exposes as `data-csrf` on the page root.

### `POST /routes/save`

| Param | Required | Meaning |
|-------|----------|---------|
| `id` | no | Route id; empty creates a new route |
| `discord_channel_id` | yes | Discord channel snowflake, digits only |
| `chat_id` | yes | Telegram chat id (negative for a group/channel) |
| `telegram_bot_id` | conditionally | Existing `telegram_bots.id` |
| `bot_token` | conditionally | New bot token; wins over `telegram_bot_id` |
| `enabled` | no | `1` to enable |
| `csrf` | yes | Session CSRF token |

One of `telegram_bot_id` or `bot_token` must be supplied.

### `POST /routes/delete`

`id` and `csrf`. Refused with an explanation when the route has already
forwarded messages — see *Deleting a used route* below.

### `GET /routes/chats`

`q` (search term) and optional `limit` (1–50, default 20). Returns select2's
shape, reusing `dao::discord::searchChats` so both pages resolve chats
identically:

```json
{ "results": [ { "id": -1001347566306, "text": "…", "type": "group", "title": "…" } ] }
```

## Responses

Success is `{"ok":true}`. Failure is `{"ok":false,"error":"…"}` with a 4xx/5xx
status. The client shows `error` verbatim, so the strings are written to be read
by an operator.

Unlike `/integrations`, the handlers here wrap their database calls in
`try`/`catch` and return the error envelope on failure. Without that, a missing
`GRANT` (see below) surfaces as an HTML 500 into a caller that is parsing JSON.

### Validation, and why each check exists

| Error | Cause |
|---|---|
| `Enter the Discord channel ID …` | Non-numeric or empty channel id. Pasted by hand, so a typo must be named rather than silently becoming `0`. |
| `That chat is not in the log yet …` | The chat is not in `telegram_groups`/`telegram_users`, so the logger has never seen it. |
| `That Discord channel already forwards to that Telegram chat.` | Would violate `uq_dtr_channel_chat`. Checked first so the duplicate reads as a sentence. |
| `That does not look like a Telegram bot token …` | Token is not `<digits>:<secret>`. Shape only — whether it *works* is discovered when `discordd` logs it in. |
| `Choose an existing bot, or paste a bot token to add one.` | Neither `telegram_bot_id` nor `bot_token` given. |
| `This route has forwarded N message(s) …` | See below. |
| `Your session expired. Please reload.` | CSRF check failed (403). |

## Deleting a used route

`discord_telegram_sent_messages.route_id` references
`discord_telegram_routes(id)` under the default **RESTRICT**, so deleting a
route that has ever forwarded a message fails at the database. Rather than
letting a foreign-key error escape, `/routes/delete` counts the referencing rows
first and refuses with a count and the alternative: **disable** the route.

That is the right default. Those rows are what map a Discord message to the
Telegram message it became; cascading the delete would destroy the history that
makes replies and edits resolve.

## Grants

The web app reaches the logger's schema through the read-only `web_ro` user, so
this page needs DML on exactly two more tables:

```sql
GRANT INSERT, UPDATE, DELETE ON `tgloggerd`.discord_telegram_routes TO 'web_ro'@'%';
GRANT INSERT, UPDATE, DELETE ON `tgloggerd`.telegram_bots TO 'web_ro'@'%';
FLUSH PRIVILEGES;
```

`docker/mysql/init/10-web-users.sh` issues these, but that hook only runs on a
**fresh** MySQL data directory. On an existing deployment they must be applied
once as root; until then the page lists routes but every save and delete fails.

## Propagation

`discordd` re-reads the route table every `DISCORDD_ROUTE_REFRESH_SECS`
(default 30, floor 5), so changes take effect without a restart. A route is live
only when both `discord_telegram_routes.enabled` and `telegram_bots.enabled` are
set, which is why the page distinguishes *Disabled* from *Bot disabled*.

One ordering caveat: a **newly added bot** has `bot_user_id = 0` until `discordd`
logs it in, and the Telegram → Discord forwarder's loop guard matches on that id.
So in the first seconds after adding a route through this page, a message it
forwards may still be echoed back into Discord once. It settles on the next
refresh.

## Why no Discord channel picker

The Telegram side has a select2 search because the logger has a populated list
of chats. There is no equivalent for Discord: `discordd` only writes
`discord_channels` for channels that **already** have a route, so a picker would
be empty exactly when it is needed. The channel is therefore entered as an id
(Discord → Developer Mode → Copy Channel ID), and shown as an id in the table.
