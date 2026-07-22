-- Rename discord_sent_messages columns to name each id by the platform it
-- belongs to. The table is a discord_* table, so its own Discord id needs no
-- prefix, while the source-message ids point at the external Telegram side:
--   chat_id            -> telegram_chat_id
--   message_id         -> telegram_message_id
--   discord_message_id -> message_id
--
-- Pure renames (data, indexes and the endpoint_id FK are preserved; InnoDB
-- rewrites idx_dsm_msg to the new column names automatically). Order matters:
-- free the `message_id` name before reusing it for discord_message_id.
ALTER TABLE discord_sent_messages RENAME COLUMN chat_id TO telegram_chat_id;
ALTER TABLE discord_sent_messages RENAME COLUMN message_id TO telegram_message_id;
ALTER TABLE discord_sent_messages RENAME COLUMN discord_message_id TO message_id;
