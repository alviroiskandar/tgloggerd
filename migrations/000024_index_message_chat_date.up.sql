-- Index (chat_id, date, sender_user_id) on the message tables.
--
-- Every "one group, one time window" question currently falls off a cliff.
-- MySQL has to pick between two half-right indexes: uq_group_messages_chat_msg
-- narrows to the chat but then reads all of it to test `date`, and
-- idx_group_messages_chat_sender narrows to the chat but carries no date, so a
-- date bound turns a covering scan into a row fetch per message. Cost ends up
-- proportional to the group's whole history no matter how narrow the window.
--
-- Measured on the 5.0M-row table, one group of 237k messages:
--
--                                              before      after
--   leaderboard, all time                       0.13 s     0.13 s
--   leaderboard, 30-day window                  2.27 s     0.008 s
--   leaderboard, window covering all history    9.03 s     0.22 s
--   count one user's messages, 30-day window    0.44 s     0.009 s
--   read message text, 30-day window            2.55 s     0.03 s
--   read message text, all history (50k cap)    2.55 s     0.88 s
--
-- The 9.03 s case is the one that forced this: it is not an unreasonable
-- request, just a leaderboard whose start_date predates the group, and it
-- already exceeds the MCP tools' 5-second statement timeout on the only
-- currently exposed group. A busier group has no working leaderboard at all.
--
-- With date following chat_id, (chat_id = ?, date BETWEEN ? AND ?) is one
-- contiguous stretch of index, so the scan is proportional to that group's
-- messages in that window. It also orders by date within the chat for free,
-- which removes the filesort from "newest N messages in this window".
--
-- sender_user_id is third so the index *covers* the two counting queries:
-- grouping and counting by sender inside a date window then touches no rows at
-- all. That is the difference between 0.22 s and 9.03 s above -- the range
-- scan was never the expensive part, the 237k row fetches behind it were.
--
-- Cost is roughly 9% growth in index size on telegram_group_messages, and 41
-- seconds to build. Created with online DDL (MySQL 8+ defaults to
-- ALGORITHM=INPLACE, LOCK=NONE for a secondary index), so the daemon keeps
-- writing while it builds.
ALTER TABLE telegram_group_messages
	ADD INDEX idx_group_messages_chat_date (chat_id, date, sender_user_id);

-- The private table gets the same index for symmetry with migration 000023 and
-- because it is tiny (5k rows). No MCP tool reads it -- DMs are out of scope
-- there -- but the web UI asks the same shape of question of it.
ALTER TABLE telegram_private_messages
	ADD INDEX idx_private_messages_chat_date (chat_id, date, sender_id);
