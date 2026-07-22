-- discord_webhooks cleanup:
--  * Drop chat_title: it was a denormalized snapshot of the chat's name taken
--    at save time. The admin UI now resolves the current title by joining
--    telegram_groups/telegram_users on the chat id, so the copy (which went
--    stale on a rename) is redundant.
--  * Prefix the source-chat columns with telegram_ (they point at the external
--    Telegram side; the webhook_url is the Discord side and stays bare):
--      chat_id   -> telegram_chat_id
--      chat_type -> telegram_chat_type
--
-- Pure schema changes; the 3 configured rows are preserved and InnoDB rewrites
-- idx_discord_webhooks_enabled_chat to the new column name.
ALTER TABLE discord_webhooks DROP COLUMN chat_title;
ALTER TABLE discord_webhooks RENAME COLUMN chat_id TO telegram_chat_id;
ALTER TABLE discord_webhooks RENAME COLUMN chat_type TO telegram_chat_type;
