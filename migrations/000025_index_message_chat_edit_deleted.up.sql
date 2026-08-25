-- Index (chat_id, edit_date) and (chat_id, deleted_at) on the message tables.
--
-- Migration 000024 made "one group, one time window" cheap for the SEND time.
-- These do the same for the two other timestamps a message carries, which the
-- MCP tools can now filter and order on: when it was last edited, and when its
-- deletion was observed.
--
-- Those are genuinely different questions from `date`. A message sent in
-- January and edited in August is invisible to any window on `date`, so
-- "what changed this week?" and "what was deleted this week?" were not
-- expressible at all before, and are the queries these indexes serve.
--
-- Measured on the 5.0M-row table, one group of 238k messages, asking for the
-- most recently edited 200 messages of the last 7 days ordered by edit_date:
--
--                                                   before      after
--   edited in a window, ordered by edit_date         6.18 s     0.012 s
--   count of the same                                5.11 s     0.0005 s
--   deleted in a window, ordered by deleted_at       0.83 s     (range scan)
--
-- The 6.18 s figure is not a worst case: it exceeds the MCP tools' 5-second
-- statement timeout outright, so before this the query could not complete at
-- all. With edit_date following chat_id the range is one contiguous stretch of
-- index walked in reverse, which also supplies the ORDER BY for free -- the
-- same shape as 000024, for a different column.
--
-- NOTE: the gain only materialises because the exposure gate now binds a
-- literal list of allowed group ids instead of a `chat_id IN (SELECT ...)`
-- semi-join. With the subquery, MySQL drove the join from the allowlist table
-- and estimated ~90 rows per group against a real 238k, so it never chose
-- these indexes; measured at 6.18 s WITH this index present. The two changes
-- are a pair, and reverting either one alone puts the query back over the
-- timeout.
--
-- Cost is roughly 9% index growth per index. Created with online DDL (MySQL 8+
-- defaults to ALGORITHM=INPLACE, LOCK=NONE for a secondary index), so the
-- daemon keeps writing while they build; measured at 24 seconds each.
ALTER TABLE telegram_group_messages
	ADD INDEX idx_group_messages_chat_edit (chat_id, edit_date),
	ADD INDEX idx_group_messages_chat_deleted (chat_id, deleted_at);

-- The private table gets the same pair for symmetry with migrations 000023 and
-- 000024, and because it is tiny (5k rows). No MCP tool reads it -- DMs are out
-- of scope there -- but the web UI asks the same shape of question of it.
ALTER TABLE telegram_private_messages
	ADD INDEX idx_private_messages_chat_edit (chat_id, edit_date),
	ADD INDEX idx_private_messages_chat_deleted (chat_id, deleted_at);
