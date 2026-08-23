-- Revert to the single-platform names (see the .up migration). As there, the
-- web user's grant on the integrations table does not follow the rename and
-- must be re-issued as root:
--
--   GRANT INSERT, UPDATE, DELETE ON `tgloggerd`.discord_webhooks
--       TO 'web_ro'@'%';
--   FLUSH PRIVILEGES;
ALTER TABLE telegram_discord_webhooks      RENAME TO discord_webhooks;
ALTER TABLE telegram_discord_sent_messages RENAME TO discord_sent_messages;
