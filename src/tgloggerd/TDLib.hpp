// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#ifndef TGLOGGERD__TDLIB_HPP
#define TGLOGGERD__TDLIB_HPP

#include <chrono>
#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include <future>
#include <optional>

#include <tgloggerd/models/User.hpp>
#include <tgloggerd/models/File.hpp>
#include <tgloggerd/models/Poll.hpp>
#include <tgloggerd/models/ForumTopic.hpp>
#include <tgloggerd/models/Group.hpp>
#include <tgloggerd/models/PrivateMessage.hpp>
#include <tgloggerd/models/GroupMessage.hpp>
#include <tgloggerd/models/LinkPreview.hpp>
#include <tgloggerd/models/GroupAdmin.hpp>
#include <tgloggerd/models/Backfill.hpp>
#include <tgloggerd/models/Community.hpp>

#include "ChatRecheck.hpp"
#include "helpers/log.h"

#include <utility>
#include <unordered_map>
#include <vector>

/*
 * Just enough of TDLib's types to declare TDLib::request() without pulling
 * in td_api.h; callers that build requests or read answers include it.
 */
namespace td {
namespace tl {
template <class T>
class unique_ptr;
} /* namespace tl */
namespace td_api {
class Object;
class Function;
} /* namespace td_api */
} /* namespace td */

