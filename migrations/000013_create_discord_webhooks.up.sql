-- Discord webhook integrations: forward every new message in a given Telegram
-- chat to a Discord channel via that channel's incoming webhook.
--
-- `chat_id` is the Telegram chat_id to mirror -- the same value stored in
-- group_messages.chat_id / private_messages.chat_id, and globally unique across
-- all chats (negative for groups/channels, positive for a private chat, where
-- it equals the peer user's id). The daemon loads the enabled rows and forwards
-- matching messages; the web admin UI manages the rows.
CREATE TABLE discord_webhooks (
	id           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,

	chat_id      BIGINT          NOT NULL
	                             COMMENT 'Telegram chat_id to forward from (negative = group/channel, positive = private/user).',
	chat_type    ENUM('private', 'group') NOT NULL
	                             COMMENT 'private = user DM; group = basic group / supergroup / channel.',
	chat_title   VARCHAR(255)    NOT NULL DEFAULT ''
	                             COMMENT 'Cached chat label shown in the admin UI (denormalized at save time).',

	webhook_url  VARCHAR(512)    NOT NULL
	                             COMMENT 'Discord webhook URL forwarded messages are POSTed to.',
	enabled      TINYINT(1)      NOT NULL DEFAULT 1
	                             COMMENT 'Forwarding is active only while 1.',

	created_at   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',
	updated_at   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP
	                             ON UPDATE CURRENT_TIMESTAMP COMMENT 'Last update time.',

	PRIMARY KEY (id),
	-- The daemon loads enabled rows and matches incoming messages by chat_id.
	KEY idx_discord_webhooks_enabled_chat (enabled, chat_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Discord webhook integrations: mirror a Telegram chat to a Discord channel.';
