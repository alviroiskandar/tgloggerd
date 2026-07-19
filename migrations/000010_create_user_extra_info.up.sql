-- Extra, sparse per-user attributes split out of the users table. Most users
-- leave all of these at their defaults (e.g. no bio, no custom appearance), so
-- a row exists here only when at least one field is set; the daemon deletes the
-- row when everything falls back to its "empty" sentinel. A user therefore may
-- have no row here, which reads as all-default.
--
-- Sources: everything except bio/personal_chat_id comes from the td_api::user
-- object; bio and personal_chat_id come from userFullInfo, fetched separately.
-- The "empty" sentinel per column is noted in its comment and matches the
-- default, so the daemon's prune check is exactly "every column is default".

CREATE TABLE user_extra_info (
	-- Owning user; one row per user at most.
	user_id                            BIGINT          NOT NULL COMMENT 'FK to users.id.',

	-- userFullInfo.bio; empty '' if none. History in user_hist_bio.
	bio                                VARCHAR(255)    NOT NULL DEFAULT '' COMMENT 'User bio/about text; empty if none.',

	-- Phone number, if visible; empty '' if none. History in user_hist_phone_num.
	phone_number                       VARCHAR(32)     NOT NULL DEFAULT '' COMMENT 'User phone number; empty if none.',

	-- Appearance emoji ids.
	background_custom_emoji_id         BIGINT          NOT NULL DEFAULT 0 COMMENT 'Custom emoji id for the name background; 0 if none.',
	profile_accent_color_id            INT             NOT NULL DEFAULT -1 COMMENT 'Profile accent color id; -1 if none.',
	profile_background_custom_emoji_id BIGINT          NOT NULL DEFAULT 0 COMMENT 'Custom emoji id for the profile background; 0 if none.',

	-- td_api::emojiStatus
	emoji_status_custom_emoji_id       BIGINT          NULL COMMENT 'Custom emoji id shown as emoji status; NULL if none.',
	emoji_status_expiration_date       BIGINT          NULL COMMENT 'Unix time when the emoji status expires; NULL if none.',

	-- td_api::restrictionInfo
	restriction_reason                 VARCHAR(255)    NOT NULL DEFAULT '' COMMENT 'Reason the user is restricted; empty if none.',
	has_sensitive_content              TINYINT(1)      NOT NULL DEFAULT 0 COMMENT 'User content is marked sensitive.',

	restricts_new_chats                TINYINT(1)      NOT NULL DEFAULT 0 COMMENT 'User may restrict new chats from non-contacts.',
	paid_message_star_count            BIGINT          NOT NULL DEFAULT 0 COMMENT 'Telegram Stars required to message the user; 0 if none.',

	-- IETF BCP-47 language tag of the user, if known; empty if none.
	language_code                      VARCHAR(35)     NOT NULL DEFAULT '' COMMENT 'User language code; empty if none.',

	-- userFullInfo.personal_chat_id: the channel the user linked to their
	-- profile. It is a chat id (always a channel/supergroup), so it references
	-- groups.id; NULL when the user has none (or the channel is not stored).
	personal_chat_id                   BIGINT          NULL DEFAULT NULL COMMENT 'Linked personal chat (FK to groups.id); NULL if none.',

	-- Bookkeeping
	created_at                         DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',
	updated_at                         DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP
	                                                   ON UPDATE CURRENT_TIMESTAMP COMMENT 'Last time the row was updated.',

	PRIMARY KEY (user_id),
	KEY idx_user_extra_info_personal_chat_id (personal_chat_id),
	CONSTRAINT fk_user_extra_info_user
		FOREIGN KEY (user_id) REFERENCES users (id)
		ON DELETE CASCADE ON UPDATE CASCADE,
	CONSTRAINT fk_user_extra_info_personal_chat
		FOREIGN KEY (personal_chat_id) REFERENCES `groups` (id)
		ON DELETE SET NULL ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Sparse extra user attributes; a row exists only when something is set.';
