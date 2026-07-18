-- Private messages for one-to-one chats (not groups).
-- Each Telegram message is keyed by (chat_id, message_id).
-- Soft-deleted messages keep their row with is_deleted = 1.
-- Edits are tracked in private_message_edits (append-only).

CREATE TABLE private_messages (
	-- Surrogate primary key referenced by edits and fwd_info.
	id           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,

	-- Telegram chat_id; for private chats this equals the peer user's id.
	chat_id      BIGINT          NOT NULL COMMENT 'Telegram chat_id (peer user id for private chats).',

	-- Telegram message identifier (unique per chat).
	message_id   BIGINT          NOT NULL COMMENT 'td_api::message.id_.',

	-- Who sent the message; NULL for the logged-in account.
	sender_id    BIGINT          NULL COMMENT 'FK to users.id for the sender; NULL = own account.',

	-- Whether the message was sent by the logged-in user.
	is_outgoing  TINYINT(1)      NOT NULL DEFAULT 0 COMMENT 'Message was sent by the logged-in account.',

	-- Unix timestamp of the original send.
	date         BIGINT          NOT NULL DEFAULT 0 COMMENT 'Unix timestamp of the original send.',

	-- Unix timestamp of the last edit; 0 if never edited.
	edit_date    BIGINT          NOT NULL DEFAULT 0 COMMENT 'Unix timestamp of the last edit; 0 = never edited.',

	-- Coarse content type for routing and filtering.
	content_type ENUM('text', 'photo', 'video', 'document', 'audio',
	                  'voice', 'sticker', 'animation', 'unknown')
	                             NOT NULL DEFAULT 'unknown' COMMENT 'Coarse message content category.',

	-- Message text; NULL for non-text messages. MEDIUMTEXT for long messages.
	text         MEDIUMTEXT      NULL COMMENT 'Message text; NULL for non-text messages.',

	-- For media messages: FK to files.id for the attached file.
	file_id      BIGINT UNSIGNED NULL COMMENT 'FK to files.id for media attachments.',

	-- Soft-delete timestamp: when the deletion was observed, NULL if the
	-- message is not deleted. Deleted messages keep their row.
	deleted_at   DATETIME        NULL DEFAULT NULL COMMENT 'When the message was observed deleted; NULL if live.',

	-- Whether the message is forwarded. Denormalized from the presence
	-- of forward info (a private_message_fwd_info row) for quick filtering.
	is_forwarded TINYINT(1)      NOT NULL DEFAULT 0 COMMENT 'Message is a forwarded message.',

	-- The message this one replies to. reply_to_chat_id/reply_to_msg_id
	-- identify it universally (works across chats and tables, e.g. a reply
	-- to a group message). reply_to_id is the surrogate-id FK, set only
	-- when the replied message is in this same table. All NULL if not a
	-- reply. Resolved in setPrivateMessageReply.
	reply_to_chat_id BIGINT      NULL COMMENT 'Replied message chat_id; may differ from chat_id (cross-chat).',
	reply_to_msg_id  BIGINT      NULL COMMENT 'Replied message message_id.',
	reply_to_id  BIGINT UNSIGNED NULL COMMENT 'FK to private_messages.id of the replied message (same table only); NULL otherwise.',

	-- Bookkeeping.
	created_at   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',
	updated_at   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP
	                             ON UPDATE CURRENT_TIMESTAMP COMMENT 'Last time the row was updated.',

	PRIMARY KEY (id),
	UNIQUE KEY uq_private_messages_chat_msg (chat_id, message_id),
	KEY idx_private_messages_sender_id (sender_id),
	KEY idx_private_messages_date (date),
	KEY idx_private_messages_deleted_at (deleted_at),
	KEY idx_private_messages_reply (reply_to_id),
	KEY idx_private_messages_reply_target (reply_to_chat_id, reply_to_msg_id),
	CONSTRAINT fk_private_messages_chat_user
		FOREIGN KEY (chat_id) REFERENCES users (id)
		ON DELETE CASCADE ON UPDATE CASCADE,
	CONSTRAINT fk_private_messages_sender_user
		FOREIGN KEY (sender_id) REFERENCES users (id)
		ON DELETE SET NULL ON UPDATE CASCADE,
	CONSTRAINT fk_private_messages_file
		FOREIGN KEY (file_id) REFERENCES files (id)
		ON DELETE SET NULL ON UPDATE CASCADE,
	-- Self-reference to the replied message by surrogate id.
	CONSTRAINT fk_private_messages_reply
		FOREIGN KEY (reply_to_id) REFERENCES private_messages (id)
		ON DELETE SET NULL ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Private chat messages (one-to-one chats only). Soft-deleted rows persist.';

-- Append-only edit history. A row is inserted only when an edit is
-- detected. The current state is always in private_messages; this
-- table records snapshots of the message content *before* each edit.
-- Never updated; only inserted.

CREATE TABLE private_message_edits (
	-- Surrogate primary key.
	id                BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,

	-- FK to private_messages.id for the message being edited.
	private_message_id BIGINT UNSIGNED NOT NULL COMMENT 'FK to private_messages.id.',

	-- Content captured before the edit was applied.
	content_type      ENUM('text', 'photo', 'video', 'document', 'audio',
	                       'voice', 'sticker', 'animation', 'unknown')
	                                  NOT NULL DEFAULT 'unknown' COMMENT 'Content type before the edit.',
	text              MEDIUMTEXT      NULL COMMENT 'Text before the edit; NULL for non-text.',
	file_id           BIGINT UNSIGNED NULL COMMENT 'FK to files.id before the edit.',

	-- The edit_date value that triggered this snapshot (equals the
	-- new edit_date in private_messages after the update).
	edit_date         BIGINT          NOT NULL COMMENT 'The edit_date value that triggered this snapshot.',

	-- Bookkeeping.
	created_at        DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',

	PRIMARY KEY (id),
	KEY idx_private_message_edits_private_message_id (private_message_id),
	KEY idx_private_message_edits_edit_date (edit_date),
	CONSTRAINT fk_private_message_edits_message
		FOREIGN KEY (private_message_id) REFERENCES private_messages (id)
		ON DELETE CASCADE ON UPDATE CASCADE,
	CONSTRAINT fk_private_message_edits_file
		FOREIGN KEY (file_id) REFERENCES files (id)
		ON DELETE SET NULL ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Append-only edit history: snapshot of message content before each edit.';

-- Forward information for forwarded messages. One row per forwarded
-- message; NULL forward info means the message is not forwarded.
-- The origin_* columns capture the original source.

CREATE TABLE private_message_fwd_info (
	-- Surrogate primary key.
	id                  BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,

	-- FK to private_messages.id for the forwarded message.
	private_message_id   BIGINT UNSIGNED NOT NULL COMMENT 'FK to private_messages.id.',

	-- td_api::MessageOrigin discriminator.
	origin_type         ENUM('user', 'hidden_user', 'chat', 'channel')
	                                    NOT NULL COMMENT 'Kind of message origin.',

	-- Origin sender user id (messageOriginUser).
	origin_sender_user_id  BIGINT       NULL COMMENT 'Original sender user id (messageOriginUser); FK to users.id.',

	-- Origin sender name when the sender is hidden (messageOriginHiddenUser)
	-- or author_signature from messageOriginChat/messageOriginChannel.
	origin_sender_name     VARCHAR(255) NULL COMMENT 'Original sender name or author signature.',

	-- Origin chat/channel id (messageOriginChat, messageOriginChannel).
	origin_chat_id         BIGINT       NULL COMMENT 'Original chat/channel id.',

	-- Original message id within the origin chat (messageOriginChannel).
	origin_message_id      BIGINT       NULL COMMENT 'Original message id (messageOriginChannel).',

	-- Date of the original message (from messageForwardInfo.date_).
	origin_date            BIGINT       NOT NULL DEFAULT 0 COMMENT 'Unix timestamp of the original message.',

	-- Bookkeeping.
	created_at            DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',

	PRIMARY KEY (id),
	UNIQUE KEY uq_private_message_fwd_info_msg (private_message_id),
	KEY idx_private_message_fwd_info_origin_sender_user_id (origin_sender_user_id),
	KEY idx_private_message_fwd_info_origin_chat_id (origin_chat_id),
	CONSTRAINT fk_private_message_fwd_info_message
		FOREIGN KEY (private_message_id) REFERENCES private_messages (id)
		ON DELETE CASCADE ON UPDATE CASCADE,
	CONSTRAINT fk_private_message_fwd_info_origin_sender_user
		FOREIGN KEY (origin_sender_user_id) REFERENCES users (id)
		ON DELETE SET NULL ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Forward information for forwarded private messages.';
