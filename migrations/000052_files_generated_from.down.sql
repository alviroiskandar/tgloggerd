-- Drops the link from a generated still to its video. The stills' rows
-- stay (they are ordinary thumbnails then), as do their message links.
-- One statement each, so that none rebuilds the table: dropping the
-- key is in place, dropping the column instant.
ALTER TABLE telegram_files
	DROP FOREIGN KEY fk_files_generated_from,
	ALGORITHM=INPLACE, LOCK=NONE;

ALTER TABLE telegram_files
	DROP KEY idx_files_generated_from,
	ALGORITHM=INPLACE, LOCK=NONE;

ALTER TABLE telegram_files
	DROP COLUMN generated_from_file_id,
	ALGORITHM=INSTANT;
