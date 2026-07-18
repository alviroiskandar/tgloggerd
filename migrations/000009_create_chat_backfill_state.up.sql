-- Progress state for the background message backfiller: one row per chat it
-- has registered. The backfiller walks each chat's history newest -> oldest;
-- cursor_msg_id is the oldest TdLib LOCAL message id fetched so far
-- (getChatHistory paginates by local id, unlike the server ids stored on the
-- message rows), and done = 1 once the chat's history start is reached. This
-- lets the walk resume across restarts and round-robin across chats.

CREATE TABLE chat_backfill_state (
	-- The chat being backfilled: a groups.id (negative) or a peer users.id.
	chat_id       BIGINT       NOT NULL COMMENT 'groups.id or peer users.id.',

	-- Which message table this chat maps to.
	scope         ENUM('group', 'private') NOT NULL COMMENT 'group_messages or private_messages.',

	-- Oldest TdLib LOCAL message id fetched so far; NULL = not started
	-- (fetch from the newest message). Not a server id.
	cursor_msg_id BIGINT       NULL COMMENT 'Oldest TdLib local msg id fetched; NULL = start at newest.',

	-- History start reached; nothing older remains to fetch.
	done          TINYINT(1)   NOT NULL DEFAULT 0 COMMENT 'History start reached.',

	-- When a page was last fetched for this chat.
	last_fetch_at DATETIME     NULL COMMENT 'Last time a page was fetched for this chat.',

	created_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'Row creation time.',
	updated_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP
	                           ON UPDATE CURRENT_TIMESTAMP COMMENT 'Last time the row was updated.',

	PRIMARY KEY (chat_id),
	KEY idx_chat_backfill_state_done (done)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Backward-walk progress for the background message backfiller.';
