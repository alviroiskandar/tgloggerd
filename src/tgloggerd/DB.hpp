// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#ifndef TGLOGGERD__DB_HPP
#define TGLOGGERD__DB_HPP

#include <mysql/Database.hpp>
#include <tgloggerd/ChatRecheck.hpp>
#include <tgloggerd/ForwardFilter.hpp>
#include <tgloggerd/models/User.hpp>
#include <tgloggerd/models/ContactName.hpp>
#include <tgloggerd/models/File.hpp>
#include <tgloggerd/models/Group.hpp>
#include <tgloggerd/models/PrivateMessage.hpp>
#include <tgloggerd/models/GroupMessage.hpp>
#include <tgloggerd/models/LinkPreview.hpp>
#include <tgloggerd/models/Poll.hpp>
#include <tgloggerd/models/ForumTopic.hpp>
#include <tgloggerd/models/GroupAdmin.hpp>
#include <tgloggerd/models/Backfill.hpp>
#include <tgloggerd/models/Emoji.hpp>
#include <tgloggerd/models/Community.hpp>

#include <utility>
#include <vector>
#include <tuple>
#include <string>
#include <cstdint>
#include <mutex>
#include <map>
#include <optional>
#include <unordered_map>

namespace tgloggerd {

/*
 * tgloggerd::DB is the tgloggerd-specific database facade. It maps the
 * tgloggerd models onto SQL statements executed through the generic
 * mysql::Database, so the rest of tgloggerd never writes SQL directly.
 */

/* What a message upsert did to the row, once it is committed. */
enum class MsgChange {
	None,		/* nothing visible: a re-ingest, a repeated deletion */
	New,		/* the row was inserted */
	Edited,		/* a copy with a later edit_date replaced the content */
	Deleted,	/* deleted_at was stamped */
};

/*
 * Returned by the message upserts: a user and/or group whose msg_count just
 * crossed a multiple of 10 on this insert, so its full info should be
 * refetched (empty on edits/deletes: no new message was recorded), and what
 * the upsert changed, for the event stream.
 */
struct MsgCountRefetch {
	std::optional<int64_t> user;
	std::optional<int64_t> group;
	MsgChange change = MsgChange::None;
	/* A deletion concluded on a recheck was undone: the message came
	 * back (change is then at least Edited). */
	bool restored = false;
};

/* One enabled Discord webhook integration: mirror chat_id -> webhook_url. */
struct DiscordWebhook {
	int64_t     chat_id;
	std::string webhook_url;
	unsigned    types;	/* FWD_* bits this webhook forwards */
};

/* A quoted (replied-to) message, for rendering a reply in a forward. */
struct QuotedMessage {
	std::string sender_name;     /* may be empty (unknown/own) */
	std::string text;            /* may be empty (media with no caption) */
	int64_t     sender_id = 0;      /* user sender; 0 if a chat/channel sent it */
	int64_t     sender_chat_id = 0; /* chat/channel sender; 0 if a user */
};

/* A user sender's photo (telegram_files.id) and stored name columns. */
struct SenderUser {
	std::optional<uint64_t>	photo_file_id;
	models::StoredUserName	name;
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

/* A file row's cache state, for the internal API (GET /v1/files/{id}). */
struct FileStatus {
	uint64_t		id = 0;
	std::string		file_type;	/* "photo", "video", ... */
	std::string		ext;		/* may be empty */
	uint64_t		file_size = 0;
	bool			on_disk = false;
	std::optional<uint64_t>	alias_of;	/* row holding the same bytes */
	bool			has_unique_id = false;	/* tg_unique_id known */
	int64_t			accessed_at = 0;	/* Unix seconds */
};

/* What the store holds: telegram_files rows with on_disk = 1. */
struct StoreTotals {
	uint64_t	files = 0;
	uint64_t	bytes = 0;	/* SUM(file_size), the logical size */
};

/* A stored file the StorageJanitor may evict. */
struct EvictCandidate {
	uint64_t	id = 0;
	std::string	sha256;		/* empty if NULL (no blob to unlink) */
	std::string	ext;		/* may be empty */
	uint64_t	file_size = 0;
	/* As the database printed it; the eviction guard compares it back. */
	std::string	accessed_at;
};

/* A file row as the on-demand fetch service needs it (FileFetcher). */
struct FetchRow {
	uint64_t		id = 0;
	std::string		tg_file_id;
	std::string		tg_unique_id;	/* empty if unknown */
	std::string		file_type;	/* "photo", "video", ... */
	uint64_t		file_size = 0;	/* 0 if unknown */
	std::string		sha256_hex;	/* empty: bytes never seen */
	std::string		ext;		/* may be empty */
	std::string		orig_file_name;
	bool			on_disk = false;
	std::optional<uint64_t>	alias_of;
};

/* A Discord message the forwarder posted, for applying a later edit/delete. */
/*
 * The Discord message a Telegram message was bridged FROM.
 *
 * Present only when discordd posted that Telegram message on behalf of a
 * Discord user (discord_telegram_sent_messages). Ids are carried as strings
 * because Discord snowflakes are 64-bit UNSIGNED and are only ever
 * concatenated into a URL or a mention here -- parsing them into an int64
 * would risk a wrap for no gain.
 */
struct DiscordOrigin {
	std::string message_id;    /* the original Discord message */
	std::string channel_id;
	std::string guild_id;      /* may be empty; then no jump link */
	std::string author_id;     /* may be empty if the message was not logged */
	std::string username;
	std::string discriminator;
	std::string avatar;        /* CDN hash; may be empty */
	std::string content;
};

struct SentMessage {
	std::string webhook_url;
	std::string discord_message_id;
	std::string kind; /* "text" or "media": how to re-derive its content */
};

/* The source-message fields needed to re-render a forward (for a delete
 * tombstone, which rebuilds the content instead of storing it). */
struct MessageForward {
	std::string             text;              /* message text/caption */
	std::string             content_type;      /* its kind, e.g. "poll" */
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

