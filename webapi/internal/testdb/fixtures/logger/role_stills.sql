-- Dummy videos that are no message's own file but play a role in one
-- (a rich message's block, an animated chat photo, a link preview's
-- medium), for the pictures the files list gives them (the role
-- stills tests), loaded after "sample" (its users are the senders).
-- Invented; nothing here comes from a real archive.
--
-- Ids, beside the sample's:
--   groups     -1001000000090 (Role Stills, supergroup, not exposed),
--              -1001000000091 (Hidden Stills, supergroup, not exposed)
--   files      897001 - 897010 the videos, 897101 - 897111 the pictures
--   messages   group rows 5401 - 5407 (Role Stills), 5409 (Hidden
--              Stills); private row 5410 (with sample user 9299000002)
--
-- By video:
--   897001  5401's rich video at position 1, its thumbnail at position
--           1 stored (897101): that is its still
--   897002  5401's rich animation at position 2, its thumbnail (897102)
--           not stored: none
--   897003  5402's animated chat photo (chat_photo position 1), the
--           still photo at position 0 stored (897103)
--   897004  5403's video link preview's medium (preview_media position
--           0), its cover at position 1 (897104) and the card's photo
--           (897105) stored: the cover wins
--   897005  5404's animation link preview's medium, the card's photo
--           stored (897106)
--   897006  5405's album link preview's first item (a video), the
--           second item a photo (897107) at position 1: not a cover,
--           so none
--   897007  5406's own video with its thumbnail (897108), and 5401's
--           rich video at position 3 with a thumbnail (897109): the
--           own message's picture comes first
--   897008  5407's paid video: none (paid items have no still)
--   897009  5401's rich video at position 4 without a thumbnail, and
--           Hidden Stills' 5409's at position 0 with one (897110)
--   897010  the private rich video of 5410 at position 0, its
--           thumbnail stored (897111)
-- 897011 is 5401's rich photo at position 0, which has no thumbnail.

INSERT INTO telegram_files (id, tg_file_id, tg_unique_id, file_type, file_size, on_disk,
	sha256, file_ext, orig_file_name, created_at, accessed_at)
WITH RECURSIVE seq (n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < 11)
SELECT 897000 + n, CONCAT('rstill-', 897000 + n), CONCAT('rstill-u-', 897000 + n),
	ELT(n, 'video', 'animation', 'animation', 'video', 'animation', 'video', 'video',
	 'video', 'video', 'video', 'photo'),
	500000, 1, UNHEX(SHA2(CONCAT('rstill ', 897000 + n), 256)),
	IF(n = 11, 'jpg', 'mp4'), '', '2026-11-17 10:00:00', '2026-11-17 10:00:00'
FROM seq;

INSERT INTO telegram_files (id, tg_file_id, tg_unique_id, file_type, file_size, on_disk,
	sha256, file_ext, orig_file_name, created_at, accessed_at)
WITH RECURSIVE seq (n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < 11)
SELECT 897100 + n, CONCAT('rstill-', 897100 + n), CONCAT('rstill-u-', 897100 + n),
	IF(n IN (1, 2, 8, 9, 10, 11), 'thumbnail', 'photo'), 9000, IF(n = 2, 0, 1),
	IF(n = 2, NULL, UNHEX(SHA2(CONCAT('rstill ', 897100 + n), 256))),
	'jpg', '', '2026-11-17 10:00:00', '2026-11-17 10:00:00'
FROM seq;

INSERT INTO telegram_groups (id, type, title, description, msg_count, created_at, updated_at)
	VALUES (-1001000000090, 'supergroup', 'Role Stills', 'Videos in their roles.', 7,
	 '2025-05-05 05:05:00', '2026-11-17 15:00:00'),
	(-1001000000091, 'supergroup', 'Hidden Stills', 'A video seen elsewhere.', 1,
	 '2025-05-05 05:05:00', '2026-11-17 15:00:00');

INSERT INTO telegram_group_messages (id, chat_id, message_id, sender_user_id, date,
	content_type, text, file_id) VALUES
	(5401, -1001000000090, 720001 << 20, 9299000001, 1794931200 - 3000, 'text', 'rich', NULL),
	(5402, -1001000000090, 720002 << 20, 9299000001, 1794931200 - 2940, 'service', NULL, NULL),
	(5403, -1001000000090, 720003 << 20, 9299000001, 1794931200 - 2880, 'text',
	 'https://video.example.org/a', NULL),
	(5404, -1001000000090, 720004 << 20, 9299000001, 1794931200 - 2820, 'text',
	 'https://gif.example.org/b', NULL),
	(5405, -1001000000090, 720005 << 20, 9299000001, 1794931200 - 2760, 'text',
	 'https://album.example.org/c', NULL),
	(5406, -1001000000090, 720006 << 20, 9299000001, 1794931200 - 2700, 'video', NULL, 897007),
	(5407, -1001000000090, 720007 << 20, 9299000001, 1794931200 - 2640, 'paid_media', NULL,
	 NULL),
	(5409, -1001000000091, 720009 << 20, 9299000001, 1794931200 - 2520, 'text', 'rich', NULL);

INSERT INTO telegram_private_messages (id, chat_id, message_id, sender_id, is_outgoing, date,
	content_type, text, file_id) VALUES
	(5410, 9299000002, 720010 << 20, 9299000002, 0, 1794931200 - 2460, 'text', 'rich', NULL);

INSERT INTO telegram_message_files (group_message_id, private_message_id, role, position,
	file_id) VALUES
	(5401, NULL, 'rich', 0, 897011),
	(5401, NULL, 'rich', 1, 897001),
	(5401, NULL, 'thumbnail', 1, 897101),
	(5401, NULL, 'rich', 2, 897002),
	(5401, NULL, 'thumbnail', 2, 897102),
	(5401, NULL, 'rich', 3, 897007),
	(5401, NULL, 'thumbnail', 3, 897109),
	(5401, NULL, 'rich', 4, 897009),
	(5402, NULL, 'chat_photo', 0, 897103),
	(5402, NULL, 'chat_photo', 1, 897003),
	(5403, NULL, 'preview_media', 0, 897004),
	(5403, NULL, 'preview_media', 1, 897104),
	(5404, NULL, 'preview_media', 0, 897005),
	(5405, NULL, 'preview_media', 0, 897006),
	(5405, NULL, 'preview_media', 1, 897107),
	(5406, NULL, 'thumbnail', 0, 897108),
	(5407, NULL, 'paid', 0, 897008),
	(5409, NULL, 'rich', 0, 897009),
	(5409, NULL, 'thumbnail', 0, 897110),
	(NULL, 5410, 'rich', 0, 897010),
	(NULL, 5410, 'thumbnail', 0, 897111);

INSERT INTO telegram_link_previews (group_message_id, url, display_url, site_name, title,
	type, photo_file_id) VALUES
	(5403, 'https://video.example.org/a', 'video.example.org/a', 'Example Video', 'A',
	 'video', 897105),
	(5404, 'https://gif.example.org/b', 'gif.example.org/b', 'Example GIF', 'B',
	 'animation', 897106),
	(5405, 'https://album.example.org/c', 'album.example.org/c', 'Example Album', 'C',
	 'album', NULL);
