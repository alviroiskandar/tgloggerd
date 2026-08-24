-- Index (chat_id, sender_user_id) on the message tables.
--
-- "Which users have ever posted in this group?" is a natural question and an
-- expensive one without this index. The existing keys cover (chat_id,
-- message_id) and (sender_user_id) separately, so answering it meant reading
-- every message in the group and de-duplicating: measured on the 236,490
-- messages of one group, 2.3 seconds to produce 292 distinct senders.
--
-- That cost is proportional to the group's message count, not to the answer,
-- so it degrades exactly where it matters -- the busiest groups are both the
-- most interesting to ask about and the slowest to answer. A group ten times
-- this size would exceed the MCP tools' 5-second statement timeout outright.
--
-- With sender_user_id following chat_id, an equality on chat_id plus a GROUP BY
-- on sender_user_id becomes a loose index scan: MySQL walks one entry per
-- distinct sender instead of one per message, so the work becomes proportional
-- to the number of admins-worth of rows returned rather than the archive.
--
-- Cost is roughly 8% growth in index size on telegram_group_messages. Created
-- with online DDL (MySQL 8+ defaults to ALGORITHM=INPLACE, LOCK=NONE for a
-- secondary index), so the daemon keeps writing while it builds.
ALTER TABLE telegram_group_messages
	ADD INDEX idx_group_messages_chat_sender (chat_id, sender_user_id);

-- The private table gets the same index for symmetry and because it is tiny
-- (6k rows); no MCP tool reads it, but a future one asking the same question
-- of a DM should not have to reason about why only one table is covered.
ALTER TABLE telegram_private_messages
	ADD INDEX idx_private_messages_chat_sender (chat_id, sender_id);