	/*
	 * Every bot of both kinds (telegram_bots, discord_bots) as id -> a
	 * string of what the web shows of it (name, enabled, status and its
	 * detail and time, identity, last change), never the token: two
	 * reads that differ for a bot mean it changed. The ids of the two
	 * tables never meet (migration 000040).
	 */
	std::map<uint64_t, std::string> loadBotSignatures(void);

	/*
	 * (chat_id, message_id) of every live message whose content is a media
	 * kind but which has no file linked -- media whose file row was lost
	 * (a database crash between the copy and the insert, or a reset), or
	 * whose download never finished. Input to TDLib::relinkMessageMedia().
	 * Deleted messages are left out: the server no longer has them.
	 */
	std::vector<std::pair<int64_t, int64_t>> listMediaMessagesWithoutFile(void);

	/*
	 * The live messages stored before content classification whose kind
	 * or detail is missing: content_type 'unknown', or a service message,
	 * with no tdlib_type; and the rich messages stored before their
	 * mapping, whose content_extra has no "blocks" key. Re-reading one
	 * through the normal path gives it a tdlib_type (and a rich one its
	 * "blocks"), so a repaired row is never listed again.
	 */
	std::vector<std::pair<int64_t, int64_t>> listMessagesToRepair(void);

	/* How many rows listMessagesToRepair would return, per table and
	 * content_type, for the dry run: ("group", "unknown") -> n, ... */
	std::vector<std::tuple<std::string, std::string, uint64_t>>
	countMessagesToRepair(void);

	/*
	 * The live messages that should show a still but have none stored:
	 * a video, an animation, a video note or a video (.webm) sticker
	 * with no message file of role "thumbnail" (nor "cover", which a
	 * video shows first). Re-reading one stores the still thumbnail
	 * TDLib offers, so a message that got one is not listed again; one
	 * whose video has no Telegram thumbnail stays listed. Input to
	 * TDLib::refetchThumbnails().
	 */
	std::vector<std::pair<int64_t, int64_t>>
	listMessagesWithoutThumbnail(void);

	/* The statement listMessagesWithoutThumbnail runs, for its plan. */
	static std::string thumbnailCandidatesSql(void);

	/* How many rows listMessagesWithoutThumbnail would return, per table
	 * and content_type, for the dry run: ("group", "video") -> n, ... */
	std::vector<std::tuple<std::string, std::string, uint64_t>>
	countMessagesWithoutThumbnail(void);

