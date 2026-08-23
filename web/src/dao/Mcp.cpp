// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "dao/Mcp.hpp"

#include "views/Render.hpp"

#include <string>
#include <utility>

namespace tgweb::dao::mcp {

namespace {

using tgweb::views::Render;

std::string colStr(const drogon::orm::Row &r, const char *c)
{
	return r[c].isNull() ? std::string() : r[c].as<std::string>();
}

/*
 * The derived publicness signal, as a correlated subquery. A group is public
 * on Telegram exactly when it currently owns an active username: releasing one
 * NULLs its group_id, so this is live rather than historical.
 *
 * Shown to admins as a hint. It is NOT what gates the MCP server -- that is
 * membership of telegram_public_groups, decided by a human.
 */
constexpr const char *IS_PUBLIC_NOW =
	"EXISTS (SELECT 1 FROM telegram_group_usernames gu "
	"        WHERE gu.group_id = g.id AND gu.kind = 'active')";

/* The group's current public username, or NULL. */
constexpr const char *CUR_USERNAME =
	"(SELECT gu.username FROM telegram_group_usernames gu "
	"  WHERE gu.group_id = g.id AND gu.kind = 'active' "
	"  ORDER BY gu.position LIMIT 1)";

} /* namespace */

drogon::Task<nlohmann::json> listAllowed(drogon::orm::DbClientPtr db)
{
	const std::string sql =
		std::string("SELECT p.group_id, p.note, p.added_by, p.created_at, "
			    "g.title, g.type, g.msg_count, ") +
		CUR_USERNAME + " AS username, " + IS_PUBLIC_NOW +
		" AS is_public_now "
		"FROM telegram_public_groups p "
		"JOIN `telegram_groups` g ON g.id = p.group_id "
		"ORDER BY p.created_at DESC";

	auto rows = co_await db->execSqlCoro(sql);

	nlohmann::json arr = nlohmann::json::array();
	for (const auto &r : rows) {
		nlohmann::json j;
		j["group_id"] = r["group_id"].as<int64_t>();
		j["title"] = Render::esc(colStr(r, "title"));
		j["type"] = Render::esc(colStr(r, "type"));
		j["username"] = Render::esc(colStr(r, "username"));
		j["msg_count"] = Render::esc(colStr(r, "msg_count"));
		j["note"] = Render::esc(colStr(r, "note"));
		j["created_at"] = Render::esc(colStr(r, "created_at"));
		j["is_public_now"] = r["is_public_now"].as<int>() != 0;
		arr.push_back(std::move(j));
	}
	co_return arr;
}

drogon::Task<bool> groupExists(drogon::orm::DbClientPtr db, int64_t groupId)
{
	auto rows = co_await db->execSqlCoro(
		"SELECT 1 FROM `telegram_groups` WHERE id = ? LIMIT 1", groupId);
	co_return !rows.empty();
}

drogon::Task<bool> isAllowed(drogon::orm::DbClientPtr db, int64_t groupId)
{
	auto rows = co_await db->execSqlCoro(
		"SELECT 1 FROM telegram_public_groups WHERE group_id = ? LIMIT 1",
		groupId);
	co_return !rows.empty();
}

drogon::Task<void> allowGroup(drogon::orm::DbClientPtr db, int64_t groupId,
			      std::string note, uint64_t addedBy)
{
	/*
	 * INSERT IGNORE, not ON DUPLICATE KEY UPDATE: the web user is granted
	 * INSERT and DELETE on this table but deliberately not UPDATE, because
	 * a row here is a decision to expose a group and is meant to be added
	 * or withdrawn, never quietly edited. Changing a note means removing
	 * and re-adding, which leaves the intent visible in created_at.
	 *
	 * The caller checks membership first, so reaching the IGNORE path means
	 * a genuine race between two admins -- in which case doing nothing is
	 * right.
	 */
	co_await db->execSqlCoro(
		"INSERT IGNORE INTO telegram_public_groups "
		"(group_id, note, added_by) VALUES (?, ?, ?)",
		groupId, note, addedBy);
	co_return;
}

drogon::Task<void> disallowGroup(drogon::orm::DbClientPtr db, int64_t groupId)
{
	co_await db->execSqlCoro(
		"DELETE FROM telegram_public_groups WHERE group_id = ?", groupId);
	co_return;
}

drogon::Task<nlohmann::json> searchGroups(drogon::orm::DbClientPtr db,
					  std::string q, int limit)
{
	if (limit < 1)
		limit = 1;
	if (limit > 50)
		limit = 50;

	const std::string like = "%" + q + "%";
	const std::string sql =
		std::string("SELECT g.id, g.title, g.type, g.msg_count, ") +
		CUR_USERNAME + " AS username, " + IS_PUBLIC_NOW +
		" AS is_public_now "
		"FROM `telegram_groups` g "
		"WHERE g.title LIKE ? "
		/* Exclude what is already allowed; re-adding is a no-op. */
		"  AND NOT EXISTS (SELECT 1 FROM telegram_public_groups p "
		"                   WHERE p.group_id = g.id) "
		/* Busiest first: an empty group is rarely what is wanted. */
		"ORDER BY g.msg_count DESC LIMIT " + std::to_string(limit);

	auto rows = co_await db->execSqlCoro(sql, like);

	/* RAW text: select2 escapes result text itself. */
	nlohmann::json arr = nlohmann::json::array();
	for (const auto &r : rows) {
		const std::string title = colStr(r, "title");
		const std::string uname = colStr(r, "username");
		const bool pub = r["is_public_now"].as<int>() != 0;

		std::string text = title.empty() ? "(untitled)" : title;
		if (!uname.empty())
			text += "  ·  @" + uname;
		text += pub ? "  ·  public" : "  ·  no public username";

		nlohmann::json j;
		j["group_id"] = r["id"].as<int64_t>();
		j["text"] = text;
		j["is_public_now"] = pub;
		arr.push_back(std::move(j));
	}
	co_return arr;
}

drogon::Task<nlohmann::json> listTokens(drogon::orm::DbClientPtr db)
{
	auto rows = co_await db->execSqlCoro(
		"SELECT t.id, t.name, t.created_at, t.last_used_at, "
		"t.revoked_at, u.username "
		"FROM web_mcp_tokens t "
		"JOIN web_users u ON u.id = t.web_user_id "
		"ORDER BY t.id DESC");

	nlohmann::json arr = nlohmann::json::array();
	for (const auto &r : rows) {
		nlohmann::json j;
		j["id"] = r["id"].as<uint64_t>();
		j["name"] = Render::esc(colStr(r, "name"));
		j["username"] = Render::esc(colStr(r, "username"));
		j["created_at"] = Render::esc(colStr(r, "created_at"));
		j["last_used_at"] = Render::esc(colStr(r, "last_used_at"));
		j["revoked"] = !r["revoked_at"].isNull();
		j["revoked_at"] = Render::esc(colStr(r, "revoked_at"));
		arr.push_back(std::move(j));
	}
	co_return arr;
}

drogon::Task<uint64_t> createToken(drogon::orm::DbClientPtr db,
				   uint64_t webUserId, std::string name,
				   std::string sha256)
{
	auto r = co_await db->execSqlCoro(
		"INSERT INTO web_mcp_tokens (web_user_id, name, token_sha256) "
		"VALUES (?, ?, ?)",
		webUserId, name, sha256);
	co_return r.insertId();
}

drogon::Task<void> revokeToken(drogon::orm::DbClientPtr db, uint64_t id)
{
	co_await db->execSqlCoro("UPDATE web_mcp_tokens SET revoked_at = NOW() "
				 "WHERE id = ? AND revoked_at IS NULL",
				 id);
	co_return;
}

drogon::Task<std::optional<TokenOwner>> resolveToken(drogon::orm::DbClientPtr db,
						     std::string sha256)
{
	auto rows = co_await db->execSqlCoro(
		"SELECT t.id, t.web_user_id, u.username, u.role "
		"FROM web_mcp_tokens t "
		"JOIN web_users u ON u.id = t.web_user_id "
		/* Both gates matter: a revoked token and a deactivated account
		 * must each be enough on their own to refuse. */
		"WHERE t.token_sha256 = ? AND t.revoked_at IS NULL "
		"  AND u.is_active = 1 LIMIT 1",
		sha256);
	if (rows.empty())
		co_return std::nullopt;

	TokenOwner o;
	o.tokenId = rows[0]["id"].as<uint64_t>();
	o.webUserId = rows[0]["web_user_id"].as<uint64_t>();
	o.username = colStr(rows[0], "username");
	o.role = colStr(rows[0], "role");
	co_return o;
}

drogon::Task<void> touchToken(drogon::orm::DbClientPtr db, uint64_t tokenId)
{
	co_await db->execSqlCoro(
		"UPDATE web_mcp_tokens SET last_used_at = NOW() WHERE id = ?",
		tokenId);
	co_return;
}

} /* namespace tgweb::dao::mcp */
