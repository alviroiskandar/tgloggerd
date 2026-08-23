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
#include <string>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace tgloggerd {

/*
 * tgloggerd::DB is the tgloggerd-specific database facade. It maps the
 * tgloggerd models onto SQL statements executed through the generic
 * mysql::Database, so the rest of tgloggerd never writes SQL directly.
 */

/*
 * Returned by the message upserts: a user and/or group whose msg_count just
 * crossed a multiple of 10 on this insert, so its full info should be
 * refetched. Empty on edits/deletes (no new message was recorded).
 */
struct MsgCountRefetch {
	std::optional<int64_t> user;
	std::optional<int64_t> group;
};

/* One enabled Discord webhook integration: mirror chat_id -> webhook_url. */
struct DiscordWebhook {
	int64_t     chat_id;
	std::string webhook_url;
};

/* A quoted (replied-to) message, for rendering a reply in a forward. */
struct QuotedMessage {
	std::string sender_name;     /* may be empty (unknown/own) */
	std::string text;            /* may be empty (media with no caption) */
	int64_t     sender_id = 0;      /* user sender; 0 if a chat/channel sent it */
	int64_t     sender_chat_id = 0; /* chat/channel sender; 0 if a user */
};

/* A chat's current photo (telegram_files.id) and title, for a channel/group sender. */
struct ChatPhoto {
	std::optional<uint64_t> photo_file_id;
	std::string             title;
};

/* A stored file's category/extension/availability, for media forwarding. */
struct FileInfo {
	std::string file_type; /* telegram_files.file_type: "photo", "video", ... */
	std::string ext;       /* lowercase extension without dot; may be empty */
	bool        on_disk;   /* false = metadata-only, no servable bytes */
};

/* A Discord message the forwarder posted, for applying a later edit/delete. */
struct SentMessage {
	std::string webhook_url;
	std::string discord_message_id;
	std::string kind; /* "text" or "media": how to re-derive its content */
};

/* The source-message fields needed to re-render a forward (for a delete
 * tombstone, which rebuilds the content instead of storing it). */
struct MessageForward {
	std::string             text;              /* message text/caption */
	int64_t                 reply_to_chat_id = 0;
	int64_t                 reply_to_msg_id = 0;
	std::optional<uint64_t> file_id;           /* media attachment, if any */
};

class DB {
public:
	explicit DB(const mysql::Config &cfg);
	~DB(void);

	/* Verify connectivity; throws std::runtime_error on failure. */
	void ping(void);

	/* Load all enabled Discord webhook integrations (telegram_discord_webhooks). */
	std::vector<DiscordWebhook> loadDiscordWebhooks(void);

	/*
	 * Telegram user ids of the bots discordd forwards Discord messages
	 * with (telegram_bots.bot_user_id, populated at bot login).
	 *
	 * These exist to break the other half of the bridge loop. A Discord
	 * message that discordd delivers into Telegram becomes an ordinary
	 * new Telegram message, which this daemon would then mirror straight
	 * back into the Discord channel it came from -- the sender sees their
	 * own message repeated by the webhook. Messages sent by these bots are
	 * therefore not forwarded.
	 */
	std::vector<int64_t> loadForwardingBotUserIds(void);

	/* Discord-forwarder lookups (see DiscordForwarder). */
	std::optional<uint64_t> getUserPhotoFileId(int64_t user_id);
	ChatPhoto getGroupPhoto(int64_t chat_id);
	std::optional<QuotedMessage> getQuotedMessage(int64_t chat_id,
						      int64_t message_id);
	std::optional<FileInfo> getFileInfo(uint64_t files_id);
	/* The source message's text/reply/file, to re-render it for a tombstone. */
	std::optional<MessageForward> getMessageForward(int64_t chat_id,
							int64_t message_id);

