-- Revert the discord_sent_messages column renames (see the .up migration).
-- Order matters: free the `message_id` name before reusing it.
ALTER TABLE discord_sent_messages RENAME COLUMN message_id TO discord_message_id;
ALTER TABLE discord_sent_messages RENAME COLUMN telegram_message_id TO message_id;
ALTER TABLE discord_sent_messages RENAME COLUMN telegram_chat_id TO chat_id;