namespace tgloggerd {

/*
 * A text message delivered to the message handler.
 *
 * It intentionally uses only plain types so that TDLib headers do not
 * leak into other tgloggerd sources.
 */
struct TextMessage {
	int64_t		sender_id;
	int64_t		message_id;
	std::string	sender_name;
	std::string	sender_username;
	std::string	text;
};

/*
 * A profile photo whose download has completed, ready to be stored.
 */
struct ProfilePhoto {
	int64_t		user_id;
	std::string	local_path;
	std::string	tg_file_id;	/* remote id (string), for DB dedup */
	std::string	tg_unique_id;	/* remote unique id; empty if unknown */
	int32_t		tg_local_file_id; /* local id, for TDLib deleteFile */
	int64_t		file_size;
};

/*
 * A group photo whose download has completed, ready to be stored.
 */
struct GroupPhoto {
	int64_t		group_id;
	std::string	local_path;
	std::string	tg_file_id;	/* remote id (string), for DB dedup */
	std::string	tg_unique_id;	/* remote unique id; empty if unknown */
	int32_t		tg_local_file_id; /* local id, for TDLib deleteFile */
	int64_t		file_size;
};

/*
 * A message's media attachment whose download has completed, ready to be
 * stored and linked to its message row. is_group selects the target
 * table (telegram_group_messages vs telegram_private_messages).
 */
struct MessageFile {
	int64_t		chat_id;
	int64_t		message_id;
	bool		is_group;
	/*
	 * The file is a LINK PREVIEW image, not an attachment. It belongs to the
	 * message's preview card, so it must be linked to the preview row rather
	 * than set as the message's own file -- a text message with a preview is
	 * still a text message, and treating the card's picture as an attachment
	 * would make it render as one everywhere.
	 */
	bool		is_link_preview = false;
	std::string	local_path;
	std::string	tg_file_id;	/* remote id (string), for DB dedup */
	std::string	tg_unique_id;	/* remote unique id; empty if unknown */
	int32_t		tg_local_file_id; /* local id, for TDLib deleteFile */
	int64_t		file_size;
	std::string	content_type;	/* telegram_files.file_type: "photo", ... */
	std::string	orig_file_name;	/* original Telegram name; empty if none */
	/* A file beyond the message's main one: its role and position in
	 * telegram_message_files ("cover", "paid", ...); empty for the main
	 * file, which is the message row's file_id. */
	std::string	role{};
	int		position = 0;
};

/*
 * A message whose media attachment is already recorded in the files table
 * (looked up by its remote id): link the message to the existing files row
 * without re-downloading the file.
 */
struct MessageFileLink {
	int64_t		chat_id;
	int64_t		message_id;
	bool		is_group;
	bool		is_link_preview = false; /* see MessageFile */
	uint64_t	file_id;	/* existing telegram_files.id */
	std::string	role{};		/* see MessageFile */
	int		position = 0;
};

/*
 * A user's profile photo or a group's photo that is already recorded in the
 * files table (see setFileLookup): point the user or group at the existing
 * row without re-downloading the photo.
 */
struct PhotoLink {
	int64_t		owner_id;	/* user id, or group (chat) id */
	bool		is_group;
	uint64_t	file_id;	/* existing telegram_files.id */
};

/*
 * A chat file recorded without downloading it (storage-cache.md section 6):
 * it comes from a chat the account is not a member of, or its known size is
 * at or over the per-file cap. The receiver records the row by unique id
 * (DB::upsertFileMeta) and links it like a MessageFileLink, or, with
 * is_group_photo, as chat_id's group photo (message_id is then unused).
 */
struct FileMeta {
	int64_t		chat_id;
	int64_t		message_id;
	bool		is_group;
	bool		is_link_preview = false; /* see MessageFile */
	bool		is_group_photo = false;
	std::string	tg_file_id;	/* remote id */
	std::string	tg_unique_id;	/* remote unique id; empty if unknown */
	int64_t		file_size;	/* 0 if unknown */
	std::string	content_type;	/* telegram_files.file_type */
	std::string	orig_file_name;	/* original Telegram name; empty if none */
	std::string	role{};		/* see MessageFile */
	int		position = 0;
};

/*
 * A reply relationship: message (chat_id, message_id) replies to
 * message reply_to_msg_id in the same chat. is_group selects the table.
 * Emitted only after the replied message has been saved.
 */
struct MessageReply {
	int64_t		chat_id;
	int64_t		message_id;
	/* The replied message's (chat_id, message_id); reply_to_chat_id may
	 * differ from chat_id for a cross-chat reply. */
	int64_t		reply_to_chat_id;
	int64_t		reply_to_msg_id;
	bool		is_group;
};

/*
 * A live, newly-received message flattened for the Discord forwarder. Emitted
 * once per updateNewMessage only (never for edits, deletes or backfill), with
 * the sender name already resolved and the text/caption extracted. `kind` is
 * the content category ("text", "photo", ...) used to render a placeholder when
 * there is no text.
 */
/*
 * A formatting span over ForwardMessage::text. offset/length are UTF-16
 * code-unit indices (as TDLib reports them). Only the types Discord can render
 * are kept; everything else is Other and left as plain text.
 */
struct FmtEntity {
	enum class Type {
		Bold, Italic, Underline, Strikethrough, Spoiler,
		Code, Pre, BlockQuote, Other,
	};
	int32_t     offset;
	int32_t     length;
	Type        type;
	std::string language; /* Pre only: the code-block language, may be empty */
};

/* One message's latest interaction totals, from the coalesced updates. */
struct InteractionUpdate {
	int64_t			chat_id = 0;
	int64_t			message_id = 0;	/* server id */
	bool			is_group = false;
	models::Interaction	interaction;
};

/*
 * What one chat recheck got back (see TDLib::recheckChat), handed over
 * after its messages went through the message handlers: is_group
 * selects the table, page the getChatHistory sent and the server ids
 * it returned.
 */
struct RecheckAnswer {
	int64_t		chat_id = 0;
	bool		is_group = false;
	recheck::Page	page;
	/* Seconds from queueing to Telegram's answer. */
	int64_t		waited_secs = 0;
	/* The recheck and its page, to ask for the next page with
	 * TDLib::recheckNextPage; seq ties the page's found and answer
	 * callbacks together. */
	recheck::Item	item;
	int64_t		oldest_td = 0;	/* TDLib id of the oldest returned */
	uint64_t	seq = 0;
};

struct ForwardMessage {
	int64_t		chat_id = 0;
	int64_t		message_id = 0;
	int64_t		sender_id = 0;      /* user sender; 0 if a chat sent it */
	int64_t		sender_chat_id = 0; /* chat/channel sender; 0 if a user */
	std::string	sender_name;
	std::string	sender_username;
	std::string	text;	/* message text or media caption; may be empty */
	std::vector<FmtEntity> entities; /* formatting over `text` (UTF-16 spans) */
	/* A short plain-text rendering of content with nothing to read but
	 * its structure (a location, a poll, ...); sent after the text.
	 * Kept apart so each follows its own filter. May be empty. */
	std::string	summary;
	std::string	kind;	/* content category; empty for plain unknown */
	int64_t		reply_to_chat_id = 0; /* replied chat_id; 0 = same chat */
	int64_t		reply_to_msg_id = 0;  /* replied server msg id; 0 = none */
	bool		has_file = false;   /* carries a downloadable media file */
	bool		is_outgoing = false;
};

/*
 * tgloggerd::TDLib is a wrapper class for TDLib.
 *
 * Since TDLib contains very heavy header files, keep tgloggerd
 * compilation time low by not including TDLib header files in
 * other tgloggerd telegram_files. Expose only used functions in
 * tgloggerd::TDLib class.
 */
class TDLib {
public:
	TDLib(uint32_t api_id, const char *api_hash, const char *data_dir);
	~TDLib(void);

