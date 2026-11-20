-- Stills generated from a stored video (docs/todo/video-thumbnails.md,
-- "Frames"; decision 2026-11-21-video-stills-format): a video, an
-- animation, a video note or a video sticker that has neither a cover nor
-- a Telegram thumbnail gets a picture taken from its stored bytes by the
-- stills service. The picture is a telegram_files row of its own
-- (file_type 'thumbnail', tg_file_id 'generated:<sha256>', no unique id)
-- linked to the video's messages as their role 'thumbnail'.
--
-- generated_from_file_id: the telegram_files row of the video the still
-- was taken from; NULL for every file that came from Telegram, and again
-- if that video's row is deleted. It marks a still as one the daemon
-- made (there is nothing to fetch from Telegram for it) and finds the
-- still of a video again when another message carries the same video.
--
-- Additive: telegram_files holds real data. The column is added
-- instantly (ALGORITHM=INSTANT, metadata only). The key and the foreign
-- key cannot be: a secondary index is built in place (ALGORITHM=INPLACE,
-- LOCK=NONE, reads and writes go on), and a foreign key is added in place
-- only with foreign_key_checks off. With it on MySQL copies the table,
-- so the session turns it off for that statement: every row's new column
-- is NULL, which no check could refuse.
ALTER TABLE telegram_files
	ADD COLUMN generated_from_file_id BIGINT UNSIGNED NULL DEFAULT NULL
		COMMENT 'FK to telegram_files.id: the video a generated still was taken from; NULL for files from Telegram.',
	ALGORITHM=INSTANT;

ALTER TABLE telegram_files
	ADD KEY idx_files_generated_from (generated_from_file_id),
	ALGORITHM=INPLACE, LOCK=NONE;

SET foreign_key_checks = 0;
ALTER TABLE telegram_files
	ADD CONSTRAINT fk_files_generated_from FOREIGN KEY (generated_from_file_id)
		REFERENCES telegram_files (id) ON DELETE SET NULL,
	ALGORITHM=INPLACE, LOCK=NONE;
SET foreign_key_checks = 1;
