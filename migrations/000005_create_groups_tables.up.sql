-- Telegram group chats: basic groups, supergroups and broadcast
-- channels. The data is assembled from several TDLib objects (chat,
-- supergroup/basicGroup and their full info). Keyed by the Telegram
-- chat_id, which is globally unique across all chats.

CREATE TABLE `groups` (
	-- Telegram chat_id (globally unique across all chats).
	id            BIGINT          NOT NULL,
	-- Kind of group chat.
	type          ENUM('basic_group', 'supergroup', 'channel')
	                              NOT NULL DEFAULT 'basic_group' COMMENT 'Kind of group chat.',
	-- Group title (from the chat object).
	title         VARCHAR(255)    NOT NULL DEFAULT '' COMMENT 'Group title.',
	-- Group description (from supergroup/basicGroup full info).
	description   VARCHAR(255)    NOT NULL DEFAULT '' COMMENT 'Group description.',
	-- Current group photo; references the shared files table.
	photo_file_id BIGINT UNSIGNED NULL COMMENT 'FK to files.id for the current group photo.',

	created_at    DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',
	updated_at    DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP
	                              ON UPDATE CURRENT_TIMESTAMP COMMENT 'Last time the row was updated.',

	PRIMARY KEY (id),
	KEY idx_groups_photo_file_id (photo_file_id),
	CONSTRAINT fk_groups_photo_file
		FOREIGN KEY (photo_file_id) REFERENCES files (id)
		ON DELETE SET NULL ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Telegram group chats (basic groups, supergroups, channels).';

-- Normalized group usernames (td_api::usernames), mirroring
-- user_usernames. Usernames share Telegram's global namespace, so
-- username is UNIQUE and ownership is tracked by group_id (NULL once
-- released).

CREATE TABLE group_usernames (
	id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
	group_id   BIGINT          NULL COMMENT 'FK to groups.id; NULL when released.',
	username   VARCHAR(32)     NOT NULL COMMENT 'Username without the leading @.',
	-- Activation status; "collectible" is orthogonal (see is_collectible).
	kind       ENUM('active', 'disabled') NOT NULL
	                           COMMENT 'Activation status: active or disabled.',
	is_collectible TINYINT(1)  NOT NULL DEFAULT 0 COMMENT 'Purchased at fragment.com.',
	position   INT             NOT NULL DEFAULT 0 COMMENT 'Order within its list (0-based).',
	created_at DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',

	PRIMARY KEY (id),
	UNIQUE KEY uq_group_usernames_username (username),
	KEY idx_group_usernames_group_id (group_id),
	CONSTRAINT fk_group_usernames_group
		FOREIGN KEY (group_id) REFERENCES `groups` (id)
		ON DELETE SET NULL ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Normalized group usernames; NULL group_id marks released usernames.';

-- History of group title changes.

CREATE TABLE group_hist_title (
	id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
	group_id   BIGINT          NOT NULL COMMENT 'FK to groups.id.',
	title      VARCHAR(255)    NOT NULL DEFAULT '' COMMENT 'Group title snapshot.',
	created_at DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',

	PRIMARY KEY (id),
	KEY idx_group_hist_title_group_id (group_id),
	CONSTRAINT fk_group_hist_title_group
		FOREIGN KEY (group_id) REFERENCES `groups` (id)
		ON DELETE CASCADE ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='History of group title changes.';

-- History of group description changes.

CREATE TABLE group_hist_description (
	id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
	group_id    BIGINT          NOT NULL COMMENT 'FK to groups.id.',
	description VARCHAR(255)    NOT NULL DEFAULT '' COMMENT 'Group description snapshot.',
	created_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',

	PRIMARY KEY (id),
	KEY idx_group_hist_description_group_id (group_id),
	CONSTRAINT fk_group_hist_description_group
		FOREIGN KEY (group_id) REFERENCES `groups` (id)
		ON DELETE CASCADE ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='History of group description changes.';

-- History of group photo changes.

CREATE TABLE group_hist_photo (
	id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
	group_id   BIGINT          NOT NULL COMMENT 'FK to groups.id.',
	file_id    BIGINT UNSIGNED NULL COMMENT 'FK to files.id; NULL if photo was removed.',
	created_at DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',

	PRIMARY KEY (id),
	KEY idx_group_hist_photo_group_id (group_id),
	CONSTRAINT fk_group_hist_photo_group
		FOREIGN KEY (group_id) REFERENCES `groups` (id)
		ON DELETE CASCADE ON UPDATE CASCADE,
	CONSTRAINT fk_group_hist_photo_file
		FOREIGN KEY (file_id) REFERENCES files (id)
		ON DELETE SET NULL ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='History of group photo changes.';

-- Audit log of group username changes, mirroring
-- user_hist_usernames_events.

CREATE TABLE group_hist_usernames_events (
	id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
	group_id   BIGINT          NOT NULL COMMENT 'FK to groups.id.',
	username   VARCHAR(32)     NOT NULL COMMENT 'The username that was modified.',
	action     ENUM('added', 'removed', 'reordered', 'kind_changed',
	                'collectible_changed') NOT NULL
	                           COMMENT 'What triggered this history log.',
	kind       ENUM('active', 'disabled') NULL
	                           COMMENT 'Activation status after the change; NULL if removed.',
	is_collectible TINYINT(1)  NULL COMMENT 'Collectible state after the change; NULL if removed.',
	position   INT             NULL COMMENT 'New position (0-based); NULL if removed.',
	created_at DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',

	PRIMARY KEY (id),
	KEY idx_group_hist_usernames_events_group_id (group_id),
	KEY idx_group_hist_usernames_events_username (username),
	CONSTRAINT fk_group_hist_usernames_events_group
		FOREIGN KEY (group_id) REFERENCES `groups` (id)
		ON DELETE CASCADE ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Event log of additions, removals, and position changes to group usernames.';