	TDLib(const TDLib &) = delete;
	TDLib &operator=(const TDLib &) = delete;

	/*
	 * Set the log handle for the daemon's own messages (not TDLib's). The
	 * handle must outlive this object; nullptr, the default, logs nothing.
	 * Must be called before loop().
	 */
	void setLogger(log_hd_t *l);

	/*
	 * Set the callback invoked for every incoming text message.
	 */
	void setMessageHandler(std::function<void(const TextMessage &)> cb);

	/*
	 * Set the callback invoked for every private-chat message
	 * (new, edited, or deleted). Replaces the simpler
	 * setMessageHandler for full private message tracking.
	 */
	void setPrivateMessageHandler(
		std::function<void(const models::PrivateMessage &)> cb);

	/*
	 * Set the callback invoked for every group-chat message (basic
	 * group, supergroup or channel), whether new, edited, or deleted.
	 */
	void setGroupMessageHandler(
		std::function<void(const models::GroupMessage &)> cb);

	/*
	 * Set the callback invoked when Telegram reports a message's content,
	 * delivered inline rather than as a date to go and look up. Called with
	 * (chat_id, message_id, content, is_group).
	 *
	 * This is what preserves a version that was replaced before anyone
	 * could ask about it -- see recordMessageContentVersion(). It is not
	 * called for polls or live locations, whose content changes on its own.
	 */
	void setContentVersionHandler(
		std::function<void(int64_t, int64_t,
				   const models::MessageContent &, bool)> cb);

	/*
	 * Set the callback invoked when a message carries a server-resolved link
	 * preview. Fires after the message handler, so the row it attaches to
	 * already exists.
	 */
	void setLinkPreviewHandler(
		std::function<void(const models::LinkPreview &, bool is_group)> cb);

	/*
	 * Set the callback invoked with a poll as it stands now: for the poll
	 * of each ingested message (after the message handler) and for every
	 * updatePoll, which TDLib sends as votes arrive or the poll closes.
	 */
	void setPollHandler(std::function<void(const models::Poll &)> cb);

	/*
	 * Set the callback for a group's member count and whether the account
	 * may list its members, whenever Telegram reports them (supergroup
	 * updates, supergroup and basic group full info). The group is its
	 * chat id; a member not reported is nullopt. Repeats of the same
	 * values are not passed on.
	 */
	void setGroupMemberInfoHandler(
		std::function<void(int64_t chat_id, std::optional<int64_t> count,
				   std::optional<bool> can_get_members)> cb);

	/*
	 * Set the callback for interaction totals (views, forwards, replies,
	 * reactions) that changed since a message was stored. Updates are
	 * coalesced per message and handed over in one batch every
	 * @flush_secs seconds, so a message is written at most once per
	 * interval; 0 records none of them (a new copy still carries its
	 * totals). A rate report is logged every ten minutes.
	 */
	void setInteractionHandler(
		std::function<void(std::vector<InteractionUpdate>)> cb,
		int flush_secs);

	/*
	 * Set the callbacks for changes to a message outside its content:
	 * @pinned (updateMessageIsPinned) and @fact_check
	 * (updateMessageFactCheck, with the "fact_check" JSON or nullopt when
	 * removed). Message ids are server ids.
	 */
	void setMessageMetaHandlers(
		std::function<void(int64_t chat_id, int64_t message_id,
				   bool is_group, bool pinned)> pinned,
		std::function<void(int64_t chat_id, int64_t message_id,
				   bool is_group,
				   const std::optional<std::string> &)> fact_check);

