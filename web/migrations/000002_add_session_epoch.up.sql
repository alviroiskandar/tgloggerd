-- Per-account session epoch. It is embedded in every issued session cookie and
-- checked against this column on every authenticated request. Bumping it (done
-- on a password change) invalidates all previously issued cookies -- i.e. signs
-- the account out everywhere -- while the session that performed the change is
-- handed a fresh cookie carrying the new epoch, so it stays signed in.
--
-- Additive migration (not an edit-in-place of 000001) so the existing web_users
-- rows -- the seeded admin account -- are preserved.
ALTER TABLE web_users
	ADD COLUMN session_epoch INT UNSIGNED NOT NULL DEFAULT 0
		COMMENT 'Bumped on password change to invalidate old session cookies.'
		AFTER is_active;
