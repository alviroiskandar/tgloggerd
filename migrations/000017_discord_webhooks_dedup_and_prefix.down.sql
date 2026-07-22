-- Revert the discord_webhooks changes (see the .up migration). The dropped
-- chat_title cannot be restored; re-add it empty so the schema round-trips.
ALTER TABLE discord_webhooks RENAME COLUMN telegram_chat_type TO chat_type;
ALTER TABLE discord_webhooks RENAME COLUMN telegram_chat_id TO chat_id;
ALTER TABLE discord_webhooks ADD COLUMN chat_title VARCHAR(255) NOT NULL DEFAULT ''
	COMMENT 'Cached chat label shown in the admin UI (denormalized at save time).'
	AFTER chat_type;