	/* Record a forwarded Discord message so a later edit/delete can find it.
	 * The webhook URL is interned into discord_endpoints and stored by id. */
	void recordSentMessage(int64_t chat_id, int64_t message_id,
			       const std::string &webhook_url,
			       const std::string &discord_message_id,
			       const char *kind);
	/* The Discord messages posted for a source message; kind=nullptr = all. */
	std::vector<SentMessage> getSentMessages(int64_t chat_id,
						 int64_t message_id,
						 const char *kind);
	/* Drop the tracking rows for a message (after it is deleted/tombstoned). */
	void deleteSentMessages(int64_t chat_id, int64_t message_id);

	/*
	 * Insert or update a user together with its usernames, atomically.
	 * Does not touch telegram_users.profile_photo_file_id, which is managed
	 * separately once the profile photo has been downloaded.
	 */
	void upsertUser(const models::User &u);

	/*
	 * Apply td_api::userFullInfo fields (bio, birthdate, personal chat)
	 * onto an existing users row, recording the previous bio in
	 * telegram_user_hist_bio when it changes. No-op if the user row is absent.
	 */
	void upsertUserFullInfo(const models::UserFullInfo &fi);

	/*
	 * Insert a file, or, if a row with the same SHA-256 already exists,
	 * bump its hit_count. Returns the telegram_files.id in both cases.
	 */
	uint64_t upsertFile(const models::File &f);

	/*
	 * Load the full tg_file_id -> telegram_files.id index, so the daemon can link a
	 * message to a file it already recorded without re-downloading it.
	 */
	std::unordered_map<std::string, uint64_t> loadFileIndex(void);

	/* Point a user's profile_photo_file_id at a files row. */
	void setUserProfilePhoto(int64_t user_id, uint64_t file_id);

	/*
	 * Insert or update a group together with its usernames, atomically,
	 * recording title/description/username changes in the history
	 * tables. Does not touch telegram_groups.photo_file_id, which is managed by
	 * setGroupPhoto.
	 */
	void upsertGroup(const models::Group &g);

	/* Point a group's photo_file_id at a files row. */
	void setGroupPhoto(int64_t group_id, uint64_t file_id);

	/*
	 * Replace a group's stored administrator set with a freshly fetched
	 * one, recording added/removed/privilege-change events in
	 * telegram_group_admin_hist. Only call with a genuinely fetched list: an empty
	 * list removes all stored admins, so an errored fetch must not reach
	 * here.
	 */
	void syncGroupAdmins(const models::GroupAdminList &list);

	/*
	 * Insert or update a private-chat message. Handles:
	 *  - First-seen messages (insert).
	 *  - Edits (copies old row into telegram_private_message_edits, then
	 *    updates telegram_private_messages).
	 *  - Deletions (stamps deleted_at, keeps the row).
	 *  - Forward info (inserts into telegram_private_message_fwd_info if
	 *    present and not already recorded).
	 */
	MsgCountRefetch upsertPrivateMessage(const models::PrivateMessage &msg);

	/*
	 * Insert or update a group-chat message. Same semantics as
	 * upsertPrivateMessage, targeting the telegram_group_messages tables.
	 */
	MsgCountRefetch upsertGroupMessage(const models::GroupMessage &msg);

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
	 * telegram_user_extra_info holds the sparse per-user fields split out of the
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

	/*
	 * Increment `<table>.msg_count` for row `id` and return whether the new
	 * value is a positive multiple of 10 (i.e. this message just crossed a
	 * refetch boundary). `table` is a compile-time literal, never user
	 * input.
	 */
	bool bumpMsgCount(mysql::Transaction &tx, const char *table, int64_t id);

	/*
	 * Return discord_endpoints.id for a webhook URL, inserting it on first
	 * use. The dictionary is tiny and append-only, so results are memoized
	 * in endpoint_ids_ (guarded by endpoint_mtx_) to keep recording cheap.
	 */
	uint64_t internEndpoint(const std::string &webhook_url);

	mysql::Database db_;

	std::mutex				endpoint_mtx_;
	std::unordered_map<std::string, uint64_t> endpoint_ids_;
};

} /* namespace tgloggerd */

#endif /* #ifndef TGLOGGERD__DB_HPP */
