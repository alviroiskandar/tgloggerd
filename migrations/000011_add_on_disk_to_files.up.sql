-- Storage is capped (TG_MAX_STORE_FILE_SIZE): files at or above the cap are
-- still recorded, but their bytes are not kept in the store. `on_disk` marks
-- whether the content is present: TRUE = stored under TG_STORAGE_DIR, FALSE =
-- metadata-only (too large), re-downloadable from Telegram by tg_file_id.
--
-- Additive migration (not an edit-in-place of 000001) because existing rows --
-- including the metadata of files already removed from the store -- must be
-- preserved.
ALTER TABLE files
	ADD COLUMN on_disk BOOLEAN NOT NULL DEFAULT TRUE
		COMMENT 'Whether the file bytes are present in the store; FALSE = metadata-only (too large), re-downloadable by tg_file_id.'
		AFTER file_size,
	ADD KEY idx_files_on_disk (on_disk);

-- Files already deleted from the store because they exceed the 1 GiB cap keep
-- their rows but no longer have bytes on disk.
UPDATE files SET on_disk = FALSE WHERE file_size >= 1073741824;