	/*
	 * Set the callbacks for forum topics: @info with a topic and whether
	 * it only fills in a missing one (read from its "created" message)
	 * rather than describing it now; @pinned when a topic is pinned or
	 * unpinned in the forum's list.
	 */
	void setForumTopicHandlers(
		std::function<void(const models::ForumTopic &, bool only_if_absent)> info,
		std::function<void(int64_t chat_id, int32_t topic_id, bool pinned)> pinned);

	/*
	 * Set the callback invoked once for every live, newly-received message
	 * (updateNewMessage only), for forwarding to external sinks (Discord).
	 * Runs on the TDLib event thread, so it must return quickly.
	 */
	void setForwardHandler(std::function<void(const ForwardMessage &)> cb);

	/*
	 * Set the callback invoked when a live message is edited
	 * (updateMessageEdited), carrying the updated content, so the forwarder
	 * can edit the corresponding Discord message. Runs on the event thread.
	 */
	void setEditForwardHandler(std::function<void(const ForwardMessage &)> cb);

	/*
	 * Set the callback invoked when a live message is permanently deleted
	 * (chat_id, server message id), so the forwarder can tombstone the
	 * corresponding Discord message. Runs on the event thread.
	 */
	void setDeleteForwardHandler(std::function<void(int64_t, int64_t)> cb);

	/*
	 * Set the callback invoked when a message's media attachment has
	 * finished downloading, so it can be stored and linked.
	 */
	void setMessageFileHandler(std::function<void(const MessageFile &)> cb);

	/*
	 * Set the callbacks invoked, with the TDLib file id, before the first
	 * and after the last store emit (profile photo, group photo, message
	 * file) for one completed download, so the file can be held for the
	 * whole sequence. Run on the event thread; end runs even if an emit
	 * throws.
	 */
	void setFileEmitBatchHandlers(std::function<void(int32_t)> begin,
				      std::function<void(int32_t)> end);

	/*
	 * Predicate used before downloading a file (message media, a link
	 * preview image, a user's or group's photo): given the file's remote
	 * id and unique id (either may be empty), return the existing
	 * telegram_files row if it is already recorded, so the file can be
	 * linked without a re-download. The unique id matters: the same file
	 * often comes back under a different remote id, in another message or
	 * in a later updateUser or getChat. Runs on the loop thread.
	 */
	void setFileLookup(
		std::function<std::optional<models::KnownFile>(
			const std::string &, const std::string &)> cb);

	/*
	 * Set the callback invoked to link a message to a file that is already
	 * recorded (see setFileLookup), instead of downloading it again.
	 */
	void setMessageFileLinkHandler(
		std::function<void(const MessageFileLink &)> cb);

	/*
	 * Set the callback invoked to point a user or group at a photo that
	 * is already recorded (see setFileLookup), instead of downloading it
	 * again. Without it, or without setFileLookup, every photo that
	 * TDLib does not hold locally is downloaded.
	 */
	void setPhotoLinkHandler(std::function<void(const PhotoLink &)> cb);

	/*
	 * Set the callback invoked for a chat file recorded without its bytes
	 * (see FileMeta and setMediaPolicy). Without it every file is
	 * downloaded. Runs on the loop thread; keep DB writes off it.
	 */
	void setFileMetaHandler(std::function<void(const FileMeta &)> cb);

	/*
	 * When to record a chat file (message attachment, link preview image,
	 * group photo) without downloading it: skip_nonmember for every file
	 * of a chat the account is not a member of; max_store_file_size, if
	 * not 0, for every file whose known size is at least that. User
	 * profile photos are always downloaded. Must be called before loop().
	 */
	void setMediaPolicy(bool skip_nonmember, uint64_t max_store_file_size);

	/*
	 * Set the callback invoked to link a message to the one it replies
	 * to, after the replied message has been fetched and saved.
	 */
	void setMessageReplyHandler(std::function<void(const MessageReply &)> cb);

