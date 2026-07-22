// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "dao/Discord.hpp"

#include "views/Render.hpp"

#include <cstdlib>
#include <string>
#include <utility>

namespace tgweb::dao::discord {

namespace {

using tgweb::views::Render;

std::string colStr(const drogon::orm::Row &r, const char *c)
{
	return r[c].isNull() ? std::string() : r[c].as<std::string>();
}

/* A user's display name from first/last, never empty (raw, not escaped). */
std::string userName(const drogon::orm::Row &r)
{
	std::string first = colStr(r, "first_name");
	std::string last  = colStr(r, "last_name");
	std::string name  = first;
	if (!last.empty())
		name += (name.empty() ? "" : " ") + last;
	return name.empty() ? std::string("(no name)") : name;
}

} /* namespace */

drogon::Task<nlohmann::json> list(drogon::orm::DbClientPtr db)
{
	/* Resolve the current chat title on the fly (a group's title, or a user's
	 * name) instead of storing a copy that goes stale on a rename. A chat_id is
	 * unique to one side, so only one join matches. */
	auto rows = co_await db->execSqlCoro(
		"SELECT w.id, w.telegram_chat_id, w.telegram_chat_type, "
		"w.webhook_url, w.enabled, w.created_at, "
		"g.title AS group_title, u.first_name, u.last_name "
		"FROM discord_webhooks w "
		"LEFT JOIN `telegram_groups` g "
		"  ON w.telegram_chat_id < 0 AND g.id = w.telegram_chat_id "
		"LEFT JOIN telegram_users u "
		"  ON w.telegram_chat_id > 0 AND u.id = w.telegram_chat_id "
		"ORDER BY w.id DESC");

	nlohmann::json arr = nlohmann::json::array();
	for (const auto &r : rows) {
		int64_t chatId = r["telegram_chat_id"].as<int64_t>();
		std::string title;
		if (chatId < 0) {
			title = colStr(r, "group_title");
			if (title.empty())
				title = "(untitled)";
		} else {
			title = userName(r); /* "(no name)" when unknown */
		}

		nlohmann::json j;
		j["id"]         = r["id"].as<uint64_t>();
		j["chat_id"]    = chatId;
		j["chat_type"]  = r["telegram_chat_type"].as<std::string>();
		j["chat_title"] = Render::esc(title);
		j["webhook_url"] = Render::esc(colStr(r, "webhook_url"));
		j["enabled"]    = r["enabled"].as<int>() != 0;
		j["created_at"] = Render::esc(colStr(r, "created_at"));
		arr.push_back(std::move(j));
	}
	co_return arr;
}

drogon::Task<std::optional<Webhook>> get(drogon::orm::DbClientPtr db,
					 uint64_t id)
{
	auto rows = co_await db->execSqlCoro(
		"SELECT id, telegram_chat_id, telegram_chat_type, webhook_url, "
		"enabled FROM discord_webhooks WHERE id = ?",
		id);
	if (rows.empty())
		co_return std::nullopt;

	const auto &r = rows[0];
	Webhook w;
	w.id         = r["id"].as<uint64_t>();
	w.chatId     = r["telegram_chat_id"].as<int64_t>();
	w.chatType   = r["telegram_chat_type"].as<std::string>();
	w.webhookUrl = colStr(r, "webhook_url");
	w.enabled    = r["enabled"].as<int>() != 0;
	co_return w;
}

drogon::Task<uint64_t> create(drogon::orm::DbClientPtr db, int64_t chatId,
			      std::string chatType, std::string webhookUrl,
			      bool enabled)
{
	auto r = co_await db->execSqlCoro(
		"INSERT INTO discord_webhooks "
		"(telegram_chat_id, telegram_chat_type, webhook_url, enabled) "
		"VALUES (?, ?, ?, ?)",
		chatId, chatType, webhookUrl, enabled ? 1 : 0);
	co_return r.insertId();
}

drogon::Task<void> update(drogon::orm::DbClientPtr db, uint64_t id,
			  int64_t chatId, std::string chatType,
			  std::string webhookUrl, bool enabled)
{
	co_await db->execSqlCoro(
		"UPDATE discord_webhooks SET telegram_chat_id = ?, "
		"telegram_chat_type = ?, webhook_url = ?, enabled = ? WHERE id = ?",
		chatId, chatType, webhookUrl, enabled ? 1 : 0, id);
	co_return;
}

drogon::Task<void> remove(drogon::orm::DbClientPtr db, uint64_t id)
{
	co_await db->execSqlCoro("DELETE FROM discord_webhooks WHERE id = ?", id);
	co_return;
}

drogon::Task<std::optional<ChatRef>> resolveChat(drogon::orm::DbClientPtr db,
						 int64_t chatId)
{
	/* Negative ids are groups/channels; positive ids are private chats. Query
	 * the matching table (a chat_id is unique to one of them). */
	if (chatId < 0) {
		auto g = co_await db->execSqlCoro(
			"SELECT title FROM `telegram_groups` WHERE id = ?", chatId);
		if (!g.empty()) {
			std::string t = colStr(g[0], "title");
			co_return ChatRef{ chatId, "group",
					   t.empty() ? "(untitled)" : t };
		}
		co_return std::nullopt;
	}

	auto u = co_await db->execSqlCoro(
		"SELECT first_name, last_name FROM telegram_users WHERE id = ?", chatId);
	if (!u.empty())
		co_return ChatRef{ chatId, "private", userName(u[0]) };
	co_return std::nullopt;
}

drogon::Task<nlohmann::json> searchChats(drogon::orm::DbClientPtr db,
					 std::string q, int limit)
{
	std::string like = "%" + q + "%";
	bool numeric = !q.empty() &&
		       q.find_first_not_of("-0123456789") == std::string::npos;
	int64_t qid = numeric ? std::strtoll(q.c_str(), nullptr, 10) : 0;

	nlohmann::json arr = nlohmann::json::array();

	/* Groups / channels first (the common forwarding source). */
	auto gs = co_await db->execSqlCoro(
		"SELECT id, title, type FROM `telegram_groups` "
		"WHERE title LIKE ? OR id = ? ORDER BY title LIMIT ?",
		like, qid, limit);
	for (const auto &r : gs) {
		std::string title = colStr(r, "title");
		if (title.empty())
			title = "(untitled)";
		std::string type = r["type"].as<std::string>();
		nlohmann::json j;
		j["chat_id"] = r["id"].as<int64_t>();
		j["type"]    = "group";
		j["title"]   = title;
		j["text"]    = title + "  \xC2\xB7  " + type; /* middot */
		arr.push_back(std::move(j));
	}

	/* Then private chats (users). */
	auto us = co_await db->execSqlCoro(
		"SELECT id, first_name, last_name FROM telegram_users "
		"WHERE CONCAT_WS(' ', first_name, last_name) LIKE ? OR id = ? "
		"ORDER BY first_name LIMIT ?",
		like, qid, limit);
	for (const auto &r : us) {
		std::string name = userName(r);
		nlohmann::json j;
		j["chat_id"] = r["id"].as<int64_t>();
		j["type"]    = "private";
		j["title"]   = name;
		j["text"]    = name + "  \xC2\xB7  private";
		arr.push_back(std::move(j));
	}

	co_return arr;
}

} /* namespace tgweb::dao::discord */
