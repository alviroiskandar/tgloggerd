// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#ifndef TGLOGGERD__MODELS__GROUP_MESSAGE_HPP
#define TGLOGGERD__MODELS__GROUP_MESSAGE_HPP

#include <string>
#include <cstdint>
#include <optional>

#include <tgloggerd/models/Message.hpp>

namespace tgloggerd {
namespace models {

/*
 * A group-chat message (basic group, supergroup or channel), as stored
 * in the group_messages, group_message_edits, and group_message_fwd_info
 * tables. Its chat_id is the group's chat_id (a valid groups.id).
 *
 * Unlike a private message, the sender may be a user or a chat/channel,
 * so exactly one of sender_user_id / sender_chat_id is set.
 */
struct GroupMessage {
	int64_t		chat_id = 0;
	int64_t		message_id = 0;

	/* Sender is a user (messageSenderUser). */
	std::optional<int64_t>	sender_user_id;

	/* Sender is a chat/channel (messageSenderChat): channel posts and
	 * anonymous group admins. */
	std::optional<int64_t>	sender_chat_id;

	bool		is_outgoing = false;
	bool		is_channel_post = false;

	/* Author signature for channel posts / anonymous admins. */
	std::optional<std::string>	author_signature;

	int64_t		date = 0;
	int64_t		edit_date = 0;

	/* Album (media group) id shared by messages sent together; nullopt
	 * when the message is not part of an album. */
	std::optional<int64_t>	media_album_id;

	/* Content that an edit can change (type, text, file). */
	MessageContent	content;

	bool		is_deleted = false;

	/* Forward information; nullopt if not forwarded. */
	std::optional<ForwardInfo>	forward_info;
};

} /* namespace models */
} /* namespace tgloggerd */

#endif /* #ifndef TGLOGGERD__MODELS__GROUP_MESSAGE_HPP */