	/*
	 * What a chat recheck needs of the stored messages of @chat_id
	 * (telegram_group_messages, or telegram_private_messages unless
	 * @is_group) around @span: the live ones strictly between its ends,
	 * and whether a live one lies below it. Two range reads on the
	 * (chat_id, message_id) unique key.
	 */
	recheck::Stored listRecheckStored(int64_t chat_id, bool is_group,
					  const recheck::Span &span);

	/*
	 * Mark the live messages @message_ids of @chat_id deleted as found by
	 * a recheck: deleted_at and meta.deleted_on_recheck = true in one
	 * UPDATE each (a NULL meta becomes an object). The only writer of
	 * that flag. Returns the ids it marked; one already deleted, or not
	 * stored, is left as it is.
	 */
	std::vector<int64_t> markDeletedOnRecheck(
		int64_t chat_id, bool is_group,
		const std::vector<int64_t> &message_ids);

	/*
	 * Undo markDeletedOnRecheck for those of @message_ids a later
	 * recheck found again: deleted_at back to NULL and the flag removed,
	 * for rows that carry the flag only (a deletion Telegram reported is
	 * never undone). Returns the ids it restored.
	 */
	/*
	 * The newest message id stored for each of @chat_ids (deleted ones
	 * too), for the recheck catch-up's gap test; a chat with nothing
	 * stored is left out. Groups have negative ids, private chats
	 * positive.
	 */
	std::unordered_map<int64_t, int64_t> newestStoredMessageIds(
		const std::vector<int64_t> &chat_ids);

	/*
	 * For each supergroup or channel in @newest (chat -> newest stored
	 * message id), how many messages are stored among the
	 * recheck::kDenseWindow ids up to that newest one; for the catch-up's
	 * test of a gap below the newest message.
	 */
	std::unordered_map<int64_t, int64_t> storedNearNewest(
		const std::unordered_map<int64_t, int64_t> &newest);

	/* How many of @message_ids are stored for @chat_id (deleted or not). */
	size_t countStoredMessages(int64_t chat_id, bool is_group,
				   const std::vector<int64_t> &message_ids);

	std::vector<int64_t> restoreRecheckDeleted(
		int64_t chat_id, bool is_group,
		const std::vector<int64_t> &message_ids);

	/*
	 * Whether the chat is stored: a telegram_groups row for a group, a
	 * telegram_users row for a private chat (its id is the peer's).
	 */
	bool chatKnown(int64_t chat_id);

	/* Discord-forwarder lookups (see DiscordForwarder). */
	std::optional<SenderUser> getSenderUser(int64_t user_id);
	ChatPhoto getGroupPhoto(int64_t chat_id);
	/*
	 * Was this Telegram message bridged in from Discord? Keyed by
	 * (telegram_chat_id, telegram_message_id), which
	 * discord_telegram_sent_messages indexes directly.
	 */
	std::optional<DiscordOrigin> getDiscordOrigin(int64_t chat_id,
						      int64_t message_id);

	std::optional<QuotedMessage> getQuotedMessage(int64_t chat_id,
						      int64_t message_id);
	std::optional<FileInfo> getFileInfo(uint64_t files_id);

	/* A file row's cache state; nullopt if there is no such row. */
	std::optional<FileStatus> getFileStatus(uint64_t files_id);

	/*
	 * Record that the web used these files: accessed_at = NOW() for the
	 * rows whose accessed_at is more than an hour old, keeping
	 * updated_at (an access is not a change). Returns the rows moved.
	 */
	uint64_t touchFiles(const std::vector<uint64_t> &files_ids);

	/*
	 * StorageJanitor queries (models/FileEvict.cpp). Times are DATETIME
	 * strings as the database prints them, so they compare back exactly.
	 */
	/* Stored files; before non-empty: only accessed_at < before. */
	StoreTotals storedTotals(const std::string &before = std::string());
	/* NOW() - INTERVAL days DAY, the retention cutoff. */
	std::string retentionCutoff(uint32_t days);
	/*
	 * Up to limit stored files, least recently accessed first, ordered by
	 * (accessed_at, id) and strictly after (after_at, after_id) unless
	 * after_at is empty. before non-empty: only accessed_at < before;
	 * from non-empty: only accessed_at >= from.
	 */
	std::vector<EvictCandidate> listEvictCandidates(const std::string &before,
							const std::string &from,
							const std::string &after_at,
							uint64_t after_id,
							size_t limit);
	/*
	 * Clear on_disk for one row, keeping updated_at, if it is still stored
	 * and its accessed_at is still the one listed. True when it changed the
	 * row; only then may the blob be unlinked.
	 */
	bool evictFile(uint64_t files_id, const std::string &accessed_at);

