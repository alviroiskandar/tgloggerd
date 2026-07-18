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

/*
 * A LIKE pattern "%q%" with the LIKE metacharacters in q escaped, so a user's
 * literal % or _ matches itself (backslash is LIKE's default escape char). The
 * value is a bound parameter, so this is about match semantics, not injection.
 */
std::string likePattern(const std::string &q)
{
	std::string e;
	for (char c : q) {
		if (c == '\\' || c == '%' || c == '_')
			e += '\\';
		e += c;
	}
	return "%" + e + "%";
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
				       int64_t cursor, int limit,
				       std::string query, std::string field)
{
	/*
	 * A single parameterized statement covers every case: the leading
	 * "? = ''" disables the search filter when no query is given, each
	 * matched column is gated by "? IN ('all', <field>)" so `field` scopes
	 * the search ("all" matches id/name/username), and "? = 0 OR u.id < ?"
	 * makes the cursor optional (cursor 0 = first page). Fetch one extra row
	 * to know whether a further page exists. The SQL is a named local (never
	 * a temporary in the co_await operand) so it outlives the suspension; a
	 * temporary there is mishandled by the coroutine lowering and
	 * double-freed across thread migration.
	 */
	std::string like = likePattern(query);
	std::string q =
		"SELECT u.id, u.first_name, u.last_name, u.type, "
		"u.profile_photo_file_id, "
		"u.is_premium, u.is_verified, u.is_scam, u.is_fake, "
		"(SELECT un.username FROM user_usernames un "
		" WHERE un.user_id = u.id AND un.kind = 'active' "
		" ORDER BY un.position LIMIT 1) AS username "
		"FROM users u "
		"WHERE (? = '' "
		"       OR (CAST(u.id AS CHAR) LIKE ? AND ? IN ('all','id')) "
		"       OR (CONCAT_WS(' ', u.first_name, u.last_name) LIKE ? "
		"           AND ? IN ('all','name')) "
		"       OR (EXISTS (SELECT 1 FROM user_usernames un "
		"                   WHERE un.user_id = u.id AND un.username LIKE ?) "
		"           AND ? IN ('all','username')) "
		"       OR (u.phone_number LIKE ? AND ? IN ('phone')) "
		"       OR (u.bio LIKE ? AND ? IN ('bio'))) "
		"AND (? = 0 OR u.id < ?) "
		"ORDER BY u.id DESC LIMIT ?";
	auto rowsHolder = co_await db->execSqlCoro(
		q, query, like, field, like, field, like, field, like, field,
		like, field, cursor, cursor, limit + 1);
	const drogon::orm::Result &rows = rowsHolder;

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
		if (!r["profile_photo_file_id"].isNull())
			u["photo_file_id"] =
				r["profile_photo_file_id"].as<int64_t>();
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
		"is_close_friend, have_access, has_sensitive_content, "
		"restricts_new_chats, paid_message_star_count, "
		"language_code, bio, personal_chat_id, "
		"accent_color_id, profile_accent_color_id, "
		"background_custom_emoji_id, profile_background_custom_emoji_id, "
		"emoji_status_custom_emoji_id, "
		"IF(emoji_status_expiration_date > 0, "
		"   FROM_UNIXTIME(emoji_status_expiration_date), NULL) "
		"   AS emoji_status_expires, "
		"birthday_day, birthday_month, birthday_year, "
		"restriction_reason, created_at, updated_at "
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
	user["is_mutual_contact"] = r["is_mutual_contact"].as<int>() != 0;
	user["is_close_friend"]   = r["is_close_friend"].as<int>() != 0;
	user["have_access"]       = r["have_access"].as<int>() != 0;
	user["has_sensitive_content"] = r["has_sensitive_content"].as<int>() != 0;
	user["restricts_new_chats"]   = r["restricts_new_chats"].as<int>() != 0;
	user["paid_message_star_count"] = r["paid_message_star_count"].as<int64_t>();
	user["personal_chat_id"]  = r["personal_chat_id"].as<int64_t>();
	user["accent_color_id"]   = r["accent_color_id"].as<int>();
	user["profile_accent_color_id"] = r["profile_accent_color_id"].as<int>();
	user["background_custom_emoji_id"] =
		r["background_custom_emoji_id"].as<int64_t>();
	user["profile_background_custom_emoji_id"] =
		r["profile_background_custom_emoji_id"].as<int64_t>();
	if (!r["emoji_status_custom_emoji_id"].isNull())
		user["emoji_status_custom_emoji_id"] =
			r["emoji_status_custom_emoji_id"].as<int64_t>();
	user["emoji_status_expires"] = escCol(r, "emoji_status_expires");
	/* Birthday: day + month name (+ year), built from digits and static month
	 * names, so it needs no escaping. Omitted entirely when unset. */
	if (!r["birthday_day"].isNull() && !r["birthday_month"].isNull()) {
		static const char *kMonths[] = {
			"January", "February", "March", "April", "May", "June",
			"July", "August", "September", "October", "November",
			"December"};
		int d = r["birthday_day"].as<int>();
		int m = r["birthday_month"].as<int>();
		std::string bday = std::to_string(d) + " " +
			(m >= 1 && m <= 12 ? kMonths[m - 1] : "?");
		if (!r["birthday_year"].isNull())
			bday += " " + std::to_string(r["birthday_year"].as<int>());
		user["birthday"] = bday;
	}
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

namespace {

/* Admin permission columns and their display labels. */
struct Perm { const char *col; const char *label; };
const Perm kPerms[] = {
	{"can_manage_chat",           "manage chat"},
	{"can_change_info",           "change info"},
	{"can_post_messages",         "post messages"},
	{"can_edit_messages",         "edit messages"},
	{"can_delete_messages",       "delete messages"},
	{"can_invite_users",          "invite users"},
	{"can_restrict_members",      "restrict members"},
	{"can_pin_messages",          "pin messages"},
	{"can_manage_topics",         "manage topics"},
	{"can_promote_members",       "promote members"},
	{"can_manage_video_chats",    "manage video chats"},
	{"can_post_stories",          "post stories"},
	{"can_edit_stories",          "edit stories"},
	{"can_delete_stories",        "delete stories"},
	{"can_manage_direct_messages","manage direct messages"},
	{"can_manage_tags",           "manage tags"},
};

/* Names of the permissions granted (value 1) in a group_admins row. */
nlohmann::json grantedPerms(const drogon::orm::Row &r)
{
	nlohmann::json perms = nlohmann::json::array();
	for (const auto &p : kPerms) {
		if (!r[p.col].isNull() && r[p.col].as<int>() != 0)
			perms.push_back(p.label);
	}
	return perms;
}

} /* namespace */

drogon::Task<nlohmann::json> listGroups(drogon::orm::DbClientPtr db,
					int64_t cursor, int limit,
					std::string query, std::string field)
{
	/* See listUsers for the "? = ''" (search), "? IN ('all', <field>)"
	 * (field scope) and "? = 0 OR ..." (cursor) toggles and the named-local
	 * requirement for the SQL string. */
	std::string like = likePattern(query);
	std::string q =
		"SELECT g.id, g.type, g.title, g.photo_file_id, "
		"(SELECT gu.username FROM group_usernames gu "
		" WHERE gu.group_id = g.id AND gu.kind = 'active' "
		" ORDER BY gu.position LIMIT 1) AS username, "
		"(SELECT COUNT(*) FROM group_admins ga "
		" WHERE ga.group_id = g.id) AS admins "
		"FROM `groups` g "
		"WHERE (? = '' "
		"       OR (CAST(g.id AS CHAR) LIKE ? AND ? IN ('all','id')) "
		"       OR (g.title LIKE ? AND ? IN ('all','title')) "
		"       OR (EXISTS (SELECT 1 FROM group_usernames gu "
		"                   WHERE gu.group_id = g.id AND gu.username LIKE ?) "
		"           AND ? IN ('all','username')) "
		"       OR (g.description LIKE ? AND ? IN ('description'))) "
		"AND (? = 0 OR g.id < ?) "
		"ORDER BY g.id DESC LIMIT ?";
	auto rowsHolder = co_await db->execSqlCoro(
		q, query, like, field, like, field, like, field, like, field,
		cursor, cursor, limit + 1);
	const drogon::orm::Result &rows = rowsHolder;

	nlohmann::json groups = nlohmann::json::array();
	int64_t lastId = 0;
	bool haveLast = false;
	int n = 0;
	for (const auto &r : rows) {
		if (n++ >= limit)
			break;
		lastId = r["id"].as<int64_t>();
		haveLast = true;
		nlohmann::json g;
		g["id"]       = lastId;
		g["type"]     = r["type"].as<std::string>();
		g["title"]    = r["title"].isNull()
					? std::string("(no title)")
					: Render::esc(r["title"].as<std::string>());
		if (g["title"].get<std::string>().empty())
			g["title"] = "(no title)";
		g["username"] = escCol(r, "username");
		g["admins"]   = r["admins"].as<int64_t>();
		if (!r["photo_file_id"].isNull())
			g["photo_file_id"] = r["photo_file_id"].as<int64_t>();
		groups.push_back(std::move(g));
	}

	nlohmann::json j;
	j["groups"] = std::move(groups);
	if ((int)rows.size() > limit && haveLast)
		j["next_cursor"] = lastId;
	else
		j["next_cursor"] = nullptr;
	co_return j;
}

drogon::Task<std::optional<nlohmann::json>> getGroup(drogon::orm::DbClientPtr db,
						     int64_t id)
{
	auto gr = co_await db->execSqlCoro(
		"SELECT id, type, title, description, photo_file_id, "
		"created_at, updated_at FROM `groups` WHERE id = ?",
		id);

	if (gr.empty())
		co_return std::nullopt;

	const auto &r = gr[0];
	std::string title = r["title"].isNull() ? "" : r["title"].as<std::string>();
	nlohmann::json group;
	group["id"]          = r["id"].as<int64_t>();
	group["type"]        = r["type"].as<std::string>();
	group["title"]       = Render::esc(title.empty() ? "(no title)" : title);
	group["description"] = escCol(r, "description");
	group["created_at"]  = r["created_at"].as<std::string>();
	group["updated_at"]  = r["updated_at"].as<std::string>();
	if (!r["photo_file_id"].isNull())
		group["photo_file_id"] = r["photo_file_id"].as<int64_t>();

	nlohmann::json j;
	j["group"] = std::move(group);

	/* Current usernames. */
	auto un = co_await db->execSqlCoro(
		"SELECT username, kind, position FROM group_usernames "
		"WHERE group_id = ? ORDER BY kind, position",
		id);
	nlohmann::json usernames = nlohmann::json::array();
	for (const auto &row : un) {
		nlohmann::json e;
		e["username"] = escCol(row, "username");
		e["kind"]     = row["kind"].as<std::string>();
		usernames.push_back(std::move(e));
	}
	j["usernames"] = std::move(usernames);

	/* Current admins, joined to the user for a display name. */
	auto ad = co_await db->execSqlCoro(
		"SELECT ga.*, u.first_name, u.last_name, "
		"(SELECT un.username FROM user_usernames un "
		" WHERE un.user_id = ga.user_id AND un.kind = 'active' "
		" ORDER BY un.position LIMIT 1) AS username "
		"FROM group_admins ga LEFT JOIN users u ON u.id = ga.user_id "
		"WHERE ga.group_id = ? ORDER BY ga.status, ga.user_id",
		id);
	nlohmann::json admins = nlohmann::json::array();
	for (const auto &row : ad) {
		nlohmann::json a;
		a["user_id"]      = row["user_id"].as<int64_t>();
		a["name"]         = displayName(row);
		a["username"]     = escCol(row, "username");
		a["status"]       = row["status"].as<std::string>();
		a["custom_title"] = escCol(row, "custom_title");
		a["is_anonymous"] = row["is_anonymous"].as<int>() != 0;
		a["perms"]        = grantedPerms(row);
		admins.push_back(std::move(a));
	}
	j["admins"] = std::move(admins);

	/* Title history. */
	auto th = co_await db->execSqlCoro(
		"SELECT title, created_at FROM group_hist_title "
		"WHERE group_id = ? ORDER BY id DESC LIMIT 100",
		id);
	nlohmann::json titleHist = nlohmann::json::array();
	for (const auto &row : th) {
		nlohmann::json e;
		e["title"]      = escCol(row, "title");
		e["created_at"] = row["created_at"].as<std::string>();
		titleHist.push_back(std::move(e));
	}
	j["title_hist"] = std::move(titleHist);

	/* Description history. */
	auto dh = co_await db->execSqlCoro(
		"SELECT description, created_at FROM group_hist_description "
		"WHERE group_id = ? ORDER BY id DESC LIMIT 100",
		id);
	nlohmann::json descHist = nlohmann::json::array();
	for (const auto &row : dh) {
		nlohmann::json e;
		e["description"] = escCol(row, "description");
		e["created_at"]  = row["created_at"].as<std::string>();
		descHist.push_back(std::move(e));
	}
	j["desc_hist"] = std::move(descHist);

	/* Username events. */
	auto ue = co_await db->execSqlCoro(
		"SELECT username, action, kind, created_at "
		"FROM group_hist_usernames_events "
		"WHERE group_id = ? ORDER BY id DESC LIMIT 100",
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

	/* Photo history. */
	auto ph = co_await db->execSqlCoro(
		"SELECT file_id, created_at FROM group_hist_photo "
		"WHERE group_id = ? ORDER BY id DESC LIMIT 100",
		id);
	nlohmann::json photoHist = nlohmann::json::array();
	for (const auto &row : ph) {
		nlohmann::json e;
		if (!row["file_id"].isNull())
			e["file_id"] = row["file_id"].as<int64_t>();
		e["created_at"] = row["created_at"].as<std::string>();
		photoHist.push_back(std::move(e));
	}
	j["photo_hist"] = std::move(photoHist);

	/* Admin change history, joined to the user for a display name. */
	auto ah = co_await db->execSqlCoro(
		"SELECT ah.user_id, ah.action, ah.status, ah.custom_title, "
		"ah.created_at, u.first_name, u.last_name "
		"FROM group_admin_hist ah LEFT JOIN users u ON u.id = ah.user_id "
		"WHERE ah.group_id = ? ORDER BY ah.id DESC LIMIT 100",
		id);
	nlohmann::json adminHist = nlohmann::json::array();
	for (const auto &row : ah) {
		nlohmann::json e;
		e["user_id"]      = row["user_id"].as<int64_t>();
		e["name"]         = displayName(row);
		e["action"]       = row["action"].as<std::string>();
		e["status"]       = row["status"].as<std::string>();
		e["custom_title"] = escCol(row, "custom_title");
		e["created_at"]   = row["created_at"].as<std::string>();
		adminHist.push_back(std::move(e));
	}
	j["admin_hist"] = std::move(adminHist);

	co_return j;
}

drogon::Task<std::optional<nlohmann::json>>
getGroupAdmins(drogon::orm::DbClientPtr db, int64_t id)
{
	auto gr = co_await db->execSqlCoro(
		"SELECT id, type, title FROM `groups` WHERE id = ?", id);
	if (gr.empty())
		co_return std::nullopt;

	const auto &g = gr[0];
	std::string title = g["title"].isNull() ? "" : g["title"].as<std::string>();
	nlohmann::json group;
	group["id"]    = g["id"].as<int64_t>();
	group["type"]  = g["type"].as<std::string>();
	group["title"] = Render::esc(title.empty() ? "(no title)" : title);

	/* Same projection as getGroup's admin block; see grantedPerms/kPerms. */
	auto ad = co_await db->execSqlCoro(
		"SELECT ga.*, u.first_name, u.last_name, "
		"(SELECT un.username FROM user_usernames un "
		" WHERE un.user_id = ga.user_id AND un.kind = 'active' "
		" ORDER BY un.position LIMIT 1) AS username "
		"FROM group_admins ga LEFT JOIN users u ON u.id = ga.user_id "
		"WHERE ga.group_id = ? ORDER BY ga.status, ga.user_id",
		id);
	nlohmann::json admins = nlohmann::json::array();
	for (const auto &row : ad) {
		nlohmann::json a;
		a["user_id"]      = row["user_id"].as<int64_t>();
		a["name"]         = displayName(row);
		a["username"]     = escCol(row, "username");
		a["status"]       = row["status"].as<std::string>();
		a["custom_title"] = escCol(row, "custom_title");
		a["is_anonymous"] = row["is_anonymous"].as<int>() != 0;
		a["perms"]        = grantedPerms(row);
		admins.push_back(std::move(a));
	}

	nlohmann::json j;
	j["group"]  = std::move(group);
	j["admins"] = std::move(admins);
	co_return j;
}

namespace {

/* Escaped display name from two name columns, or the fallback if both empty. */
std::string nameOf(const drogon::orm::Row &r, const char *fc, const char *lc,
		   const std::string &fallback)
{
	std::string f = r[fc].isNull() ? "" : r[fc].as<std::string>();
	std::string l = r[lc].isNull() ? "" : r[lc].as<std::string>();
	std::string n = f;
	if (!l.empty()) {
		if (!n.empty())
			n += " ";
		n += l;
	}
	return Render::esc(n.empty() ? fallback : n);
}

/* Escaped column value, or the (escaped) fallback if NULL/empty. */
std::string escOr(const drogon::orm::Row &r, const char *col,
		  const std::string &fallback)
{
	if (r[col].isNull() || r[col].as<std::string>().empty())
		return Render::esc(fallback);
	return Render::esc(r[col].as<std::string>());
}

/* SELECT lists (without WHERE) resolving chat/sender names via joins. The
 * "text" projection differs between the list snippet and the detail view. */
std::string privSelect(const char *textProj)
{
	return std::string(
		"SELECT m.id, m.message_id, m.chat_id, m.sender_id, m.is_outgoing, "
		"IF(m.date>0, FROM_UNIXTIME(m.date), NULL) AS date_str, "
		"m.edit_date, m.content_type, ") + textProj + ", "
		"m.file_id, m.is_deleted, m.is_forwarded, "
		"m.reply_to_id, m.reply_to_chat_id, m.reply_to_msg_id, "
		"cu.first_name AS chat_first, cu.last_name AS chat_last, "
		"su.first_name AS sender_first, su.last_name AS sender_last "
		"FROM private_messages m "
		"LEFT JOIN users cu ON cu.id = m.chat_id "
		"LEFT JOIN users su ON su.id = m.sender_id ";
}

std::string groupSelect(const char *textProj)
{
	return std::string(
		"SELECT m.id, m.message_id, m.chat_id, m.sender_user_id, "
		"m.sender_chat_id, m.is_channel_post, m.author_signature, "
		"m.is_outgoing, IF(m.date>0, FROM_UNIXTIME(m.date), NULL) AS date_str, "
		"m.edit_date, m.content_type, ") + textProj + ", "
		"m.file_id, m.is_deleted, m.is_forwarded, "
		"m.reply_to_id, m.reply_to_chat_id, m.reply_to_msg_id, "
		"g.title AS chat_title, "
		"su.first_name AS sender_first, su.last_name AS sender_last, "
		"sg.title AS sender_chat_title "
		"FROM group_messages m "
		"LEFT JOIN `groups` g ON g.id = m.chat_id "
		"LEFT JOIN users su ON su.id = m.sender_user_id "
		"LEFT JOIN `groups` sg ON sg.id = m.sender_chat_id ";
}

/* Fields shared by list and detail rendering. */
void fillCommon(nlohmann::json &m, const drogon::orm::Row &r)
{
	m["id"]           = r["id"].as<int64_t>();
	m["message_id"]   = r["message_id"].as<int64_t>();
	m["chat_id"]      = r["chat_id"].as<int64_t>();
	m["date"]         = escCol(r, "date_str");
	m["content_type"] = r["content_type"].as<std::string>();
	m["is_deleted"]   = r["is_deleted"].as<int>() != 0;
	m["is_forwarded"] = r["is_forwarded"].as<int>() != 0;
	m["is_edited"]    = !r["edit_date"].isNull() &&
			    r["edit_date"].as<int64_t>() > 0;
	if (!r["file_id"].isNull())
		m["file_id"] = r["file_id"].as<int64_t>();
	if (!r["reply_to_id"].isNull())
		m["reply_to_id"] = r["reply_to_id"].as<int64_t>();
}

/* Chat and sender display names, per scope. */
void fillParties(nlohmann::json &m, const drogon::orm::Row &r,
		 const std::string &scope)
{
	std::string chatId = std::to_string(r["chat_id"].as<int64_t>());
	if (scope == "group") {
		m["chat_name"] = escOr(r, "chat_title", "#" + chatId);
		if (!r["sender_user_id"].isNull()) {
			m["sender_name"] = nameOf(r, "sender_first", "sender_last",
				"#" + std::to_string(r["sender_user_id"].as<int64_t>()));
		} else if (!r["sender_chat_id"].isNull()) {
			m["sender_name"] = escOr(r, "sender_chat_title",
				"#" + std::to_string(r["sender_chat_id"].as<int64_t>()));
		} else if (!r["author_signature"].isNull() &&
			   !r["author_signature"].as<std::string>().empty()) {
			m["sender_name"] = Render::esc(
				r["author_signature"].as<std::string>());
		} else {
			m["sender_name"] = "(self)";
		}
	} else {
		m["chat_name"] = nameOf(r, "chat_first", "chat_last", "#" + chatId);
		if (r["sender_id"].isNull())
			m["sender_name"] = "(self)";
		else
			m["sender_name"] = nameOf(r, "sender_first", "sender_last",
				"#" + std::to_string(r["sender_id"].as<int64_t>()));
	}
}

} /* namespace */

drogon::Task<nlohmann::json> listMessages(drogon::orm::DbClientPtr db,
					  std::string scope,
					  std::optional<int64_t> chatId,
					  int64_t cursor, int limit)
{
	/* LEFT(text,200) snippet + a length probe to flag truncation. */
	const char *snippet =
		"LEFT(m.text, 200) AS snippet, CHAR_LENGTH(m.text) AS text_len";
	std::string sel = scope == "group" ? groupSelect(snippet)
					   : privSelect(snippet);

	/* cursor == 0 sentinel means "first page"; ids and message_ids are all
	 * positive so 0 never collides with a real key. The SQL goes into a
	 * named local (see listUsers) to keep it alive across the suspension. */
	std::optional<drogon::orm::Result> rowsHolder;
	if (chatId) {
		std::string q = sel +
			"WHERE m.chat_id = ? AND (? = 0 OR m.message_id < ?) "
			"ORDER BY m.message_id DESC LIMIT ?";
		rowsHolder = co_await db->execSqlCoro(q, *chatId, cursor, cursor,
						      limit + 1);
	} else {
		std::string q = sel +
			"WHERE (? = 0 OR m.id < ?) ORDER BY m.id DESC LIMIT ?";
		rowsHolder = co_await db->execSqlCoro(q, cursor, cursor, limit + 1);
	}
	const drogon::orm::Result &rows = *rowsHolder;

	nlohmann::json messages = nlohmann::json::array();
	int64_t lastKey = 0;
	bool haveLast = false;
	int n = 0;
	for (const auto &r : rows) {
		if (n++ >= limit)
			break;
		lastKey = chatId ? r["message_id"].as<int64_t>()
				 : r["id"].as<int64_t>();
		haveLast = true;

		nlohmann::json m;
		fillCommon(m, r);
		fillParties(m, r, scope);
		m["snippet"] = escCol(r, "snippet");
		long len = r["text_len"].isNull() ? 0 : r["text_len"].as<long>();
		m["text_truncated"] = len > 200;
		messages.push_back(std::move(m));
	}

	nlohmann::json j;
	j["scope"] = scope;
	j["messages"] = std::move(messages);
	if (chatId)
		j["chat_id"] = *chatId;
	if ((int)rows.size() > limit && haveLast)
		j["next_cursor"] = lastKey;
	else
		j["next_cursor"] = nullptr;
	co_return j;
}

drogon::Task<std::optional<nlohmann::json>> getMessage(drogon::orm::DbClientPtr db,
						       std::string scope,
						       int64_t id)
{
	bool group = scope == "group";
	std::string sel = group ? groupSelect("m.text AS full_text")
				: privSelect("m.text AS full_text");

	/*
	 * Build everything derived from the first result up front and do not
	 * hold a reference into it across the co_awaits below (subsequent queries
	 * reuse the connection and can invalidate earlier result state).
	 */
	nlohmann::json msg;
	bool hasReply = false;
	int64_t replyToId = 0;
	{
		std::string q = sel + "WHERE m.id = ?";
		auto mr = co_await db->execSqlCoro(q, id);
		if (mr.empty())
			co_return std::nullopt;

		const auto &r = mr[0];
		fillCommon(msg, r);
		fillParties(msg, r, scope);
		msg["text"] = escCol(r, "full_text");
		msg["is_outgoing"] = r["is_outgoing"].as<int>() != 0;
		if (group) {
			msg["is_channel_post"] = r["is_channel_post"].as<int>() != 0;
			msg["author_signature"] = escCol(r, "author_signature");
		}
		if (!r["reply_to_id"].isNull()) {
			hasReply = true;
			replyToId = r["reply_to_id"].as<int64_t>();
		}
	}

	nlohmann::json j;
	j["scope"] = scope;
	j["message"] = std::move(msg);

	/* Edit history. */
	std::string editTable = group ? "group_message_edits"
				      : "private_message_edits";
	std::string editFk = group ? "group_message_id" : "private_message_id";
	std::string editQuery =
		"SELECT content_type, LEFT(text, 500) AS snippet, file_id, "
		"IF(edit_date>0, FROM_UNIXTIME(edit_date), NULL) AS edit_date_str "
		"FROM " + editTable + " WHERE " + editFk + " = ? "
		"ORDER BY id DESC LIMIT 100";
	auto er = co_await db->execSqlCoro(editQuery, id);
	nlohmann::json edits = nlohmann::json::array();
	for (const auto &row : er) {
		nlohmann::json e;
		e["content_type"] = row["content_type"].as<std::string>();
		e["snippet"]      = escCol(row, "snippet");
		e["edit_date"]    = escCol(row, "edit_date_str");
		if (!row["file_id"].isNull())
			e["file_id"] = row["file_id"].as<int64_t>();
		edits.push_back(std::move(e));
	}
	j["edits"] = std::move(edits);

	/* Forward info (at most one row). */
	std::string fwdTable = group ? "group_message_fwd_info"
				     : "private_message_fwd_info";
	std::string fwdFk = group ? "group_message_id" : "private_message_id";
	std::string fwdQuery =
		"SELECT origin_type, origin_sender_user_id, origin_sender_name, "
		"origin_chat_id, origin_message_id, "
		"IF(origin_date>0, FROM_UNIXTIME(origin_date), NULL) AS origin_date_str "
		"FROM " + fwdTable + " WHERE " + fwdFk + " = ? LIMIT 1";
	auto fr = co_await db->execSqlCoro(fwdQuery, id);
	if (!fr.empty()) {
		const auto &f = fr[0];
		nlohmann::json fi;
		fi["origin_type"] = f["origin_type"].as<std::string>();
		fi["origin_sender_name"] = escCol(f, "origin_sender_name");
		fi["origin_date"] = escCol(f, "origin_date_str");
		if (!f["origin_sender_user_id"].isNull())
			fi["origin_sender_user_id"] =
				f["origin_sender_user_id"].as<int64_t>();
		if (!f["origin_chat_id"].isNull())
			fi["origin_chat_id"] = f["origin_chat_id"].as<int64_t>();
		if (!f["origin_message_id"].isNull())
			fi["origin_message_id"] =
				f["origin_message_id"].as<int64_t>();
		j["fwd_info"] = std::move(fi);
	}

	/* The message this one replies to, resolved via reply_to_id. */
	if (hasReply) {
		const char *proj = "LEFT(m.text,200) AS snippet, "
				   "CHAR_LENGTH(m.text) AS text_len";
		std::string q = (group ? groupSelect(proj) : privSelect(proj)) +
			"WHERE m.id = ?";
		auto pr = co_await db->execSqlCoro(q, replyToId);
		if (!pr.empty()) {
			nlohmann::json p;
			fillCommon(p, pr[0]);
			fillParties(p, pr[0], scope);
			p["snippet"] = escCol(pr[0], "snippet");
			j["reply_to"] = std::move(p);
		}
	}

	co_return j;
}

drogon::Task<std::optional<FileMeta>> getFile(drogon::orm::DbClientPtr db,
					      int64_t id)
{
	std::string q =
		"SELECT LOWER(HEX(sha256)) AS hex, file_ext, file_type, "
		"orig_file_name, file_size FROM files WHERE id = ?";
	auto r = co_await db->execSqlCoro(q, id);
	if (r.empty())
		co_return std::nullopt;

	const auto &row = r[0];
	FileMeta f;
	f.hex      = row["hex"].as<std::string>();
	f.ext      = row["file_ext"].isNull() ? "" : row["file_ext"].as<std::string>();
	f.fileType = row["file_type"].as<std::string>();
	f.origName = row["orig_file_name"].isNull()
			     ? "" : row["orig_file_name"].as<std::string>();
	f.size     = row["file_size"].as<uint64_t>();
	co_return f;
}

} /* namespace tgweb::dao::browse */
