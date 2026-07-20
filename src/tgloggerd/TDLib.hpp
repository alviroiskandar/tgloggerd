// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#ifndef TGLOGGERD__TDLIB_HPP
#define TGLOGGERD__TDLIB_HPP

#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include <optional>

#include <tgloggerd/models/User.hpp>
#include <tgloggerd/models/Group.hpp>
#include <tgloggerd/models/PrivateMessage.hpp>
#include <tgloggerd/models/GroupMessage.hpp>
#include <tgloggerd/models/GroupAdmin.hpp>
#include <tgloggerd/models/Backfill.hpp>

#include <vector>

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
	int32_t		tg_local_file_id; /* local id, for TDLib deleteFile */
	int64_t		file_size;
};

/*
 * A message's media attachment whose download has completed, ready to be
 * stored and linked to its message row. is_group selects the target
 * table (group_messages vs private_messages).
 */
struct MessageFile {
	int64_t		chat_id;
	int64_t		message_id;
	bool		is_group;
	std::string	local_path;
	std::string	tg_file_id;	/* remote id (string), for DB dedup */
	int32_t		tg_local_file_id; /* local id, for TDLib deleteFile */
	int64_t		file_size;
	std::string	content_type;	/* files.file_type: "photo", ... */
	std::string	orig_file_name;	/* original Telegram name; empty if none */
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
	uint64_t	file_id;	/* existing files.id */
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
 * tgloggerd::TDLib is a wrapper class for TDLib.
 *
 * Since TDLib contains very heavy header files, keep tgloggerd
 * compilation time low by not including TDLib header files in
 * other tgloggerd files. Expose only used functions in
 * tgloggerd::TDLib class.
 */
class TDLib {
public:
	TDLib(uint32_t api_id, const char *api_hash, const char *data_dir);
	~TDLib(void);

	TDLib(const TDLib &) = delete;
	TDLib &operator=(const TDLib &) = delete;

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
	 * Set the callback invoked when a message's media attachment has
	 * finished downloading, so it can be stored and linked.
	 */
	void setMessageFileHandler(std::function<void(const MessageFile &)> cb);

	/*
	 * Predicate used before downloading a message's media: given the file's
	 * remote id, return the existing files.id if it is already recorded, so
	 * the file can be linked without a re-download. Runs on the loop thread.
	 */
	void setFileLookup(
		std::function<std::optional<uint64_t>(const std::string &)> cb);

	/*
	 * Set the callback invoked to link a message to a file that is already
	 * recorded (see setFileLookup), instead of downloading it again.
	 */
	void setMessageFileLinkHandler(
		std::function<void(const MessageFileLink &)> cb);

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
