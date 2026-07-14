// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "dao/Browse.hpp"

#include "views/Render.hpp"

namespace tgweb::dao::browse {

namespace {

using tgweb::views::Render;

/* Escaped string from a column, or "" if the column is NULL. */
std::string escCol(const drogon::orm::Row &r, const char *col)
{
	if (r[col].isNull())
		return std::string();
	return Render::esc(r[col].as<std::string>());
}

/* A display name from first/last name, escaped, never empty. */
std::string displayName(const drogon::orm::Row &r)
{
	std::string first = r["first_name"].isNull()
				    ? "" : r["first_name"].as<std::string>();
	std::string last = r["last_name"].isNull()
				   ? "" : r["last_name"].as<std::string>();
	std::string name = first;
	if (!last.empty()) {
		if (!name.empty())
			name += " ";
		name += last;
	}
	if (name.empty())
		name = "(no name)";
	return Render::esc(name);
}

} /* namespace */

drogon::Task<nlohmann::json> counts(drogon::orm::DbClientPtr db)
{
	auto r = co_await db->execSqlCoro(
		"SELECT "
		"(SELECT COUNT(*) FROM users)            AS users, "
		"(SELECT COUNT(*) FROM `groups`)         AS `groups`, "
		"(SELECT COUNT(*) FROM private_messages) AS private_messages, "
		"(SELECT COUNT(*) FROM group_messages)   AS group_messages, "
		"(SELECT COUNT(*) FROM files)            AS files");

	const auto &row = r[0];
	nlohmann::json j;
	j["users"]            = row["users"].as<int64_t>();
	j["groups"]           = row["groups"].as<int64_t>();
	j["private_messages"] = row["private_messages"].as<int64_t>();
	j["group_messages"]   = row["group_messages"].as<int64_t>();
	j["files"]            = row["files"].as<int64_t>();
	co_return j;
}

drogon::Task<nlohmann::json> listUsers(drogon::orm::DbClientPtr db,
				       int64_t cursor, int limit)
{
	static const char *kSelect =
		"SELECT u.id, u.first_name, u.last_name, u.type, "
		"u.is_premium, u.is_verified, u.is_scam, u.is_fake, "
		"(SELECT un.username FROM user_usernames un "
		" WHERE un.user_id = u.id AND un.kind = 'active' "
		" ORDER BY un.position LIMIT 1) AS username "
		"FROM users u ";

	/* Fetch one extra row to know whether a further page exists. */
	drogon::orm::Result rows = cursor > 0
		? co_await db->execSqlCoro(std::string(kSelect) +
			"WHERE u.id < ? ORDER BY u.id DESC LIMIT ?",
			cursor, limit + 1)
		: co_await db->execSqlCoro(std::string(kSelect) +
			"ORDER BY u.id DESC LIMIT ?",
			limit + 1);

	nlohmann::json users = nlohmann::json::array();
	int64_t lastId = 0;
	int n = 0;
	for (const auto &r : rows) {
		if (n++ >= limit)
			break;
		lastId = r["id"].as<int64_t>();
		nlohmann::json u;
		u["id"]          = lastId;
		u["name"]        = displayName(r);
		u["username"]    = escCol(r, "username");
		u["type"]        = r["type"].as<std::string>();
		u["is_premium"]  = r["is_premium"].as<int>() != 0;
		u["is_verified"] = r["is_verified"].as<int>() != 0;
		u["is_scam"]     = r["is_scam"].as<int>() != 0;
		u["is_fake"]     = r["is_fake"].as<int>() != 0;
		users.push_back(std::move(u));
	}

	nlohmann::json j;
	j["users"] = std::move(users);
	if ((int)rows.size() > limit && lastId > 0)
		j["next_cursor"] = lastId;
	else
		j["next_cursor"] = nullptr;
	co_return j;
}

drogon::Task<std::optional<nlohmann::json>> getUser(drogon::orm::DbClientPtr db,
						    int64_t id)
{
	auto ur = co_await db->execSqlCoro(
		"SELECT id, first_name, last_name, phone_number, type, "
		"profile_photo_file_id, is_verified, is_scam, is_fake, "
		"is_premium, is_support, is_contact, is_mutual_contact, "
		"is_close_friend, language_code, bio, restriction_reason, "
		"created_at, updated_at "
		"FROM users WHERE id = ?",
		id);

	if (ur.empty())
		co_return std::nullopt;

	const auto &r = ur[0];
	nlohmann::json user;
	user["id"]                = r["id"].as<int64_t>();
	user["name"]              = displayName(r);
	user["first_name"]        = escCol(r, "first_name");
	user["last_name"]         = escCol(r, "last_name");
	user["phone_number"]      = escCol(r, "phone_number");
	user["type"]              = r["type"].as<std::string>();
	user["bio"]               = escCol(r, "bio");
	user["language_code"]     = escCol(r, "language_code");
	user["restriction_reason"] = escCol(r, "restriction_reason");
	user["is_verified"]       = r["is_verified"].as<int>() != 0;
	user["is_scam"]           = r["is_scam"].as<int>() != 0;
	user["is_fake"]           = r["is_fake"].as<int>() != 0;
	user["is_premium"]        = r["is_premium"].as<int>() != 0;
	user["is_support"]        = r["is_support"].as<int>() != 0;
	user["is_contact"]        = r["is_contact"].as<int>() != 0;
	user["created_at"]        = r["created_at"].as<std::string>();
	user["updated_at"]        = r["updated_at"].as<std::string>();
	if (!r["profile_photo_file_id"].isNull())
		user["profile_photo_file_id"] =
			r["profile_photo_file_id"].as<int64_t>();

	nlohmann::json j;
	j["user"] = std::move(user);

	/* Current usernames. */
	auto un = co_await db->execSqlCoro(
		"SELECT username, kind, position FROM user_usernames "
		"WHERE user_id = ? ORDER BY kind, position",
		id);
	nlohmann::json usernames = nlohmann::json::array();
	for (const auto &row : un) {
		nlohmann::json e;
		e["username"] = escCol(row, "username");
		e["kind"]     = row["kind"].as<std::string>();
		e["position"] = row["position"].as<int>();
		usernames.push_back(std::move(e));
	}
	j["usernames"] = std::move(usernames);

	/* Name history. */
	auto nh = co_await db->execSqlCoro(
		"SELECT first_name, last_name, created_at FROM user_hist_name "
		"WHERE user_id = ? ORDER BY id DESC LIMIT 100",
		id);
	nlohmann::json nameHist = nlohmann::json::array();
	for (const auto &row : nh) {
		nlohmann::json e;
		e["name"]       = displayName(row);
		e["created_at"] = row["created_at"].as<std::string>();
		nameHist.push_back(std::move(e));
	}
	j["name_hist"] = std::move(nameHist);

	/* Username events. */
	auto ue = co_await db->execSqlCoro(
		"SELECT username, action, kind, position, created_at "
		"FROM user_hist_usernames_events "
		"WHERE user_id = ? ORDER BY id DESC LIMIT 100",
		id);
	nlohmann::json unEvents = nlohmann::json::array();
	for (const auto &row : ue) {
		nlohmann::json e;
		e["username"]   = escCol(row, "username");
		e["action"]     = row["action"].as<std::string>();
		e["kind"]       = row["kind"].isNull()
					  ? "" : row["kind"].as<std::string>();
		e["created_at"] = row["created_at"].as<std::string>();
		unEvents.push_back(std::move(e));
	}
	j["username_events"] = std::move(unEvents);

	/* Bio history. */
	auto bh = co_await db->execSqlCoro(
		"SELECT bio, created_at FROM user_hist_bio "
		"WHERE user_id = ? ORDER BY id DESC LIMIT 100",
		id);
	nlohmann::json bioHist = nlohmann::json::array();
	for (const auto &row : bh) {
		nlohmann::json e;
		e["bio"]        = escCol(row, "bio");
		e["created_at"] = row["created_at"].as<std::string>();
		bioHist.push_back(std::move(e));
	}
	j["bio_hist"] = std::move(bioHist);

	/* Phone number history. */
	auto ph = co_await db->execSqlCoro(
		"SELECT phone_number, created_at FROM user_hist_phone_num "
		"WHERE user_id = ? ORDER BY id DESC LIMIT 100",
		id);
	nlohmann::json phoneHist = nlohmann::json::array();
	for (const auto &row : ph) {
		nlohmann::json e;
		e["phone_number"] = escCol(row, "phone_number");
		e["created_at"]   = row["created_at"].as<std::string>();
		phoneHist.push_back(std::move(e));
	}
	j["phone_hist"] = std::move(phoneHist);

	/* Profile photo history (file ids resolve to /media/<id>). */
	auto phot = co_await db->execSqlCoro(
		"SELECT file_id, created_at FROM user_hist_profile_photo "
		"WHERE user_id = ? ORDER BY id DESC LIMIT 100",
		id);
	nlohmann::json photoHist = nlohmann::json::array();
	for (const auto &row : phot) {
		nlohmann::json e;
		if (!row["file_id"].isNull())
			e["file_id"] = row["file_id"].as<int64_t>();
		e["created_at"] = row["created_at"].as<std::string>();
		photoHist.push_back(std::move(e));
	}
	j["photo_hist"] = std::move(photoHist);

	co_return j;
}

} /* namespace tgweb::dao::browse */
