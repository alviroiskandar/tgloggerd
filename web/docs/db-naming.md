# Database table naming convention

tgloggerd is growing from a Telegram-only logger into a hub that integrates
**multiple messaging platforms**. To keep the schema navigable as more
platforms are added, every table that stores data belonging to a specific
platform is **prefixed with that platform's name**:

| Prefix      | Owner / meaning                                              |
|-------------|-------------------------------------------------------------|
| `telegram_` | Telegram data: users, groups, messages, files, history, …   |
| `discord_`  | Discord integration: webhooks, forwarded-message tracking   |

So the table that stores Telegram users is `telegram_users`, the group-message
edit history is `telegram_group_message_edits`, the Discord webhook config is
`discord_webhooks`, and so on.

## The rule

- **Any new table that models data from a messaging platform MUST be prefixed
  with that platform's short, lowercase slug + `_`.** Adding, say, Slack support
  means `slack_users`, `slack_messages`, `slack_channels`, … — never a bare
  `users` or a `slack`-less name.
- Pick one slug per platform and use it consistently (`telegram_`, `discord_`,
  `slack_`, `whatsapp_`, …). Keep it the same everywhere.
- History / edit / auxiliary tables inherit the prefix of the entity they belong
  to (`telegram_user_hist_bio`, `telegram_group_admin_hist`).

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
  preserved (see `000015_prefix_telegram_tables`).
- `web/migrations/` — the web app's own `web_*` tables.
