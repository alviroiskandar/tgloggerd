-- discord_endpoints interns the distinct Discord webhook URLs that forwarded
-- messages were posted to, so discord_sent_messages can reference one by a
-- small id instead of repeating the ~120-byte URL on every row.
--
-- It is deliberately NOT the same as discord_webhooks (the admin-managed
-- integration config): that table is edited and deleted from the web UI, but a
-- sent message's endpoint is a historical fact -- the exact URL its Discord
-- message physically lives at, needed to build the edit/delete PATCH URL. So
-- this dictionary is owned by the daemon and is append-only: rows are interned
-- on first use and never updated or deleted, keeping the reference immutable
-- and decoupled from config churn.
CREATE TABLE discord_endpoints (
	id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,

	webhook_url VARCHAR(512)    NOT NULL
	                            COMMENT 'Discord webhook URL a forwarded message was posted to.',

	created_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,

	PRIMARY KEY (id),
	UNIQUE KEY uq_discord_endpoints_url (webhook_url)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Interned Discord webhook URLs referenced by discord_sent_messages.';

-- Discord messages the forwarder has posted, keyed by their source Telegram
-- message, so a later Telegram edit can be applied to the Discord message via
-- the webhook edit endpoint (PATCH /webhooks/<id>/<token>/messages/<id>).
--
-- One row per (source message, destination endpoint, part): a forwarded message
-- may produce a "text" part (caption/quote) and a separate "media" part. Rows
-- are kept indefinitely: a Telegram "delete for everyone" (and channel-post
-- edits) have no time limit, so any age of edit/delete must still resolve. The
-- posted content is NOT stored here -- an edit rebuilds it from the live event
-- and a delete re-derives it from the source message row (group_messages/
-- private_messages), so this table stays a compact id map.
CREATE TABLE discord_sent_messages (
	id                 BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,

	chat_id            BIGINT          NOT NULL
	                                   COMMENT 'Telegram chat_id of the source message.',
	message_id         BIGINT          NOT NULL
	                                   COMMENT 'Telegram server message id of the source message.',
	endpoint_id        BIGINT UNSIGNED NOT NULL
	                                   COMMENT 'FK to discord_endpoints.id: the webhook this was posted to.',
	discord_message_id VARCHAR(32)     NOT NULL
	                                   COMMENT 'Discord message snowflake, for editing.',
	kind               ENUM('text', 'media') NOT NULL DEFAULT 'text'
	                                   COMMENT 'Which forwarded part this row is.',

	created_at         DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,

	PRIMARY KEY (id),
	KEY idx_dsm_msg (chat_id, message_id),
	KEY idx_dsm_endpoint (endpoint_id),
	CONSTRAINT fk_dsm_endpoint FOREIGN KEY (endpoint_id)
		REFERENCES discord_endpoints (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Forwarded Discord messages, so Telegram edits can be applied.';
