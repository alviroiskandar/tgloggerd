-- Normalized usernames (td_api::usernames). Telegram enforces a global
-- username namespace: at most one user holds a given username at a time.
-- Each username therefore has at most one row (UNIQUE), and ownership is
-- tracked by user_id, which points at the current owner or is NULL once
-- the username has been released. tgloggerd upserts rows with
-- INSERT ... ON DUPLICATE KEY UPDATE so claiming, ownership transfer,
-- reordering and kind changes happen in place; the individual changes
-- are recorded in user_hist_usernames_events.

CREATE TABLE user_usernames (
	-- Surrogate primary key.
	id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
	-- Owning user; NULL when the username has been released.
	user_id    BIGINT          NULL COMMENT 'FK to users.id; NULL when released.',
	-- The username text without the leading '@'.
	username   VARCHAR(32)     NOT NULL COMMENT 'Username without the leading @.',
	-- Activation status. A username is active XOR disabled; "collectible" is
	-- orthogonal (see is_collectible), since a username can be active AND
	-- collectible at the same time.
	kind       ENUM('active', 'disabled') NOT NULL
	                           COMMENT 'Activation status: active or disabled.',
	-- Whether this username was purchased at fragment.com (td_api
	-- usernames.collectible_usernames). Independent of active/disabled.
	is_collectible TINYINT(1)  NOT NULL DEFAULT 0 COMMENT 'Purchased at fragment.com.',
	-- Preserves order within the active list (0-based).
	position   INT             NOT NULL DEFAULT 0 COMMENT 'Order within its list (0-based).',
	-- Row creation time.
	created_at DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',

	PRIMARY KEY (id),
	UNIQUE KEY uq_user_usernames_username (username),
	KEY idx_user_usernames_user_id (user_id),
	CONSTRAINT fk_user_usernames_user
		FOREIGN KEY (user_id) REFERENCES users (id)
		ON DELETE SET NULL ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Normalized usernames per user (td_api::usernames); NULL user_id marks released usernames.';
