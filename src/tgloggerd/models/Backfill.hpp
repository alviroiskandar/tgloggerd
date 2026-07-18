// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#ifndef TGLOGGERD__MODELS__BACKFILL_HPP
#define TGLOGGERD__MODELS__BACKFILL_HPP

#include <cstdint>
#include <optional>
#include <string>

namespace tgloggerd {
namespace models {

/*
 * Progress of the background message backfiller for one chat, mirrored in the
 * chat_backfill_state table so a chat's newest->oldest history walk resumes
 * across restarts.
 */
struct BackfillState {
	int64_t			chat_id = 0;
	std::string		scope;	     /* "group" or "private". */
	/* Oldest TdLib LOCAL message id fetched so far; std::nullopt means the
	 * walk has not started (fetch from the newest message). */
	std::optional<int64_t>	cursor_msg_id;
	bool			done = false; /* history start reached. */
	/* High priority: the chat is in the user's Main/Archive chat list (a
	 * private chat with history, or a group/channel the user joined), so it
	 * is backfilled ahead of chats seen only incidentally. */
	bool			priority = false;
};

} /* namespace models */
} /* namespace tgloggerd */

#endif /* #ifndef TGLOGGERD__MODELS__BACKFILL_HPP */
