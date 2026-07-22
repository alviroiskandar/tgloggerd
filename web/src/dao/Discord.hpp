// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_DAO_DISCORD_HPP
#define TGLOGGERD_WEB_DAO_DISCORD_HPP

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

/*
 * Discord webhook integrations (`discord_webhooks`, in the logger schema). The
 * web app is granted DML on this one table via the read-only tgloggerd user, so
 * every call here uses the "ro" DbClient. Strings placed on returned JSON are
 * Render::esc()-escaped except where noted (searchChats -> select2, raw).
 */
namespace tgweb::dao::discord {

struct Webhook {
	uint64_t    id;
	int64_t     chatId;
	std::string chatType;   /* "private" | "group" */
	std::string chatTitle;
	std::string webhookUrl;
	bool        enabled;
};

/* A Telegram chat resolved from the logged schema (groups/users). */
struct ChatRef {
	int64_t     chatId;
	std::string type;       /* "private" | "group" */
	std::string title;
};

/* All integrations, newest first, as an escaped JSON array for the admin UI. */
drogon::Task<nlohmann::json> list(drogon::orm::DbClientPtr db);

/* One integration by id (with the full webhook_url, for editing), or nullopt. */
drogon::Task<std::optional<Webhook>> get(drogon::orm::DbClientPtr db,
					 uint64_t id);

drogon::Task<uint64_t> create(drogon::orm::DbClientPtr db, int64_t chatId,
			      std::string chatType, std::string chatTitle,
			      std::string webhookUrl, bool enabled);

drogon::Task<void> update(drogon::orm::DbClientPtr db, uint64_t id,
			  int64_t chatId, std::string chatType,
			  std::string chatTitle, std::string webhookUrl,
			  bool enabled);

drogon::Task<void> remove(drogon::orm::DbClientPtr db, uint64_t id);

/*
 * Resolve a Telegram chat_id to its (type, title) by looking it up in the
 * logged schema: a negative id is a group/channel (`telegram_groups`), a positive id is
 * a private chat (`telegram_users`). Returns nullopt when the chat is not in the DB --
 * i.e. the account has never seen it, so it is treated as inaccessible.
 */
drogon::Task<std::optional<ChatRef>> resolveChat(drogon::orm::DbClientPtr db,
						 int64_t chatId);

/*
 * Search groups + users for the select2 chat picker. Returns a RAW (un-escaped)
 * JSON array [{chat_id, text, type, title}] -- select2 escapes result text
 * itself, so escaping here would double-encode.
 */
drogon::Task<nlohmann::json> searchChats(drogon::orm::DbClientPtr db,
					 std::string q, int limit);

} /* namespace tgweb::dao::discord */

#endif /* TGLOGGERD_WEB_DAO_DISCORD_HPP */