	/* A file row for fetching it from Telegram; nullopt if none. */
	std::optional<FetchRow> getFileForFetch(uint64_t files_id);
	/*
	 * Up to @limit things that reference the file and can be fetched
	 * afresh for a valid file reference, best first: the newest live
	 * messages carrying it (as attachment, then as link preview image,
	 * then as one of its other files), then the groups and users whose
	 * current photo it is, then the user and group photo lists holding
	 * it, then the custom emoji and the message effects whose file it
	 * is, then the accessible communities whose photo it is. Deleted
	 * messages are left out: the server no longer has them.
	 */
	std::vector<models::FileSource> listFileSources(uint64_t files_id,
							size_t limit);

	/*
	 * telegram_contact_lookups: the journal of the "Look up real name"
	 * cycle (ContactLookup.hpp, models/ContactLookup.cpp).
	 */
	struct ContactLookupRow {
		int64_t		user_id = 0;
		std::string	phone_number;
		std::string	first_name;
		std::string	last_name;
		std::optional<std::string>	note_json;
		bool		share_phone_number = false;
	};
	/* Throws on a duplicate: a pending row is never overwritten. */
	void insertContactLookup(const ContactLookupRow &c);
	void markContactLookupRemoved(int64_t user_id);
	void noteContactLookupFailure(int64_t user_id, const std::string &err);
	void deleteContactLookup(int64_t user_id);
	std::vector<ContactLookupRow> listContactLookups(void);
	/*
	 * Store a looked-up own name: first_name/last_name, own_name_known
	 * and own_name_checked_at = NOW(), with name history as upsertUser
	 * records it.
	 */
	void recordOwnNameLookup(int64_t user_id, const models::Name &own);
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
	 * Record a file without its bytes (storage-cache.md section 6):
	 * tg_file_id, tg_unique_id, type, size, extension and original name,
	 * with sha256 NULL and on_disk = 0. f.sha256_hex and f.on_disk are
	 * ignored. A file already recorded -- by tg_unique_id first, else by
	 * tg_file_id, stored or not -- keeps its row, which is returned with
	 * hit_count and accessed_at bumped; no second row is created.
	 */
	models::KnownFile upsertFileMeta(const models::File &f);

	/*
	 * Load the full tg_file_id -> telegram_files row index, so the daemon
	 * can link a message to a file it already recorded without
	 * re-downloading it. A row that is an alias (alias_of) maps to the
	 * row it points at.
	 */
	std::unordered_map<std::string, models::KnownFile> loadFileIndex(void);

	/*
	 * Load the tg_unique_id -> telegram_files row index, so a file seen
	 * again under another remote id (message media, a link preview image,
	 * a user's or group's photo) is linked instead of re-downloaded. A
	 * row that is an alias (alias_of) maps to the row it points at.
	 */
	std::unordered_map<std::string, models::KnownFile>
	loadUniqueIndex(void);

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
	 * A group's member count and whether the account may list its
	 * members, as Telegram last reported them (telegram_groups,
	 * migration 000044). A member left out was not reported and keeps
	 * what is stored; member_count_at is set when the count is written.
	 * A no-op for a group not stored.
	 */
	void setGroupMemberInfo(int64_t group_id,
				std::optional<int64_t> member_count,
				std::optional<bool> can_get_members);

	/* What telegram_groups holds about a group's members; nullopt when
	 * the group is not stored. */
	struct GroupMemberInfo {
		std::string		type;	/* basic_group, supergroup, channel */
		std::optional<int64_t>	member_count;
		int64_t			member_count_at = 0;	/* unix; 0 if never */
		std::optional<bool>	can_get_members;
	};
	std::optional<GroupMemberInfo> getGroupMemberInfo(int64_t group_id);