	/*
	 * Set the callback invoked whenever a user's information is received
	 * or updated (td_api::updateUser).
	 */
	void setUserHandler(std::function<void(const models::User &)> cb);

	/*
	 * Set the callback invoked when a user's td_api::userFullInfo is
	 * received (fetched on first sight, or via updateUserFullInfo).
	 */
	void setUserFullInfoHandler(
		std::function<void(const models::UserFullInfo &)> cb);

	/*
	 * Set the callback for the video of an animated current photo, seen
	 * in a user's or a group's full info: @still is the photo's largest
	 * size and @video its MPEG-4 (the big animation, else the small one),
	 * both described for a metadata-only telegram_files row. The callee
	 * links the video to the history row of that still. Loop thread;
	 * keep DB work off it.
	 */
	void setPhotoVideoHandler(
		std::function<void(int64_t owner_id, bool is_group,
				   const models::File &still,
				   const models::File &video)> cb);

	/*
	 * Set the callback invoked when a user's (big) profile photo has
	 * finished downloading.
	 */
	void setProfilePhotoHandler(std::function<void(const ProfilePhoto &)> cb);

	/*
	 * Set the callback invoked whenever a group's information is received
	 * or updated (assembled from the chat, supergroup/basicGroup and
	 * full-info objects).
	 */
	void setGroupHandler(std::function<void(const models::Group &)> cb);

	/*
	 * Set the callback invoked when a group's (big) photo has finished
	 * downloading.
	 */
	void setGroupPhotoHandler(std::function<void(const GroupPhoto &)> cb);

	/*
	 * Set the callback invoked with a group's full administrator set
	 * (fetched on first sight and refreshed by periodic polling).
	 */
	void setGroupAdminsHandler(
		std::function<void(const models::GroupAdminList &)> cb);

	/*
	 * Configure periodic admin polling: refresh interval in seconds
	 * (<= 0 disables polling) and how many groups to refresh per tick.
	 * Must be called before the client authorizes.
	 */
	void setAdminPollConfig(double interval_seconds, int batch);

	/*
	 * Background message backfiller. It walks every accessible chat's history
	 * newest -> oldest, round-robin and gently paced, to collect messages the
	 * real-time path never saw, and it resumes across restarts.
	 */

	/*
	 * Set the callback invoked whenever a chat's backfill progress changes
	 * (registered, cursor advanced, or completed), so it can be persisted.
	 */
	void setBackfillStateHandler(
		std::function<void(const models::BackfillState &)> cb);

	/*
	 * Seed the backfiller with the progress rows loaded from the database so
	 * each chat's history walk resumes where it left off. Call before the
	 * client authorizes.
	 */
	void loadBackfillState(const std::vector<models::BackfillState> &states);

	/*
	 * Configure the backfiller. tick_interval seconds between history pages
	 * (<= 0 disables backfilling), discovery_interval seconds between chat-list
	 * sweeps, page messages per getChatHistory call, and inflight the max
	 * concurrent history requests. Call before the client authorizes.
	 */
	void setBackfillConfig(double tick_interval, double discovery_interval,
			       int page, int inflight);

	/*
	 * Process a single batch of TDLib events, waiting up to @timeout
	 * seconds for one to arrive. Drives authentication and message
	 * delivery. Call it repeatedly until isStopped() returns true.
	 */
	void loop(int timeout);

	/*
	 * Whether the TDLib client has been closed and the loop should
	 * terminate.
	 */
	bool isStopped(void) const;

	/*
	 * The client's state as GET /v1/status reports it: Ready while it is
	 * authorized and not closing (exactly when request() sends), Waiting
	 * before that (connecting, or not logged in), Closing once it is
	 * logging out, closing or closed; Closing never changes back.
	 */
	enum class State { Waiting, Ready, Closing };

	struct Status {
		State	state;
		/* Unix seconds of the last change of state (or of startup). */
		int64_t	since;
		/*
		 * The logged-in account's own Telegram user id, from getMe once
		 * the client is authorized; 0 until then.
		 */
		int64_t	account_user_id = 0;
	};

	/* Any thread; cheap (no TDLib call). */
	Status status(void) const;

