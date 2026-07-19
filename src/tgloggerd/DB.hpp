// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#ifndef TGLOGGERD__DB_HPP
#define TGLOGGERD__DB_HPP

#include <mysql/Database.hpp>
#include <tgloggerd/models/User.hpp>
#include <tgloggerd/models/File.hpp>
#include <tgloggerd/models/Group.hpp>
#include <tgloggerd/models/PrivateMessage.hpp>
#include <tgloggerd/models/GroupMessage.hpp>
#include <tgloggerd/models/GroupAdmin.hpp>
#include <tgloggerd/models/Backfill.hpp>

#include <vector>

namespace tgloggerd {

/*
 * tgloggerd::DB is the tgloggerd-specific database facade. It maps the
 * tgloggerd models onto SQL statements executed through the generic
 * mysql::Database, so the rest of tgloggerd never writes SQL directly.
 */
class DB {
public:
	explicit DB(const mysql::Config &cfg);
	~DB(void);

	/* Verify connectivity; throws std::runtime_error on failure. */
	void ping(void);

	/*
	 * Insert or update a user together with its usernames, atomically.
	 * Does not touch users.profile_photo_file_id, which is managed
	 * separately once the profile photo has been downloaded.
	 */
	void upsertUser(const models::User &u);

	/*
	 * Apply td_api::userFullInfo fields (bio, birthdate, personal chat)
	 * onto an existing users row, recording the previous bio in
	 * user_hist_bio when it changes. No-op if the user row is absent.
	 */
	void upsertUserFullInfo(const models::UserFullInfo &fi);

	/*
	 * Insert a file, or, if a row with the same SHA-256 already exists,
	 * bump its hit_count. Returns the files.id in both cases.
	 */
	uint64_t upsertFile(const models::File &f);

	/* Point a user's profile_photo_file_id at a files row. */
	void setUserProfilePhoto(int64_t user_id, uint64_t file_id);

	/*
	 * Insert or update a group together with its usernames, atomically,
	 * recording title/description/username changes in the history
	 * tables. Does not touch groups.photo_file_id, which is managed by
	 * setGroupPhoto.
	 */
	void upsertGroup(const models::Group &g);

	/* Point a group's photo_file_id at a files row. */
	void setGroupPhoto(int64_t group_id, uint64_t file_id);

	/*
	 * Replace a group's stored administrator set with a freshly fetched
	 * one, recording added/removed/privilege-change events in
	 * group_admin_hist. Only call with a genuinely fetched list: an empty
	 * list removes all stored admins, so an errored fetch must not reach
	 * here.
	 */
	void syncGroupAdmins(const models::GroupAdminList &list);

	/*
	 * Insert or update a private-chat message. Handles:
	 *  - First-seen messages (insert).
	 *  - Edits (copies old row into private_message_edits, then
	 *    updates private_messages).
	 *  - Deletions (stamps deleted_at, keeps the row).
	 *  - Forward info (inserts into private_message_fwd_info if
	 *    present and not already recorded).
	 */
	void upsertPrivateMessage(const models::PrivateMessage &msg);

	/*
	 * Insert or update a group-chat message. Same semantics as
	 * upsertPrivateMessage, targeting the group_messages tables.
	 */
	void upsertGroupMessage(const models::GroupMessage &msg);

	/*
	 * Point a message's file_id at a files row, once its media
	 * attachment has been downloaded. Managed separately from the
	 * content upsert so an edit rebuild never clears the link.
	 */
	void setPrivateMessageFile(int64_t chat_id, int64_t message_id,
				   uint64_t file_id);
	void setGroupMessageFile(int64_t chat_id, int64_t message_id,
				 uint64_t file_id);

	/*
	 * Link a message to the one it replies to. Records the replied
	 * message's (reply_to_chat_id, reply_to_message_id) universally, and
	 * resolves the surrogate-id FK reply_to_id when the replied message
	 * is in the same table. The replied message should be saved first so
	 * reply_to_id resolves (cross-table replies leave it NULL).
	 */
	void setPrivateMessageReply(int64_t chat_id, int64_t message_id,
				    int64_t reply_to_chat_id,
				    int64_t reply_to_message_id);
	void setGroupMessageReply(int64_t chat_id, int64_t message_id,
				  int64_t reply_to_chat_id,
				  int64_t reply_to_message_id);

	/*
	 * Load every backfill-progress row so the backfiller can resume its
	 * per-chat newest->oldest history walks after a restart.
	 */
	std::vector<models::BackfillState> loadBackfillState(void);

	/*
	 * Insert or update a chat's backfill progress (cursor and done flag),
	 * stamping last_fetch_at. Keyed by chat_id.
	 */
	void upsertBackfillState(const models::BackfillState &st);

private:
	void syncUsernames(mysql::Transaction &tx, const models::User &u);
	void trackProfilePhotoChange(mysql::Transaction &tx,
				     int64_t user_id, uint64_t file_id);

	/*
	 * user_extra_info holds the sparse per-user fields split out of the
	 * users table. It is written from two sources -- the user object (most
	 * fields) and userFullInfo (bio, personal_chat_id) -- so each helper
	 * upserts only the columns it owns and never clobbers the other's.
	 * After either write, pruneUserExtraIfEmpty drops the row when every
	 * column is back at its default, so a row exists only when something is
	 * set. personal_chat_id is stored only when its group is already known
	 * (else NULL), keeping the FK to groups satisfiable.
	 */
	void upsertUserExtraFromUser(mysql::Transaction &tx,
				     const models::User &u);
	void upsertUserExtraFromFullInfo(mysql::Transaction &tx,
					 const models::UserFullInfo &fi);
	void pruneUserExtraIfEmpty(mysql::Transaction &tx, int64_t user_id);
	void syncGroupUsernames(mysql::Transaction &tx, const models::Group &g);
	void trackGroupPhotoChange(mysql::Transaction &tx,
				   int64_t group_id, uint64_t file_id);

	/*
	 * Append a snapshot of a free-text field (a user bio, a group
	 * description) to its history table when it is worth recording: the
	 * value is non-empty and differs from the most recent snapshot. Unlike
	 * the name/phone history, this records the value as observed -- so the
	 * first real value (which arrives with full info, after the row is
	 * created empty) is captured, and empty placeholders never are. The
	 * table and columns are compile-time literals, never user input.
	 */
	void recordTextHistory(mysql::Transaction &tx, const char *table,
			       const char *fk_column, const char *value_column,
			       int64_t entity_id, const std::string &value);

	/*
	 * Shared message-upsert helpers, parameterized by table and
	 * foreign-key column so the identical private/group logic is not
	 * duplicated. The table and column arguments are compile-time
	 * literals, never user input.
	 */
	bool snapshotMessageEditIfChanged(mysql::Transaction &tx,
					  const char *edits_table,
					  const char *fk_column,
					  uint64_t message_row_id,
					  const models::MessageContent &old_content,
					  int64_t old_edit_date,
					  const models::MessageContent &new_content,
					  int64_t new_edit_date);
	void insertForwardInfo(mysql::Transaction &tx, const char *table,
			       const char *fk_column, uint64_t message_row_id,
			       const models::ForwardInfo &info);

	mysql::Database db_;
};

} /* namespace tgloggerd */

#endif /* #ifndef TGLOGGERD__DB_HPP */