	/**
	 * Add a photo to a user's (@is_group false) or group's photo history
	 * unless it has a row for that file already. @date (Unix time) is
	 * the photo's own date when Telegram gives one, else 0 for "now".
	 * A row of the file dated later than @date gets @date: it was dated
	 * by when the photo was seen. Returns whether a row was added.
	 */
	/*
	 * Link the video of an animated photo to its history row: the row(s)
	 * of @owner_id whose file is the still @still_file_id get
	 * video_file_id = @video_file_id. Returns whether a row changed.
	 */
	bool setPhotoHistoryVideo(int64_t owner_id, bool is_group,
				  uint64_t still_file_id, uint64_t video_file_id);

	bool ensurePhotoHistory(int64_t owner_id, bool is_group,
				uint64_t file_id, int64_t date);

	/* Whether the user or the group has a row. */
	bool userExists(int64_t user_id);
	bool groupExists(int64_t group_id);

	/*
	 * The telegram_custom_emoji rows of @ids (ids without one are left
	 * out), in no particular order.
	 */
	std::vector<models::CustomEmoji>
	getCustomEmoji(const std::vector<int64_t> &ids);

	/*
	 * Insert or replace a custom emoji's row. A file_id of 0 keeps the
	 * file the row has.
	 */
	void putCustomEmoji(const models::CustomEmoji &e);

	/* The telegram_message_effects rows of @ids, as getCustomEmoji. */
	std::vector<models::MessageEffect>
	getMessageEffects(const std::vector<int64_t> &ids);

	/*
	 * Insert or replace a message effect's row. Each file id of 0 keeps
	 * the file the row has.
	 */
	void putMessageEffect(const models::MessageEffect &e);

	/*
	 * Insert or update a community from updateCommunity. @photo_file_id
	 * is its photo's telegram_files row; 0 stores none.
	 */
	void upsertCommunity(const models::Community &c, uint64_t photo_file_id);

	/*
	 * Store a community's full info: its big photo (@photo_file_id, 0
	 * none) and administrator count, and replace its chat list with
	 * the one given (chats it no longer lists are deleted). Makes the
	 * row, nameless, if it is missing.
	 */
	void putCommunityFullInfo(const models::CommunityFullInfo &fi,
				  uint64_t photo_file_id);

	/* Whether a community has no row, or a row without any chats. */
	bool communityNeedsFullInfo(int64_t community_id);

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
	 * Record the content a message now has, as delivered inline by
	 * updateMessageContent, appending the version it replaces.
	 *
	 * Distinct from the upserts because that update carries content and no
	 * edit_date, which is precisely what makes it worth using: the
	 * edit_date-bearing update carries no content and has to fetch it, and
	 * the fetch answers with whatever the message says later, losing any
	 * version that was already superseded by then. Leaves edit_date alone.
	 */
	bool recordMessageContentVersion(int64_t chat_id, int64_t message_id,
					 const models::MessageContent &content,
					 bool is_group);

	/*
	 * Record a message's server-resolved link preview. The owning message
	 * row id is resolved inside the statement via its (chat_id, message_id)
	 * unique key, so this costs one round trip and cannot race a separate
	 * lookup. Re-running it updates in place: Telegram may resolve a
	 * preview after the message first arrives.
	 */
	void upsertLinkPreview(const models::LinkPreview &lp, bool is_group);

	/*
	 * Store a poll as it stands now: the poll and its current options,
	 * updated in place, and the options it no longer lists marked
	 * deleted. See models/Poll.cpp.
	 */
	void upsertPoll(const models::Poll &p);

	/*
	 * Store a forum topic. With @only_if_absent (a topic read from its
	 * "created" service message) it only fills in a topic not stored
	 * yet; otherwise it replaces the stored state. See
	 * models/ForumTopic.cpp.
	 */
	void upsertForumTopic(const models::ForumTopic &t, bool only_if_absent);

	/* Record whether a forum topic is pinned in the forum's topic list. */
	void setForumTopicPinned(int64_t chat_id, int32_t topic_id, bool pinned);