	static const char *stateName(State s)
	{
		switch (s) {
		case State::Ready:
			return "ready";
		case State::Closing:
			return "closing";
		case State::Waiting:
			break;
		}
		return "waiting";
	}

	/*
	 * Request a graceful shutdown of the TDLib client.
	 */
	void close(void);

	/*
	 * Delete TDLib's own cached copy of a downloaded file (by its local
	 * file id) once tgloggerd has secured its own copy, so the file is not
	 * stored twice. Fire-and-forget and thread-safe: callable from a file
	 * worker thread, not just the loop thread. Keeps the remote reference,
	 * so the file can be re-downloaded on demand.
	 */
	void deleteLocalFile(int32_t file_id);

	/* td_api::object_ptr<td_api::Object> / <td_api::Function>. */
	using TdObject = td::tl::unique_ptr<td::td_api::Object>;
	using TdFunction = td::tl::unique_ptr<td::td_api::Function>;

	/*
	 * td_api::error codes request() answers with when TDLib itself was
	 * never asked or never answered. Telegram's own errors come back
	 * unchanged, with their own codes.
	 */
	static constexpr int32_t kRequestNotReady = 503; /* not authorized,
							     closing or closed */
	static constexpr int32_t kRequestTimeout = 504;
	static constexpr int32_t kRequestWrongThread = 500;

	/*
	 * Send a TDLib function from any thread but the loop thread and get
	 * its answer: the answer object, or a td_api::error (errors are
	 * values; the future never holds an exception).
	 *
	 * While the client is not authorized, or once it is closing, the
	 * request is not sent and the future is ready at once with
	 * kRequestNotReady. Requests still pending when the client closes,
	 * when close() is called, or when this object is destroyed are
	 * answered the same way.
	 *
	 * Never call it from the loop thread: the answer is delivered by
	 * loop(), so the caller would wait for itself (asserted; in a
	 * release build the future is ready at once with
	 * kRequestWrongThread). Do not call it from a serial_ or files_
	 * pool job either: the loop thread can block posting into those
	 * pools when their queues are full, and then never delivers the
	 * answer the job waits for.
	 *
	 * Callers must be done with request() before this object is
	 * destroyed.
	 */
	std::future<TdObject> request(TdFunction f);

	/*
	 * request() and wait at most @timeout for the answer. On timeout the
	 * request is forgotten, so a late answer is dropped, and the result
	 * is a td_api::error with kRequestTimeout. Same thread rules as
	 * request().
	 */
	TdObject request(TdFunction f, std::chrono::milliseconds timeout);

	/*
	 * Fetch @src afresh and take the file out of it the way ingest does
	 * (message_content_file, link_preview_image, the big chat or profile
	 * photo). The answer is the td_api::file, or a td_api::error: TDLib's
	 * own (the message is gone, the chat is not accessible), or code 404
	 * when the source no longer carries a file. Within @timeout in all;
	 * same thread rules as request().
	 */
	TdObject resolveFile(const models::FileSource &src,
			     std::chrono::milliseconds timeout);

	/*
	 * Whether ingest is waiting for TDLib file @file_id to finish
	 * downloading (a message attachment, link preview image, group or
	 * profile photo). Such a download asked for the whole file, so a
	 * ranged downloadFile must not be issued over it. Thread-safe. A
	 * download that failed is dropped once TDLib reports it; one that a
	 * ranged downloadFile replaced stays listed until ingest resumes it
	 * when that range ends, so combine this with the file's
	 * local.is_downloading_active.
	 */
	bool isIngestDownloading(int32_t file_id) const;

	/*
	 * Queue a full-info refetch of a user / group (by chat_id). Thread-safe
	 * (callable from a worker thread); the refetch runs on the loop thread
	 * and its upserts record any changed fields as history. Used to refresh
	 * an entity every 10th message it is involved in.
	 */
	void refetchUser(int64_t user_id);
	void refetchGroup(int64_t group_id);

	/*
	 * Set the callbacks for communities (message-types.md section
	 * 4.13): @community for every updateCommunity, @full_info for every
	 * community full info (updateCommunityFullInfo, or one asked for
	 * with fetchCommunityFullInfo), and @named, once per run, for each
	 * accessible community a logged message or a chat's full info
	 * names; @community is called again for it first, so its row is
	 * stored before @named runs. All run on the loop thread.
	 */
	void setCommunityHandlers(
		std::function<void(const models::Community &)> community,
		std::function<void(const models::CommunityFullInfo &)> full_info,
		std::function<void(int64_t)> named);

