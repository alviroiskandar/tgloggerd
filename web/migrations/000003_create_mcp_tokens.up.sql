-- Bearer tokens for the MCP endpoint (/mcp).
--
-- The web UI authenticates with a signed session cookie, which an MCP client
-- cannot obtain: there is no form to post and no browser to hold the cookie. So
-- /mcp takes `Authorization: Bearer <token>` instead, and this table is the
-- token store.
--
-- THE TOKEN ITSELF IS NOT STORED. Only its SHA-256 is. The plaintext is shown
-- once, at mint time, and is unrecoverable afterwards -- so a dump of this
-- table grants nobody access. That is worth the small UX cost of "copy it now
-- or mint a new one", because these tokens read the entire message archive.
--
-- SHA-256 rather than argon2id (which web_users.password_hash uses) is
-- deliberate: a password is low-entropy and needs a slow hash to survive
-- guessing, whereas this token is 32 random bytes, so there is nothing to
-- guess and a slow hash would only add latency to every single MCP request.
CREATE TABLE web_mcp_tokens (
	id           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,

	web_user_id  BIGINT UNSIGNED NOT NULL
	                             COMMENT 'Owning account; requests are attributed to it.',

	name         VARCHAR(64)     NOT NULL DEFAULT ''
	                             COMMENT 'Human label, e.g. "laptop" -- so one can be revoked without guessing.',

	token_sha256 BINARY(32)      NOT NULL
	                             COMMENT 'SHA-256 of the plaintext token. The token itself is never stored.',

	created_at   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
	last_used_at DATETIME        DEFAULT NULL
	                             COMMENT 'Updated on use, so an unused token is visibly stale.',
	revoked_at   DATETIME        DEFAULT NULL
	                             COMMENT 'Set instead of deleting, so a revoked token stays auditable.',

	PRIMARY KEY (id),
	-- Verification is one lookup on this key.
	UNIQUE KEY uq_web_mcp_tokens_hash (token_sha256),
	KEY idx_web_mcp_tokens_user (web_user_id),
	CONSTRAINT fk_web_mcp_tokens_user FOREIGN KEY (web_user_id)
		REFERENCES web_users (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Bearer tokens authenticating MCP clients at /mcp.';
