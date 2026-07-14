-- Web application's own tables. These live in a SEPARATE database
-- (tgloggerd_web) owned by the web app's read-write user, kept apart from
-- the logger's tgloggerd schema which the web app only reads. This keeps
-- the read-only boundary a database-level invariant and gives the web app
-- its own golang-migrate history.

-- Login accounts for the web interface.
CREATE TABLE web_users (
	id            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,

	username      VARCHAR(64)     NOT NULL COMMENT 'Login name.',
	-- Self-describing hash (algorithm + params + salt), e.g. libsodium
	-- crypto_pwhash_str (argon2id) output.
	password_hash VARCHAR(255)    NOT NULL COMMENT 'Password hash (argon2id).',
	role          ENUM('admin', 'viewer')
	                              NOT NULL DEFAULT 'viewer' COMMENT 'Access level.',
	is_active     TINYINT(1)      NOT NULL DEFAULT 1 COMMENT 'Login disabled when 0.',

	created_at    DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',
	updated_at    DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP
	                              ON UPDATE CURRENT_TIMESTAMP COMMENT 'Last update time.',

	PRIMARY KEY (id),
	UNIQUE KEY uq_web_users_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Web interface login accounts.';

-- Audit log of security-relevant web actions (login success/failure,
-- logout, media fetches). web_user_id has no foreign key so the log
-- survives account deletion and can record failures for unknown users.
CREATE TABLE web_audit (
	id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,

	web_user_id BIGINT UNSIGNED NULL COMMENT 'web_users.id; NULL for anonymous/failed actions.',
	action      VARCHAR(64)     NOT NULL COMMENT 'e.g. login_ok, login_fail, logout, media.',
	ip          VARCHAR(45)     NULL COMMENT 'Client IP (IPv4/IPv6).',
	detail      VARCHAR(255)    NULL COMMENT 'Optional context (username tried, file id, ...).',

	created_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'When it happened.',

	PRIMARY KEY (id),
	KEY idx_web_audit_user (web_user_id),
	KEY idx_web_audit_created (created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Audit log of web interface security events.';
