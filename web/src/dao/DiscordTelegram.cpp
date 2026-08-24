// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "dao/DiscordTelegram.hpp"

#include "views/Render.hpp"

#include <cstdlib>
#include <string>
#include <utility>

namespace tgweb::dao::discord_telegram {

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

/*
 * How a bot is shown to an admin. NEVER derived from the token.
 *
 * A Telegram bot token looks like "<bot_user_id>:<secret>", so the id half is
 * not secret -- it is the bot's own user id, which we already store separately
 * once discordd logs in. Identifying the bot by that id (and its username when
 * known) gives the operator enough to tell two bots apart without the token
 * ever being read out of the database.
 */
std::string botLabel(uint64_t id, int64_t userId, const std::string &username)
{
	if (!username.empty())
		return "@" + username;
	if (userId)
		return std::to_string(userId);
	/* Added through this UI but discordd has not logged it in yet. */
	return "bot #" + std::to_string(id) + " (not connected yet)";
}

} /* namespace */

drogon::Task<nlohmann::json> list(drogon::orm::DbClientPtr db)
{
	/*
	 * Resolve the Telegram chat title live rather than storing a copy that
	 * goes stale on a rename -- the same approach dao::telegram_discord::list takes.
	 * A chat_id is unique to one side, so only one join matches.
	 */
	auto rows = co_await db->execSqlCoro(
		"SELECT r.id, r.discord_channel_id, r.telegram_chat_id, "
		"r.telegram_bot_id, r.enabled, r.created_at, "
		"b.bot_user_id, b.username AS bot_username, "
		"b.enabled AS bot_enabled, "
		"g.title AS group_title, u.first_name, u.last_name "
		"FROM discord_telegram_routes r "
		"JOIN telegram_bots b ON b.id = r.telegram_bot_id "
		"LEFT JOIN `telegram_groups` g "
		"  ON r.telegram_chat_id < 0 AND g.id = r.telegram_chat_id "
		"LEFT JOIN telegram_users u "
		"  ON r.telegram_chat_id > 0 AND u.id = r.telegram_chat_id "
		"ORDER BY r.id DESC");

	nlohmann::json arr = nlohmann::json::array();
	for (const auto &r : rows) {
		const int64_t chatId = r["telegram_chat_id"].as<int64_t>();
		std::string title;
		if (chatId < 0) {
			title = colStr(r, "group_title");
			if (title.empty())
				title = "(untitled)";
		} else {
			title = userName(r);
		}

		const uint64_t botId = r["telegram_bot_id"].as<uint64_t>();
		const int64_t botUser = r["bot_user_id"].isNull()
						? 0
						: r["bot_user_id"].as<int64_t>();

		nlohmann::json j;
		j["id"] = r["id"].as<uint64_t>();
		/*
		 * A snowflake exceeds what a JS number holds exactly, so it
		 * goes to the browser as a string; the template prints it and
		 * the form posts it straight back.
		 */
		j["discord_channel_id"] =
			std::to_string(r["discord_channel_id"].as<uint64_t>());
		j["chat_id"] = chatId;
		j["chat_title"] = Render::esc(title);
		j["telegram_bot_id"] = botId;
		j["bot_label"] =
			Render::esc(botLabel(botId, botUser,
					     colStr(r, "bot_username")));
		j["enabled"] = r["enabled"].as<int>() != 0;
		/* A route is only live when its bot is enabled too. */
		j["bot_enabled"] = r["bot_enabled"].as<int>() != 0;
		j["created_at"] = Render::esc(colStr(r, "created_at"));
		arr.push_back(std::move(j));
	}
	co_return arr;
}

drogon::Task<std::optional<Route>> get(drogon::orm::DbClientPtr db, uint64_t id)
{
	auto rows = co_await db->execSqlCoro(
		"SELECT id, discord_channel_id, telegram_chat_id, "
		"telegram_bot_id, enabled FROM discord_telegram_routes "
		"WHERE id = ?",
		id);
	if (rows.empty())
		co_return std::nullopt;

	const auto &r = rows[0];
	Route out;
	out.id = r["id"].as<uint64_t>();
	out.discordChannelId = r["discord_channel_id"].as<uint64_t>();
	out.telegramChatId = r["telegram_chat_id"].as<int64_t>();
	out.telegramBotId = r["telegram_bot_id"].as<uint64_t>();
	out.enabled = r["enabled"].as<int>() != 0;
	co_return out;
}

