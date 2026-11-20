-- Dummy videos with stills the daemon generated from their bytes (the
-- generated stills tests, internal/server/generatedstills_test.go),
-- loaded after "sample" (its users are the senders). A generated still
-- is a thumbnail row with tg_file_id 'generated:<sha256>', no unique id
-- and generated_from_file_id set (migration 000052). Invented; nothing
-- here comes from a real archive.
--
-- Ids, beside the sample's:
--   groups     -1001000000052 (Gen Lab, supergroup, not exposed)
--   files      892001 - 892003 the videos, 892011 - 892015 their stills
--   messages   group rows 5401 - 5404, Telegram message ids
--              (730000 + n) << 20 for row 5400 + n; private row 5451
-- The rows:
--   892001  video, stored; its generated still 892011 was evicted
--           (sha256 kept, bytes gone): message 5401
--   892002  video, stored, sent twice: 5402 has its generated still
--           892012 (stored), 5403 a Telegram thumbnail 892013 (stored,
--           a larger id)
--   892003  video, stored: group message 5404 has its generated still
--           892014 (stored), private message 5451 a Telegram thumbnail
--           892015 (stored, a larger id)

INSERT INTO telegram_files (id, tg_file_id, tg_unique_id, file_type, file_size, on_disk,
	sha256, file_ext, orig_file_name, created_at, accessed_at)
WITH RECURSIVE seq (n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < 3)
SELECT 892000 + n, CONCAT('gen-video-', n), CONCAT('gen-video-u-', n), 'video',
	400000, 1, UNHEX(SHA2(CONCAT('gen video ', n), 256)), 'mp4', '',
	'2026-11-18 10:00:00', '2026-11-18 10:00:00'
FROM seq;

-- The generated stills (892011, 892012, 892014) and the Telegram
-- thumbnails (892013, 892015).
INSERT INTO telegram_files (id, tg_file_id, tg_unique_id, file_type, file_size, on_disk,
	sha256, file_ext, orig_file_name, created_at, accessed_at, generated_from_file_id)
	VALUES
	(892011, CONCAT('generated:', SHA2('gen still 11', 256)), NULL, 'thumbnail', 6000, 0,
	 UNHEX(SHA2('gen still 11', 256)), 'jpg', '',
	 '2026-11-18 10:01:00', '2026-11-18 10:01:00', 892001),
	(892012, CONCAT('generated:', SHA2('gen still 12', 256)), NULL, 'thumbnail', 6000, 1,
	 UNHEX(SHA2('gen still 12', 256)), 'jpg', '',
	 '2026-11-18 10:01:00', '2026-11-18 10:01:00', 892002),
	(892013, 'gen-thumb-13', 'gen-thumb-u-13', 'thumbnail', 9000, 1,
	 UNHEX(SHA2('gen still 13', 256)), 'jpg', '',
	 '2026-11-18 10:02:00', '2026-11-18 10:02:00', NULL),
	(892014, CONCAT('generated:', SHA2('gen still 14', 256)), NULL, 'thumbnail', 6000, 1,
	 UNHEX(SHA2('gen still 14', 256)), 'jpg', '',
	 '2026-11-18 10:01:00', '2026-11-18 10:01:00', 892003),
	(892015, 'gen-thumb-15', 'gen-thumb-u-15', 'thumbnail', 9000, 1,
	 UNHEX(SHA2('gen still 15', 256)), 'jpg', '',
	 '2026-11-18 10:02:00', '2026-11-18 10:02:00', NULL);

INSERT INTO telegram_groups (id, type, title, description, msg_count, created_at, updated_at)
	VALUES (-1001000000052, 'supergroup', 'Gen Lab', 'Videos with generated stills.', 4,
	 '2025-05-06 05:05:00', '2026-11-18 15:00:00');

INSERT INTO telegram_group_messages (id, chat_id, message_id, sender_user_id, date,
	content_type, text, file_id, tdlib_type) VALUES
	(5401, -1001000000052, 730001 << 20, 9299000001, 1794931200 - 400, 'video', NULL, 892001,
	 'messageVideo'),
	(5402, -1001000000052, 730002 << 20, 9299000001, 1794931200 - 300, 'video', NULL, 892002,
	 'messageVideo'),
	(5403, -1001000000052, 730003 << 20, 9299000002, 1794931200 - 200, 'video', NULL, 892002,
	 'messageVideo'),
	(5404, -1001000000052, 730004 << 20, 9299000001, 1794931200 - 100, 'video', NULL, 892003,
	 'messageVideo');

INSERT INTO telegram_private_messages (id, chat_id, message_id, sender_id, is_outgoing, date,
	content_type, text, file_id) VALUES
	(5451, 9299000002, 730051 << 20, 9299000002, 0, 1794931200 - 50, 'video', NULL, 892003);

INSERT INTO telegram_message_files (group_message_id, private_message_id, role, position,
	file_id) VALUES
	(5401, NULL, 'thumbnail', 0, 892011),
	(5402, NULL, 'thumbnail', 0, 892012),
	(5403, NULL, 'thumbnail', 0, 892013),
	(5404, NULL, 'thumbnail', 0, 892014),
	(NULL, 5451, 'thumbnail', 0, 892015);
