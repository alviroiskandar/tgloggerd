-- The video of an animated profile or group photo (docs/todo/
-- profile-photo-videos.md): Telegram profile photos can be short MPEG-4
-- videos, and the history kept only their still. video_file_id is the
-- telegram_files row of the video (the big animation, else the small
-- one), NULL for a still photo and for rows recorded before. Neither
-- table has a FULLTEXT index, and both are small.
ALTER TABLE telegram_user_hist_profile_photo
	ADD COLUMN video_file_id BIGINT UNSIGNED NULL DEFAULT NULL
		COMMENT 'FK to telegram_files.id: the video of an animated photo; NULL if none.'
		AFTER file_id,
	ADD KEY idx_user_hist_profile_photo_video (video_file_id),
	ADD CONSTRAINT fk_user_hist_profile_photo_video FOREIGN KEY (video_file_id)
		REFERENCES telegram_files (id) ON DELETE SET NULL ON UPDATE CASCADE;

ALTER TABLE telegram_group_hist_photo
	ADD COLUMN video_file_id BIGINT UNSIGNED NULL DEFAULT NULL
		COMMENT 'FK to telegram_files.id: the video of an animated photo; NULL if none.'
		AFTER file_id,
	ADD KEY idx_group_hist_photo_video (video_file_id),
	ADD CONSTRAINT fk_group_hist_photo_video FOREIGN KEY (video_file_id)
		REFERENCES telegram_files (id) ON DELETE SET NULL ON UPDATE CASCADE;
