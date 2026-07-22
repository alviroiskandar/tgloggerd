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
	auto rows = co_await db->execSqlCoro(
		"SELECT id, chat_id, chat_type, chat_title, webhook_url, enabled, "
		"created_at FROM discord_webhooks ORDER BY id DESC");

	nlohmann::json arr = nlohmann::json::array();
	for (const auto &r : rows) {
		nlohmann::json j;
		j["id"]         = r["id"].as<uint64_t>();
		j["chat_id"]    = r["chat_id"].as<int64_t>();
		j["chat_type"]  = r["chat_type"].as<std::string>();
		j["chat_title"] = Render::esc(colStr(r, "chat_title"));
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
		"SELECT id, chat_id, chat_type, chat_title, webhook_url, enabled "
		"FROM discord_webhooks WHERE id = ?",
		id);
	if (rows.empty())
		co_return std::nullopt;

	const auto &r = rows[0];
	Webhook w;
	w.id         = r["id"].as<uint64_t>();
	w.chatId     = r["chat_id"].as<int64_t>();
	w.chatType   = r["chat_type"].as<std::string>();
	w.chatTitle  = colStr(r, "chat_title");
	w.webhookUrl = colStr(r, "webhook_url");
	w.enabled    = r["enabled"].as<int>() != 0;
	co_return w;
}

drogon::Task<uint64_t> create(drogon::orm::DbClientPtr db, int64_t chatId,
			      std::string chatType, std::string chatTitle,
			      std::string webhookUrl, bool enabled)
{
	auto r = co_await db->execSqlCoro(
		"INSERT INTO discord_webhooks "
		"(chat_id, chat_type, chat_title, webhook_url, enabled) "
		"VALUES (?, ?, ?, ?, ?)",
		chatId, chatType, chatTitle, webhookUrl, enabled ? 1 : 0);
	co_return r.insertId();
}

drogon::Task<void> update(drogon::orm::DbClientPtr db, uint64_t id,
			  int64_t chatId, std::string chatType,
			  std::string chatTitle, std::string webhookUrl,
			  bool enabled)
{
	co_await db->execSqlCoro(
		"UPDATE discord_webhooks SET chat_id = ?, chat_type = ?, "
		"chat_title = ?, webhook_url = ?, enabled = ? WHERE id = ?",
		chatId, chatType, chatTitle, webhookUrl, enabled ? 1 : 0, id);
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
