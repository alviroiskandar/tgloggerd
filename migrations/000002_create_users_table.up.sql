-- Users table, modeled after the TDLib `user` object (td_api::user).
-- Stores the last-known state of each Telegram user seen by tgloggerd.
-- Volatile online/offline status is intentionally not stored here; per
-- attribute history (name, username, photo, ...) is tracked in dedicated
-- tables added later.
--
-- Sparse or session-specific attributes (bio, phone number, appearance emoji
-- ids, restriction/language, personal chat, ...) live in the separate
-- user_extra_info table, which only holds a row when at least one of them is
-- set. Fields that reveal the logged-in account's own relationship to the
-- user (contact/close-friend/access) are not stored at all: they are
-- meaningless once the logger is public.

CREATE TABLE users (
	-- td_api::user.id (int53): Telegram user identifier.
	id                                 BIGINT          NOT NULL,

	-- Identity
	first_name                         VARCHAR(255)    NOT NULL DEFAULT '' COMMENT 'User first name.',
	last_name                          VARCHAR(255)    NOT NULL DEFAULT '' COMMENT 'User last name.',

	-- td_api::UserType
	type                               ENUM('regular', 'deleted', 'bot', 'unknown')
	                                                   NOT NULL DEFAULT 'unknown' COMMENT 'Kind of user.',

	-- Profile photo; references the shared files table.
	profile_photo_file_id              BIGINT UNSIGNED NULL COMMENT 'FK to files.id for the current profile photo.',

	-- Appearance
	accent_color_id                    INT             NOT NULL DEFAULT 0 COMMENT 'td_api accent_color_id.',

	-- td_api::verificationStatus
	is_verified                        TINYINT(1)      NOT NULL DEFAULT 0 COMMENT 'User is verified by Telegram.',
	is_scam                            TINYINT(1)      NOT NULL DEFAULT 0 COMMENT 'User is flagged as a scam.',
	is_fake                            TINYINT(1)      NOT NULL DEFAULT 0 COMMENT 'User is flagged as fake.',

	is_premium                         TINYINT(1)      NOT NULL DEFAULT 0 COMMENT 'User has Telegram Premium.',
	is_support                         TINYINT(1)      NOT NULL DEFAULT 0 COMMENT 'User is a Telegram support account.',

	-- td_api::birthdate (from userFullInfo, fetched separately).
	birthday_day                       TINYINT UNSIGNED NULL COMMENT 'Birthday day (1-31); NULL if unset.',
	birthday_month                     TINYINT UNSIGNED NULL COMMENT 'Birthday month (1-12); NULL if unset.',
	birthday_year                      SMALLINT UNSIGNED NULL COMMENT 'Birthday year; NULL if unset or hidden.',

	-- Bookkeeping
	created_at                         DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',
	updated_at                         DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP
	                                                   ON UPDATE CURRENT_TIMESTAMP COMMENT 'Last time the row was updated.',

	PRIMARY KEY (id),
	KEY idx_users_profile_photo_file_id (profile_photo_file_id),
	CONSTRAINT fk_users_profile_photo_file
		FOREIGN KEY (profile_photo_file_id) REFERENCES files (id)
		ON DELETE SET NULL ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Last-known state of each Telegram user (td_api::user).';