	/*
	 * Queue a getCommunityFullInfo of @community_id. Thread-safe; drained
	 * on the loop thread at a fixed pace. A community whose full info
	 * arrived this run is not asked again, and one whose call failed
	 * not within the hour after.
	 */
	void fetchCommunityFullInfo(int64_t community_id);

	/*
	 * Queue (chat_id, server message id) pairs whose media should be
	 * downloaded and linked again. Each is re-fetched through the normal
	 * message path, so a file already known is linked without a download
	 * and one whose bytes are already in the store is linked without a
	 * second copy. Thread-safe; drained on the loop thread at a fixed pace
	 * so a large batch cannot flood the API.
	 */
	void relinkMessageMedia(std::vector<std::pair<int64_t, int64_t>> msgs);

	/*
	 * Queue (chat_id, server message id) pairs to be re-fetched and stored
	 * again, paced with relinkMessageMedia under one budget; see
	 * DB::listMessagesToRepair.
	 */
	void repairMessages(std::vector<std::pair<int64_t, int64_t>> msgs);

	/*
	 * Queue (chat_id, server message id) pairs to be re-fetched and stored
	 * again so that the still thumbnail TDLib offers is stored; see
	 * DB::listMessagesWithoutThumbnail. Shares the repair's share of the
	 * budget and its wait for chat rechecks, and runs after it.
	 */
	void refetchThumbnails(std::vector<std::pair<int64_t, int64_t>> msgs);

	/*
	 * Queue a recheck of chat @chat_id around server message @around (0 =
	 * the newest), within the limits of recheck::Queue, and say what
	 * became of it. Thread-safe. The recheck runs on the loop thread,
	 * paced under the relink and repair budget (after relink, before
	 * repair): one getChatHistory, every message it returns stored
	 * through the message handlers as if it had just arrived (never
	 * forwarded), then the answer handed to the recheck handler, which
	 * concludes deletions. About two minutes after authorization the
	 * newest stretch of every chat whose last message is at most two
	 * days old is queued the same way, newest first.
	 */
	recheck::Admit recheckChat(int64_t chat_id, int64_t around);

	/*
	 * Set the callback for the restart catch-up's candidates, called once
	 * per run on the loop thread with the recently active chats (newest
	 * first) and TDLib's last message id (server id) of each. The callee
	 * keeps those with a gap and queues them with queueRecheckCatchUp;
	 * keep DB work off the loop thread. Without it, every candidate is
	 * queued.
	 */
	void setRecheckCatchUpHandler(
		std::function<void(std::vector<int64_t> chats,
				   std::unordered_map<int64_t, int64_t> tdlib_last)>
			cb);

	/* Queue a newest-stretch background recheck of each of @chats; any
	 * thread. Returns how many were queued. */
	size_t queueRecheckCatchUp(const std::vector<int64_t> &chats);

	/*
	 * Set the callback for each recheck's answer (see recheckChat). Runs
	 * on the loop thread; keep DB work off it.
	 */
	void setRecheckHandler(std::function<void(const RecheckAnswer &)> cb);

	/*
	 * Set the callback for the messages a recheck found, called with
	 * their server ids BEFORE they are stored, so a deletion an earlier
	 * recheck concluded for one of them can be undone ahead of the
	 * store. Runs on the loop thread; keep DB work off it.
	 */
	void setRecheckFoundHandler(
		std::function<void(const RecheckAnswer &a)> cb);

	/* Ask for the next page of a recheck (@a's item, older than its
	 * oldest message); any thread. */
	void recheckNextPage(const RecheckAnswer &a);

	/*
	 * When enabled, issue a one-time optimizeStorage once the client
	 * reaches the ready state, reclaiming files TDLib cached in a previous
	 * run. Must be called before loop().
	 */
	void setPruneOnStart(bool on);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} /* namespace tgloggerd */

#endif /* #ifndef TGLOGGERD__TDLIB_HPP */
