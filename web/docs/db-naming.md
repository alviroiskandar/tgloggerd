# Database table naming convention

tgloggerd is growing from a Telegram-only logger into a hub that integrates
**multiple messaging platforms**. To keep the schema navigable as more
platforms are added, every table that stores data belonging to a specific
platform is **prefixed with that platform's name**:

| Prefix      | Owner / meaning                                              |
|-------------|-------------------------------------------------------------|
| `telegram_` | Telegram data: users, groups, messages, files, history, …   |
| `discord_`  | Discord data: guilds, channels, users, messages, attachments |

So the table that stores Telegram users is `telegram_users`, the group-message
edit history is `telegram_group_message_edits`, the Discord messages log is
`discord_messages`, and so on.

## The rule

- **Any new table that models data from a messaging platform MUST be prefixed
  with that platform's short, lowercase slug + `_`.** Adding, say, Slack support
  means `slack_users`, `slack_messages`, `slack_channels`, … — never a bare
  `users` or a `slack`-less name.
- Pick one slug per platform and use it consistently (`telegram_`, `discord_`,
  `slack_`, `whatsapp_`, …). Keep it the same everywhere.
- History / edit / auxiliary tables inherit the prefix of the entity they belong
  to (`telegram_user_hist_bio`, `telegram_group_admin_hist`).

## Cross-platform tables carry BOTH platform names

tgloggerd bridges platforms in both directions, so some tables do not belong to
one platform — they describe a *relationship* between two. Those are prefixed
with **both** slugs, **source first, then destination**:

| Table                            | Meaning                                     |
|----------------------------------|---------------------------------------------|
| `telegram_discord_webhooks`      | config: mirror a Telegram chat → Discord     |
| `telegram_discord_sent_messages` | what that forwarder posted into Discord      |
| `discord_telegram_routes`        | config: mirror a Discord channel → Telegram  |
| `discord_telegram_sent_messages` | what that forwarder sent into Telegram       |

A single prefix on a bridge table is a bug waiting to happen. `discord_webhooks`
(the old name of `telegram_discord_webhooks`) read as "Discord-only data" even
though every row is keyed by a *Telegram* chat, and it left no room to name the
reverse direction once one existed. Both were renamed in migration `000020`.

The direction is part of the name, not an afterthought: `telegram_discord_*` and
`discord_telegram_*` are different tables with different owners, and the prefix
order is what tells them apart. Extending to a third platform follows the same
shape — `slack_telegram_routes`, `telegram_slack_sent_messages`.

**A table is only cross-platform if it genuinely relates two platforms.**
`discord_endpoints` interns Discord webhook URLs and says nothing about
Telegram, so it keeps a single prefix. `telegram_bots` holds Telegram bot
credentials; it is *used* by the Discord bridge, but the data itself is
Telegram's, so it too keeps a single prefix.

## What is *not* prefixed

- **Framework / bookkeeping tables that are not platform data.** `schema_migrations`
  is golang-migrate's own state and keeps its fixed name.
- **The web app's own database.** The web UI has a separate database whose tables
  use a `web_` prefix (`web_users`, `web_audit`) — that prefix denotes the
  *component* (the web app's own accounts/audit), not a messaging platform. Those
  are unrelated to the platform tables the daemon owns.

## This is a schema-only convention — the HTTP surface does not change

The prefix lives in the database. The public-facing names stay platform-neutral
and stable:

- **Routes** keep their short names: `/users`, `/groups/{id}`, `/files/<token>`.
- **Search-API entity keys** stay bare: `/v1/search/users`, `entity == "users"`.
- **JSON response keys** stay bare: the dashboard stats return
  `{"users": …, "groups": …, "private_messages": …}` even though they are counted
  from `telegram_users`, `telegram_groups`, `telegram_private_messages`.

When you rename or add a table, update only the SQL that references it
(`FROM`/`JOIN`/`INTO`/`UPDATE`, backticked identifiers, and table-name arguments
passed to the query helpers) — never routes, entity keys, result-set aliases, or
JSON keys.

## Where the tables live

- `migrations/` — the daemon owns the platform schema (`telegram_*`, `discord_*`).
  Renames are done with `ALTER TABLE … RENAME TO …` so existing rows are
  preserved (see `000015_prefix_telegram_tables` and `000020_rename_bridge_tables`).
- `web/migrations/` — the web app's own `web_*` tables.