drogon::Task<nlohmann::json> listBots(drogon::orm::DbClientPtr db)
{
	/* token is deliberately absent from this SELECT. */
	auto rows = co_await db->execSqlCoro(
		"SELECT id, bot_user_id, username, enabled FROM telegram_bots "
		"ORDER BY id DESC");

	nlohmann::json arr = nlohmann::json::array();
	for (const auto &r : rows) {
		const uint64_t id = r["id"].as<uint64_t>();
		const int64_t userId = r["bot_user_id"].isNull()
					       ? 0
					       : r["bot_user_id"].as<int64_t>();
		nlohmann::json j;
		j["id"] = id;
		j["label"] = Render::esc(
			botLabel(id, userId, colStr(r, "username")));
		/* False until discordd has logged the bot in at least once. */
		j["connected"] = userId != 0;
		j["enabled"] = r["enabled"].as<int>() != 0;
		arr.push_back(std::move(j));
	}
	co_return arr;
}

drogon::Task<uint64_t> internBot(drogon::orm::DbClientPtr db, std::string token)
{
	/*
	 * The token is UNIQUE, so re-adding a bot reuses its row instead of
	 * duplicating it. LAST_INSERT_ID(id) makes the id available even on
	 * the duplicate path, where no insert happens.
	 */
	co_await db->execSqlCoro(
		"INSERT INTO telegram_bots (token) VALUES (?) "
		"ON DUPLICATE KEY UPDATE id = LAST_INSERT_ID(id), enabled = 1",
		token);

	auto rows = co_await db->execSqlCoro(
		"SELECT id FROM telegram_bots WHERE token = ?", token);
	co_return rows.empty() ? 0 : rows[0]["id"].as<uint64_t>();
}

drogon::Task<uint64_t> create(drogon::orm::DbClientPtr db,
			      uint64_t discordChannelId, int64_t telegramChatId,
			      uint64_t telegramBotId, bool enabled)
{
	auto r = co_await db->execSqlCoro(
		"INSERT INTO discord_telegram_routes "
		"(discord_channel_id, telegram_chat_id, telegram_bot_id, enabled) "
		"VALUES (?, ?, ?, ?)",
		discordChannelId, telegramChatId, telegramBotId,
		enabled ? 1 : 0);
	co_return r.insertId();
}

drogon::Task<void> update(drogon::orm::DbClientPtr db, uint64_t id,
			  uint64_t discordChannelId, int64_t telegramChatId,
			  uint64_t telegramBotId, bool enabled)
{
	co_await db->execSqlCoro(
		"UPDATE discord_telegram_routes SET discord_channel_id = ?, "
		"telegram_chat_id = ?, telegram_bot_id = ?, enabled = ? "
		"WHERE id = ?",
		discordChannelId, telegramChatId, telegramBotId,
		enabled ? 1 : 0, id);
	co_return;
}

drogon::Task<void> remove(drogon::orm::DbClientPtr db, uint64_t id)
{
	co_await db->execSqlCoro("DELETE FROM discord_telegram_routes WHERE id = ?",
				 id);
	co_return;
}

drogon::Task<uint64_t> sentCount(drogon::orm::DbClientPtr db, uint64_t id)
{
	auto rows = co_await db->execSqlCoro(
		"SELECT COUNT(1) AS n FROM discord_telegram_sent_messages "
		"WHERE route_id = ?",
		id);
	co_return rows.empty() ? 0 : rows[0]["n"].as<uint64_t>();
}

drogon::Task<bool> duplicateExists(drogon::orm::DbClientPtr db,
				   uint64_t discordChannelId,
				   int64_t telegramChatId, uint64_t excludeId)
{
	auto rows = co_await db->execSqlCoro(
		"SELECT id FROM discord_telegram_routes "
		"WHERE discord_channel_id = ? AND telegram_chat_id = ? "
		"AND id <> ? LIMIT 1",
		discordChannelId, telegramChatId, excludeId);
	co_return !rows.empty();
}

} /* namespace tgweb::dao::discord_telegram */
