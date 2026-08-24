// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_DAO_BROWSE_HPP
#define TGLOGGERD_WEB_DAO_BROWSE_HPP

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>

namespace tgweb::dao::browse {

/*
 * Read-only browsing over the logger's tgloggerd schema (the "ro" client).
 *
 * Every function returns a nlohmann::json context that is ready to hand to a
 * template: all attacker-controlled strings (names, usernames, bios, phone
 * numbers, ...) are ALREADY escaped with Render::esc(). Numeric ids, enum
 * values and timestamps are emitted verbatim. Keeping the escaping here makes
 * it a data-layer invariant that no template author can forget.
 */

/* Dashboard totals: {users, groups, telegram_private_messages, telegram_group_messages, files}. */
drogon::Task<nlohmann::json> counts(drogon::orm::DbClientPtr db);

/*
 * A single user's profile and change history, or std::nullopt if no such user.
 * Result: {user:{...}, usernames:[...], name_hist:[...], username_events:[...],
 * bio_hist:[...], phone_hist:[...], photo_hist:[...]}.
 */
drogon::Task<std::optional<nlohmann::json>> getUser(drogon::orm::DbClientPtr db,
						    int64_t id);

/*
 * A user's change history as one merged, newest-first timeline (name, username
 * events, bio, phone and profile-photo changes UNION-ed), paginated by
 * limit/offset. Returns std::nullopt if no such user. Result: {entries:[{kind,
 * detail, action?, file_id?, created_at}], limit, offset, has_more}.
 */
drogon::Task<std::optional<nlohmann::json>>
userHistory(drogon::orm::DbClientPtr db, int64_t id, int limit, int offset);

/*
 * A single group with its current admins and change history, or std::nullopt
 * if no such group. Result: {group:{...}, usernames:[...], admins:[...],
 * title_hist:[...], desc_hist:[...], username_events:[...], photo_hist:[...],
 * admin_hist:[...]}.
 */
drogon::Task<std::optional<nlohmann::json>> getGroup(drogon::orm::DbClientPtr db,
						     int64_t id);

/*
 * A group's current admins for the dedicated admins page, or std::nullopt if
 * no such group. Result: {group:{id, type, title}, admins:[{user_id, name,
 * username, status, custom_title, is_anonymous, perms}]}.
 */
drogon::Task<std::optional<nlohmann::json>>
getGroupAdmins(drogon::orm::DbClientPtr db, int64_t id);

/*
 * One page of messages. scope is "private" or "group". When chatId has a
 * value the list is scoped to that chat and pages on message_id (covered by
 * the uq_*_chat_msg index); otherwise it is a global list paging on the PK id.
 * cursor is the last key seen (0 for the first page). Result:
 * {messages:[...], next_cursor, scope, chat_id}.
 */
drogon::Task<nlohmann::json> listMessages(drogon::orm::DbClientPtr db,
					  std::string scope,
					  std::optional<int64_t> chatId,
					  int64_t cursor, int limit);

/*
 * A single message (by PK id) with its edit history, forward info and the
 * message it replies to, or std::nullopt if no such message. scope selects
 * the private_* or group_* tables.
 */
drogon::Task<std::optional<nlohmann::json>> getMessage(drogon::orm::DbClientPtr db,
						       std::string scope,
						       int64_t id);

/* Metadata needed to locate and serve a stored file. */
struct FileMeta {
	std::string hex;       /* lower-case hex of the SHA-256 digest.      */
	std::string ext;       /* file extension without a dot, or empty.    */
	std::string fileType;  /* "photo", "video", "document", ...          */
	std::string origName;  /* original Telegram file name, or empty.     */
	uint64_t    size;      /* file size in bytes.                        */
	bool        onDisk;    /* false = metadata-only (too large to store).*/
};

/*
 * Look up a file by id (raw values, NOT escaped — this feeds the filesystem
 * and headers, not a template). Returns std::nullopt if no such file.
 */
drogon::Task<std::optional<FileMeta>> getFile(drogon::orm::DbClientPtr db,
					      int64_t id);

/*
 * Header for a chat-history page. scope is "group" (chatId is a telegram_groups.id) or
 * "private" (chatId is the peer telegram_users.id). Returns {kind, id, title, type,
 * photo_file_id?} with strings escaped, or std::nullopt when no such chat.
 */
drogon::Task<std::optional<nlohmann::json>>
chatHeader(drogon::orm::DbClientPtr db, std::string scope, int64_t chatId);

/*
 * One page of a chat's history for the scrollable chat view. `after` is a
 * message-id cursor: with it, the page is the oldest `limit` messages newer
 * than `after`; without it, the newest `limit` messages (the landing page).
 * Messages come oldest-first (newest at the bottom). Each same-chat reply
 * gets a `href` that jumps to its target -- an in-page #anchor when the target
 * is on this page, else a "?limit&after#msg" URL that loads the page holding
 * it (centered). The result also carries the pagination cursors
 * (older_after/newer_after, absent when there is nothing that way) and the
 * effective `limit`, so the same data drives the server-rendered pager today
 * and a scroll/fetch endpoint later.
 */
drogon::Task<nlohmann::json> chatHistory(drogon::orm::DbClientPtr db,
					 std::string scope, int64_t chatId,
					 int limit, std::optional<int64_t> after,
					 std::optional<int64_t> afterTs);

} /* namespace tgweb::dao::browse */

#endif /* TGLOGGERD_WEB_DAO_BROWSE_HPP */
