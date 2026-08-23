-- The allowlist of Telegram groups that may be exposed outside the web UI.
--
-- Everything the web UI shows is behind a login, so it has never needed a
-- notion of "public". The MCP server (see src/gwmcp and web/src/mcp) hands data
-- to a language model, and there the distinction is the whole point: a private
-- group's messages must never leave.
--
-- Publicness could have been DERIVED. A Telegram supergroup or channel is
-- public exactly when it currently owns an active public username, which the
-- schema already expresses -- telegram_group_usernames.group_id is set to NULL
-- when a username is released, so the predicate
--
--     EXISTS (SELECT 1 FROM telegram_group_usernames gu
--              WHERE gu.group_id = g.id AND gu.kind = 'active')
--
-- is live and self-healing, and matches 16,343 of the 22,715 known groups.
--
-- It is deliberately NOT used as the gate. Deriving exposure means every group
-- that ever becomes public is exposed the moment TDLib notices, with no human
-- in the loop, and one wrong reading of the predicate leaks private history. An
-- allowlist inverts the failure mode: nothing is exposed until an admin adds it,
-- and the worst outcome of a bug is that too little data is visible. The admin
-- UI still SHOWS the derived signal as a hint when picking groups -- it just
-- never acts on it.
--
-- Single-platform data, so a single telegram_ prefix (web/docs/db-naming.md).
CREATE TABLE telegram_public_groups (
	group_id   BIGINT          NOT NULL
	                           COMMENT 'FK to telegram_groups.id (negative Telegram chat id).',

	note       VARCHAR(255)    NOT NULL DEFAULT ''
	                           COMMENT 'Why this group was exposed; free text for the audit trail.',

	added_by   BIGINT UNSIGNED NOT NULL DEFAULT 0
	                           COMMENT 'web_users.id of the admin who added it. Not an FK: that table lives in the web database, a different schema.',

	created_at DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,

	PRIMARY KEY (group_id),
	CONSTRAINT fk_tpg_group FOREIGN KEY (group_id)
		REFERENCES telegram_groups (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Groups an admin has explicitly allowed the MCP server to expose.';

-- OPERATIONAL NOTE -- grant the web user DML on this table after applying.
--
-- The web app reaches this schema through the read-only web_ro user, and the
-- admin page needs to add and remove rows. docker/mysql/init/10-web-users.sh
-- grants this on a fresh data directory, but that hook only runs once. The
-- migration cannot issue the GRANT itself: it is applied by the unprivileged
-- daemon user, which can neither read mysql.user nor GRANT, and attempting it
-- only dirties the schema version and crash-loops the daemon on restart.
--
-- On an existing deployment, run this once as root:
--
--   GRANT INSERT, DELETE ON `tgloggerd`.telegram_public_groups TO 'web_ro'@'%';
--   FLUSH PRIVILEGES;
--
-- SELECT is already covered by the blanket grant on the schema.
