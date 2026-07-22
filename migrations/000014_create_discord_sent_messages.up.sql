-- Discord messages the forwarder has posted, keyed by their source Telegram
-- message, so a later Telegram edit can be applied to the Discord message via
-- the webhook edit endpoint (PATCH /webhooks/<id>/<token>/messages/<id>).
--
-- One row per (source message, destination webhook, part): a forwarded message
-- may produce a "text" part (caption/quote) and a separate "media" part. Rows
-- are pruned after a retention window (edits rarely happen long after posting).
CREATE TABLE discord_sent_messages (
	id                 BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,

	chat_id            BIGINT          NOT NULL
	                                   COMMENT 'Telegram chat_id of the source message.',
	message_id         BIGINT          NOT NULL
	                                   COMMENT 'Telegram server message id of the source message.',
	webhook_url        VARCHAR(512)    NOT NULL
	                                   COMMENT 'Destination webhook (to build the edit URL).',
	discord_message_id VARCHAR(32)     NOT NULL
	                                   COMMENT 'Discord message snowflake, for editing.',
	kind               ENUM('text', 'media') NOT NULL DEFAULT 'text'
	                                   COMMENT 'Which forwarded part this row is.',
	content            TEXT            NULL
	                                   COMMENT 'Content posted (kept current on edit), so a delete can prepend "(Deleted)".',

	created_at         DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,

	PRIMARY KEY (id),
	KEY idx_dsm_msg (chat_id, message_id),
	KEY idx_dsm_created (created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Forwarded Discord messages, so Telegram edits can be applied.';
