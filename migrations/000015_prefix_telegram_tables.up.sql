-- Namespace every Telegram-platform table with a `telegram_` prefix, mirroring
-- the existing `discord_` prefix. As the project integrates multiple messaging
-- platforms, a per-platform prefix keeps each platform's tables grouped and
-- unambiguous (see web/docs/db-naming.md for the convention).
--
-- These are pure renames: ALTER TABLE ... RENAME preserves all rows, indexes
-- and triggers, and InnoDB automatically rewrites the foreign keys that
-- reference a renamed table to point at its new name. Foreign-key *constraint*
-- names are left as-is (cosmetic; they do not affect behaviour). Tables already
-- prefixed (discord_*) and the golang-migrate bookkeeping table
-- (schema_migrations) are intentionally untouched.
ALTER TABLE `groups`                     RENAME TO telegram_groups;
ALTER TABLE users                        RENAME TO telegram_users;
ALTER TABLE user_usernames               RENAME TO telegram_user_usernames;
ALTER TABLE user_extra_info              RENAME TO telegram_user_extra_info;
ALTER TABLE user_hist_bio                RENAME TO telegram_user_hist_bio;
ALTER TABLE user_hist_name               RENAME TO telegram_user_hist_name;
ALTER TABLE user_hist_phone_num          RENAME TO telegram_user_hist_phone_num;
ALTER TABLE user_hist_profile_photo      RENAME TO telegram_user_hist_profile_photo;
ALTER TABLE user_hist_usernames_events   RENAME TO telegram_user_hist_usernames_events;
ALTER TABLE group_usernames              RENAME TO telegram_group_usernames;
ALTER TABLE group_admins                 RENAME TO telegram_group_admins;
ALTER TABLE group_admin_hist             RENAME TO telegram_group_admin_hist;
ALTER TABLE group_hist_title             RENAME TO telegram_group_hist_title;
ALTER TABLE group_hist_description       RENAME TO telegram_group_hist_description;
ALTER TABLE group_hist_photo             RENAME TO telegram_group_hist_photo;
ALTER TABLE group_hist_usernames_events  RENAME TO telegram_group_hist_usernames_events;
ALTER TABLE private_messages             RENAME TO telegram_private_messages;
ALTER TABLE private_message_edits        RENAME TO telegram_private_message_edits;
ALTER TABLE private_message_fwd_info     RENAME TO telegram_private_message_fwd_info;
ALTER TABLE group_messages               RENAME TO telegram_group_messages;
ALTER TABLE group_message_edits          RENAME TO telegram_group_message_edits;
ALTER TABLE group_message_fwd_info       RENAME TO telegram_group_message_fwd_info;
ALTER TABLE files                        RENAME TO telegram_files;
ALTER TABLE chat_backfill_state          RENAME TO telegram_chat_backfill_state;
