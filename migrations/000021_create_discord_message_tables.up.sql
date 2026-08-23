-- Discord message logging, and the routing table that drives Discord ->
-- Telegram forwarding (see src/discordd).
--
-- This is the mirror image of the existing Telegram -> Discord path: that one
-- is configured in telegram_discord_webhooks and records what it posted in
-- telegram_discord_sent_messages. Together the two directions make a bridge,
-- which is why the echo-suppression columns below matter -- see
-- discord_messages.webhook_id.
--
-- Table naming follows the convention introduced in migration 000020 (see
-- web/docs/db-naming.md): a table describing one platform carries that
-- platform's prefix, and a table relating two platforms carries both, source
-- first. So the Discord -> Telegram direction is discord_telegram_routes and
-- discord_telegram_sent_messages, exactly mirroring telegram_discord_* above.
--
-- Every Discord id is a snowflake. Discord serialises them as JSON *strings*
-- because they do not survive a double, but they are unsigned 64-bit integers,
-- so they are stored as BIGINT UNSIGNED. A snowflake also encodes its own
-- creation time: (id >> 22) + 1420070400000 milliseconds since the Unix epoch.

-- ---------------------------------------------------------------------------
-- Entities observed on the Discord side.
-- ---------------------------------------------------------------------------
CREATE TABLE discord_guilds (
	id          BIGINT UNSIGNED NOT NULL
	                            COMMENT 'Discord guild (server) snowflake.',
	name        VARCHAR(255)    NOT NULL DEFAULT '',
	icon        VARCHAR(255)    NOT NULL DEFAULT ''
	                            COMMENT 'Icon hash, not a URL.',

	first_seen  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
	updated_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP
	                            ON UPDATE CURRENT_TIMESTAMP,

	PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Discord guilds seen by the discordd bot.';

CREATE TABLE discord_channels (
	id          BIGINT UNSIGNED NOT NULL
	                            COMMENT 'Discord channel snowflake.',
	guild_id    BIGINT UNSIGNED NOT NULL DEFAULT 0
	                            COMMENT 'Owning guild; 0 for a DM.',
	name        VARCHAR(255)    NOT NULL DEFAULT '',
	type        INT             NOT NULL DEFAULT 0
	                            COMMENT 'Discord channel type enum (0 = guild text).',

	first_seen  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
	updated_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP
	                            ON UPDATE CURRENT_TIMESTAMP,

	PRIMARY KEY (id),
	KEY idx_discord_channels_guild (guild_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Discord channels seen by the discordd bot.';

CREATE TABLE discord_users (
	id            BIGINT UNSIGNED NOT NULL
	                              COMMENT 'Discord user snowflake. For a webhook-authored message this is the webhook id.',
	username      VARCHAR(255)    NOT NULL DEFAULT '',
	global_name   VARCHAR(255)    NOT NULL DEFAULT ''
	                              COMMENT 'Display name; empty when unset.',
	discriminator VARCHAR(8)      NOT NULL DEFAULT ''
	                              COMMENT '"0" for accounts migrated to the new username system.',
	avatar        VARCHAR(255)    NOT NULL DEFAULT ''
	                              COMMENT 'Avatar hash, not a URL.',
	is_bot        TINYINT(1)      NOT NULL DEFAULT 0,

	first_seen    DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
	updated_at    DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP
	                              ON UPDATE CURRENT_TIMESTAMP,

	PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Discord users (and webhook identities) seen by the discordd bot.';

-- ---------------------------------------------------------------------------
-- The messages themselves.
-- ---------------------------------------------------------------------------
CREATE TABLE discord_messages (
	id                  BIGINT UNSIGNED NOT NULL
	                                    COMMENT 'Discord message snowflake; the natural primary key.',
	channel_id          BIGINT UNSIGNED NOT NULL,
	guild_id            BIGINT UNSIGNED NOT NULL DEFAULT 0,
	author_id           BIGINT UNSIGNED NOT NULL DEFAULT 0,

	-- Non-zero when the message was posted by a webhook rather than a user
	-- or a bot. This is what breaks the bridge loop: a message the
	-- Telegram -> Discord forwarder posted arrives here with webhook_id
	-- set, and must NOT be forwarded back to Telegram. Note that a bot's
	-- own message has is_bot = 1 but webhook_id = 0, so discordd
	-- additionally skips its own author id.
	webhook_id          BIGINT UNSIGNED NOT NULL DEFAULT 0
	                                    COMMENT 'Webhook that posted this, or 0. Non-zero = do not forward (loop guard).',

	content             TEXT            COMMENT 'Message text; empty without the MESSAGE_CONTENT intent.',

	-- message_reference.message_id: the message this one replies to. Kept
	-- so a reply can be mapped onto the corresponding Telegram message.
	reply_to_message_id BIGINT UNSIGNED NOT NULL DEFAULT 0,

	sent_at             DATETIME(3)     NOT NULL
	                                    COMMENT 'Derived from the snowflake: (id >> 22) + 1420070400000 ms.',
	edited_at           DATETIME(3)     DEFAULT NULL
	                                    COMMENT 'Last edit; NULL if never edited.',
	deleted_at          DATETIME        DEFAULT NULL
	                                    COMMENT 'When a MESSAGE_DELETE was observed; NULL while live.',
	logged_at           DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,

	PRIMARY KEY (id),
	KEY idx_discord_messages_channel_sent (channel_id, sent_at),
	KEY idx_discord_messages_author (author_id),
	KEY idx_discord_messages_reply (reply_to_message_id),
	FULLTEXT KEY ft_discord_messages_content (content)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Discord messages logged by discordd.';

CREATE TABLE discord_attachments (
	id           BIGINT UNSIGNED NOT NULL
	                             COMMENT 'Discord attachment snowflake.',
	message_id   BIGINT UNSIGNED NOT NULL,
	filename     VARCHAR(255)    NOT NULL DEFAULT '',
	content_type VARCHAR(128)    NOT NULL DEFAULT '',
	size         BIGINT UNSIGNED NOT NULL DEFAULT 0,

	-- Discord CDN URLs are HMAC-signed and EXPIRE (the ex/is/hm query
	-- params), so this is a record of where the bytes were, not a durable
	-- link. Re-reading the message via the REST API is the only refresh.
	url          VARCHAR(1024)   NOT NULL DEFAULT '',
	width        INT             NOT NULL DEFAULT 0,
	height       INT             NOT NULL DEFAULT 0,

	created_at   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,

	PRIMARY KEY (id),
	KEY idx_discord_attachments_msg (message_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Attachment metadata for logged Discord messages.';

-- ---------------------------------------------------------------------------
-- Forwarding configuration: Discord channel -> Telegram group.
-- ---------------------------------------------------------------------------
--
-- Each route sends through a Telegram BOT, and different routes may use
-- different bots, so the token is interned here rather than repeated per
-- route. This mirrors how discord_endpoints interns webhook URLs for the
-- opposite direction.
--
-- The table is telegram_-prefixed because a bot token is a Telegram-side
-- credential (see web/docs/db-naming.md). It is distinct in kind from the
-- other telegram_* tables, which hold logged data rather than configuration.
CREATE TABLE telegram_bots (
	id           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,

	token        VARCHAR(255)    NOT NULL
	                             COMMENT 'Bot API token used to authenticate a TDLib bot session.',
	bot_user_id  BIGINT          NOT NULL DEFAULT 0
	                             COMMENT 'The bot''s own Telegram user id, learned at login. Used by the Telegram -> Discord forwarder to skip messages this bot posted (loop guard).',
	username     VARCHAR(255)    NOT NULL DEFAULT '',
	enabled      TINYINT(1)      NOT NULL DEFAULT 1,

	created_at   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
	updated_at   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP
	                             ON UPDATE CURRENT_TIMESTAMP,

	PRIMARY KEY (id),
	UNIQUE KEY uq_telegram_bots_token (token),
	KEY idx_telegram_bots_user (bot_user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Telegram bot credentials used to send forwarded Discord messages.';

CREATE TABLE discord_telegram_routes (
	id                 BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,

	discord_channel_id BIGINT UNSIGNED NOT NULL
	                                   COMMENT 'Source Discord channel.',
	telegram_chat_id   BIGINT          NOT NULL
	                                   COMMENT 'Destination Telegram chat (negative for a group/channel).',
	telegram_bot_id    BIGINT UNSIGNED NOT NULL
	                                   COMMENT 'FK to telegram_bots.id: which bot sends.',
	enabled            TINYINT(1)      NOT NULL DEFAULT 1,

	created_at         DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
	updated_at         DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP
	                                   ON UPDATE CURRENT_TIMESTAMP,

	PRIMARY KEY (id),
	-- One route per (source, destination); a channel may fan out to several
	-- Telegram chats, each through its own bot.
	UNIQUE KEY uq_dtr_channel_chat (discord_channel_id, telegram_chat_id),
	KEY idx_dtr_enabled_channel (enabled, discord_channel_id),
	KEY idx_dtr_bot (telegram_bot_id),
	CONSTRAINT fk_dtr_bot FOREIGN KEY (telegram_bot_id)
		REFERENCES telegram_bots (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Discord channel -> Telegram chat forwarding routes.';

-- ---------------------------------------------------------------------------
-- What discordd actually sent to Telegram.
-- ---------------------------------------------------------------------------
--
-- Serves three purposes: applying a later Discord edit or delete to the
-- Telegram side, preventing a duplicate send after a restart, and -- the
-- reason it stores the Telegram id -- resolving a Discord reply to the
-- Telegram message it should reply to.
--
-- Reply resolution has two sources, because a Discord message may correspond
-- to a Telegram message in either direction:
--   * discordd forwarded it   -> look it up here
--   * it mirrors a Telegram message posted by the Telegram -> Discord
--     forwarder -> look it up in telegram_discord_sent_messages
CREATE TABLE discord_telegram_sent_messages (
	id                  BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,

	discord_message_id  BIGINT UNSIGNED NOT NULL
	                                    COMMENT 'Source Discord message.',
	route_id            BIGINT UNSIGNED NOT NULL
	                                    COMMENT 'FK to discord_telegram_routes.id.',
	telegram_chat_id    BIGINT          NOT NULL,
	telegram_message_id BIGINT          NOT NULL
	                                    COMMENT 'Telegram SERVER message id (tdlib id >> 20).',

	created_at          DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,

	PRIMARY KEY (id),
	-- Idempotency: a redelivered gateway event must not double-send.
	UNIQUE KEY uq_dts_msg_route (discord_message_id, route_id),
	KEY idx_dts_discord_msg (discord_message_id),
	KEY idx_dts_telegram (telegram_chat_id, telegram_message_id),
	KEY idx_dts_route (route_id),
	CONSTRAINT fk_dts_route FOREIGN KEY (route_id)
		REFERENCES discord_telegram_routes (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Telegram messages discordd sent for a Discord message.';

-- Reply resolution walks telegram_discord_sent_messages backwards -- from a Discord
-- message id to the Telegram message it mirrors -- which that table was never
-- indexed for (it only ever looked messages up by their Telegram ids).
ALTER TABLE telegram_discord_sent_messages
	ADD KEY idx_dsm_discord_msg (message_id);
