-- The scope of each MCP token (docs/todo/mcp-token-scope.md,
-- decisions/2026-11-20-mcp-token-scope.md): the federations, groups
-- and private chats a token may read. A token without rows reads every
-- exposed group, as every token did before; a token with rows reads
-- only what they reach, and nothing when none of them reaches anything.
--
-- Additive: a new table only, and no row is written here, so every
-- existing token keeps reading what it read.
--
-- target_id is a federation id (web_federations.id, 0 for Default), a
-- group's negative chat id or a private chat's (the peer user's) id,
-- after kind. Only the token has a foreign key: a deleted federation
-- must leave its entry behind, inactive, since dropping it could empty
-- the scope and so widen the token to every exposed group; the groups
-- and users live in the logger's schema.
CREATE TABLE web_mcp_token_scopes (
	token_id   BIGINT UNSIGNED NOT NULL COMMENT 'web_mcp_tokens.id.',
	kind       ENUM('federation', 'group', 'private') NOT NULL
	                           COMMENT 'What target_id names.',
	target_id  BIGINT          NOT NULL
	                           COMMENT 'Federation id, group chat id (negative) or private chat (user) id.',
	created_at DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,

	PRIMARY KEY (token_id, kind, target_id),
	CONSTRAINT fk_web_mcp_token_scopes_token FOREIGN KEY (token_id)
		REFERENCES web_mcp_tokens (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='What each MCP token may read; none is every exposed group.';
