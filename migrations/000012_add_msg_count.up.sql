-- Per-entity message counters. `users.msg_count` counts messages a user has
-- sent (in groups and private chats); `groups.msg_count` counts messages sent
-- to the group. They drive a periodic full-info refetch (every 10th message).
--
-- Additive migration (not an edit-in-place of 000002/000005) because the
-- existing user and group rows must be preserved; the counters are back-filled
-- from the recorded messages by scripts/prefill_msg_count.py.
ALTER TABLE users
	ADD COLUMN msg_count BIGINT UNSIGNED NOT NULL DEFAULT 0
		COMMENT 'Messages sent by this user (group + private); drives periodic full-info refetch.'
		AFTER birthday_year;

ALTER TABLE `groups`
	ADD COLUMN msg_count BIGINT UNSIGNED NOT NULL DEFAULT 0
		COMMENT 'Messages sent to this group; drives periodic full-info refetch.'
		AFTER photo_file_id;
