-- History of user name changes.
-- A new row is inserted every time tgloggerd observes a user's
-- first_name or last_name change.

CREATE TABLE user_hist_name (
	-- Surrogate primary key.
	id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
	-- The user whose name changed.
	user_id    BIGINT          NOT NULL COMMENT 'FK to users.id.',
	-- Snapshot of the name at the time of the change.
	first_name VARCHAR(255)    NOT NULL DEFAULT '' COMMENT 'User first name.',
	last_name  VARCHAR(255)    NOT NULL DEFAULT '' COMMENT 'User last name.',
	-- When this snapshot was recorded.
	created_at DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',

	PRIMARY KEY (id),
	KEY idx_user_hist_name_user_id (user_id),
	CONSTRAINT fk_user_hist_name_user
		FOREIGN KEY (user_id) REFERENCES users (id)
		ON DELETE CASCADE ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='History of user name changes (first_name, last_name).';

-- History of user profile photo changes.
-- A new row is inserted every time tgloggerd observes a user's
-- profile_photo_file_id change.

CREATE TABLE user_hist_profile_photo (
	-- Surrogate primary key.
	id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
	-- The user whose profile photo changed.
	user_id    BIGINT          NOT NULL COMMENT 'FK to users.id.',
	-- The file that was the profile photo at the time of the change.
	file_id    BIGINT UNSIGNED NULL COMMENT 'FK to files.id; NULL if photo was removed.',
	-- When this snapshot was recorded.
	created_at DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',

	PRIMARY KEY (id),
	KEY idx_user_hist_profile_photo_user_id (user_id),
	CONSTRAINT fk_user_hist_profile_photo_user
		FOREIGN KEY (user_id) REFERENCES users (id)
		ON DELETE CASCADE ON UPDATE CASCADE,
	CONSTRAINT fk_user_hist_profile_photo_file
		FOREIGN KEY (file_id) REFERENCES files (id)
		ON DELETE SET NULL ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='History of user profile photo changes.';

-- History of user phone number changes.
-- A new row is inserted every time tgloggerd observes a user's
-- phone_number change.

CREATE TABLE user_hist_phone_num (
	-- Surrogate primary key.
	id           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
	-- The user whose phone number changed.
	user_id      BIGINT          NOT NULL COMMENT 'FK to users.id.',
	-- Snapshot of the phone number at the time of the change.
	phone_number VARCHAR(32)     NOT NULL DEFAULT '' COMMENT 'User phone number.',
	-- When this snapshot was recorded.
	created_at   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',

	PRIMARY KEY (id),
	KEY idx_user_hist_phone_num_user_id (user_id),
	CONSTRAINT fk_user_hist_phone_num_user
		FOREIGN KEY (user_id) REFERENCES users (id)
		ON DELETE CASCADE ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='History of user phone number changes.';

-- History of user bio changes.
-- The bio lives in td_api::userFullInfo, fetched separately from the user
-- object. A new row records the previous bio each time tgloggerd observes
-- the bio change.

CREATE TABLE user_hist_bio (
	-- Surrogate primary key.
	id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
	-- The user whose bio changed.
	user_id    BIGINT          NOT NULL COMMENT 'FK to users.id.',
	-- Snapshot of the bio as observed: the initial bio and every later
	-- change, appended when non-empty and different from the previous one.
	bio        VARCHAR(255)    NOT NULL DEFAULT '' COMMENT 'User bio snapshot.',
	-- When this snapshot was recorded.
	created_at DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',

	PRIMARY KEY (id),
	KEY idx_user_hist_bio_user_id (user_id),
	CONSTRAINT fk_user_hist_bio_user
		FOREIGN KEY (user_id) REFERENCES users (id)
		ON DELETE CASCADE ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='History of user bio changes.';

-- Audit log of username changes: additions, removals, reordering within
-- a list, and kind changes. tgloggerd computes these by diffing a user's
-- new username set against the previous state.

CREATE TABLE user_hist_usernames_events (
	-- Surrogate primary key.
	id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
	-- The user whose username set changed.
	user_id    BIGINT          NOT NULL COMMENT 'FK to users.id.',
	-- The username that was modified.
	username   VARCHAR(32)     NOT NULL COMMENT 'The username that was modified.',
	-- What triggered this history log.
	action     ENUM('added', 'removed', 'reordered', 'kind_changed',
	                'collectible_changed') NOT NULL
	                           COMMENT 'What triggered this history log.',
	-- Activation status after the change; NULL when the action is 'removed'.
	kind       ENUM('active', 'disabled') NULL
	                           COMMENT 'Activation status after the change; NULL if removed.',
	-- Collectible flag after the change; NULL when the action is 'removed'.
	is_collectible TINYINT(1)  NULL COMMENT 'Collectible state after the change; NULL if removed.',
	position   INT             NULL COMMENT 'New position (0-based); NULL if removed.',
	-- When this change was recorded.
	created_at DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',

	PRIMARY KEY (id),
	KEY idx_user_hist_usernames_events_user_id (user_id),
	KEY idx_user_hist_usernames_events_username (username),
	CONSTRAINT fk_user_hist_usernames_events_user
		FOREIGN KEY (user_id) REFERENCES users (id)
		ON DELETE CASCADE ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Event log of additions, removals, and position changes to usernames.';