	/*
	 * Store the latest interaction totals of a message (coalesced
	 * updateMessageInteractionInfo and updateMessageReactions); members
	 * left out keep what is stored. Returns whether the row exists.
	 */
	bool setMessageInteraction(int64_t chat_id, int64_t message_id,
				   bool is_group, const models::Interaction &ia);

	/* updateMessageIsPinned: a message pinned or unpinned in its chat. */
	void setMessagePinned(int64_t chat_id, int64_t message_id, bool is_group,
			      bool pinned);

	/*
	 * updateMessageFactCheck: set (@fact_check, a JSON object) or remove
	 * (nullopt) the "fact_check" key of a message's meta, leaving its
	 * other keys alone.
	 */
	void setMessageFactCheck(int64_t chat_id, int64_t message_id,
				 bool is_group,
				 const std::optional<std::string> &fact_check);

	/* Attach a downloaded image to a message's link preview. */
	void setLinkPreviewPhoto(int64_t chat_id, int64_t message_id,
				 bool is_group, uint64_t file_id);

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
	 * Record one of a message's files beyond its main one
	 * (telegram_message_files): @role is the table's role value ("cover",
	 * "paid", ...), @position its order within the role. A no-op when the
	 * message row does not exist; an existing (role, position) is pointed
	 * at the new file.
	 */
	void setMessageExtraFile(int64_t chat_id, int64_t message_id,
				 bool is_group, const std::string &role,
				 int position, uint64_t file_id);

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
	/* Own-name backfill for a contact stored before the contact split. */
	std::optional<models::Name> ownNameFromHistory(mysql::Transaction &tx,
							int64_t user_id,
							const models::Name &contact);
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
	 * SQL that is 1 when a stored content_extra and the one bound to its
	 * placeholder agree on their curated keys, ignoring TDLib's own JSON
	 * ("tdlib"), which changes with nobody editing. A stored row with no
	 * curated keys yet (none at all, or TDLib's JSON only) agrees with
	 * everything: gaining detail is not an edit.
	 */
	static constexpr const char *kCuratedExtraSame =
		"(COALESCE(JSON_REMOVE(content_extra, '$.tdlib',"
		" '$.tdlib_omitted_bytes'), JSON_OBJECT()) = JSON_OBJECT() OR"
		" COALESCE(JSON_REMOVE(content_extra, '$.tdlib',"
		" '$.tdlib_omitted_bytes'), JSON_OBJECT()) ="
		" COALESCE(JSON_REMOVE(CAST(? AS JSON), '$.tdlib',"
		" '$.tdlib_omitted_bytes'), JSON_OBJECT()))";

	/* Whether two tdlib_type values describe the same class; a value
	 * that was never recorded matches any. */
	static bool sameTdlibType(const std::optional<std::string> &a,
				  const std::optional<std::string> &b)
	{
		return !a.has_value() || !b.has_value() || *a == *b;
	}

	/*
	 * Shared message-upsert helpers, parameterized by table and
	 * foreign-key column so the identical private/group logic is not
	 * duplicated. The table and column arguments are compile-time
	 * literals, never user input.
	 */
	bool ensurePhotoHistory(mysql::Transaction &tx, bool is_group,
				int64_t owner_id, uint64_t file_id,
				int64_t date);
	bool snapshotMessageEditIfChanged(mysql::Transaction &tx,
					  const char *edits_table,
					  const char *fk_column,
					  uint64_t message_row_id,
					  const models::MessageContent &old_content,
					  int64_t old_edit_date,
					  const models::MessageContent &new_content,
					  int64_t new_edit_date,
					  bool entities_same,
					  bool extra_same);
	/*
	 * Whether a version of a location message was recorded less than a
	 * minute ago, so the next one is skipped: a live location moves
	 * every few seconds, and a version per movement would flood the
	 * edits table. The live row still takes every position.
	 */
	bool locationVersionTooSoon(mysql::Transaction &tx,
				    const char *edits_table,
				    const char *fk_column,
				    uint64_t message_row_id,
				    const models::MessageContent &old_content,
				    const models::MessageContent &new_content);
	void appendMessageVersion(mysql::Transaction &tx,
				  const char *edits_table,
				  const char *fk_column,
				  uint64_t message_row_id,
				  const models::MessageContent &content,
				  int64_t edit_date);
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
