-- Drops the photo videos' links; their telegram_files rows stay.
ALTER TABLE telegram_user_hist_profile_photo
	DROP FOREIGN KEY fk_user_hist_profile_photo_video,
	DROP KEY idx_user_hist_profile_photo_video,
	DROP COLUMN video_file_id;

ALTER TABLE telegram_group_hist_photo
	DROP FOREIGN KEY fk_group_hist_photo_video,
	DROP KEY idx_group_hist_photo_video,
	DROP COLUMN video_file_id;
