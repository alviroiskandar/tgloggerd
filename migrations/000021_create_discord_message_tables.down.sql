-- Drop in reverse dependency order: the two FK children first, then the
-- tables they reference, then the standalone entity tables.
ALTER TABLE telegram_discord_sent_messages
	DROP KEY idx_dsm_discord_msg;

DROP TABLE IF EXISTS discord_telegram_sent_messages;
DROP TABLE IF EXISTS discord_telegram_routes;
DROP TABLE IF EXISTS telegram_bots;
DROP TABLE IF EXISTS discord_attachments;
DROP TABLE IF EXISTS discord_messages;
DROP TABLE IF EXISTS discord_users;
DROP TABLE IF EXISTS discord_channels;
DROP TABLE IF EXISTS discord_guilds;
