// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "dao/Browse.hpp"

#include "views/Render.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

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

/* Like escCol, but preserves line breaks as <br> (for bios, descriptions). */
std::string escColMulti(const drogon::orm::Row &r, const char *col)
{
	if (r[col].isNull())
		return std::string();
	return Render::escMultiline(r[col].as<std::string>());
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

/*
 * Render message text together with its TDLib formatting entities into safe
 * HTML. `text` is UTF-8; `entitiesJson` is the JSON array the daemon stores,
 * whose offset/length are UTF-16 code-unit spans (TDLib's convention). With
 * no valid entities it degrades to plain escaped, <br>-joined text (same as
 * escMultiline).
 *
 * Tags nest with a stack: outer/earlier entities open first and the most
 * recently opened closes first. That is exact for the properly-nested or
 * disjoint entity sets Telegram clients emit; a rare genuine overlap may
 * nest imperfectly but every character is still escaped, so it stays safe.
 */
std::string renderFormatted(const std::string &text,
			    const std::string &entitiesJson)
{
	if (text.empty() || entitiesJson.empty())
		return Render::escMultiline(text);

	nlohmann::json ents;
	try {
		ents = nlohmann::json::parse(entitiesJson);
	} catch (...) {
		return Render::escMultiline(text);
	}
	if (!ents.is_array() || ents.empty())
		return Render::escMultiline(text);

	/*
	 * Map each UTF-16 unit boundary to a byte offset in `text`. A code
	 * point outside the BMP is two UTF-16 units, so it contributes a second
	 * (never a boundary target on its own) entry pointing at its start.
	 */
	std::vector<size_t> byteAt;
	byteAt.reserve(text.size() + 1);
	for (size_t i = 0; i < text.size();) {
		unsigned char c = (unsigned char)text[i];
		int len;
		if ((c & 0x80) == 0)		len = 1;
		else if ((c & 0xE0) == 0xC0)	len = 2;
		else if ((c & 0xF0) == 0xE0)	len = 3;
		else if ((c & 0xF8) == 0xF0)	len = 4;
		else				len = 1; /* invalid lead; skip one */
		byteAt.push_back(i);
		if (len == 4)
			byteAt.push_back(i);	/* surrogate pair: second unit */
		i += (size_t)len;
	}
	byteAt.push_back(text.size());		/* sentinel: end of text */
	size_t totalUnits = byteAt.size() - 1;

	struct Ev { size_t start, end; std::string open, close; };
	std::vector<Ev> evs;
	for (const auto &e : ents) {
		if (!e.is_object() || !e.contains("offset") || !e.contains("length"))
			continue;
		long off = e["offset"].is_number() ? e["offset"].get<long>() : -1;
		long ln  = e["length"].is_number() ? e["length"].get<long>() : -1;
		if (off < 0 || ln <= 0 || (size_t)(off + ln) > totalUnits)
			continue;

		size_t bs = byteAt[(size_t)off];
		size_t be = byteAt[(size_t)(off + ln)];
		std::string type = e.value("type", std::string());
		Ev ev{ bs, be, "", "" };

		if (type == "bold")		{ ev.open = "<b>"; ev.close = "</b>"; }
		else if (type == "italic")	{ ev.open = "<i>"; ev.close = "</i>"; }
		else if (type == "underline")	{ ev.open = "<u>"; ev.close = "</u>"; }
		else if (type == "strikethrough") { ev.open = "<s>"; ev.close = "</s>"; }
		else if (type == "spoiler")	{ ev.open = "<span class=\"spoiler\">"; ev.close = "</span>"; }
		else if (type == "code")	{ ev.open = "<code>"; ev.close = "</code>"; }
		else if (type == "pre" || type == "pre_code")
						{ ev.open = "<pre>"; ev.close = "</pre>"; }
		else if (type == "block_quote" || type == "expandable_block_quote")
						{ ev.open = "<blockquote>"; ev.close = "</blockquote>"; }
		else if (type == "text_url") {
			ev.open = "<a href=\"" + Render::esc(e.value("url", std::string())) +
				  "\" target=\"_blank\" rel=\"noopener nofollow\">";
			ev.close = "</a>";
		} else if (type == "url") {
			ev.open = "<a href=\"" + Render::esc(text.substr(bs, be - bs)) +
				  "\" target=\"_blank\" rel=\"noopener nofollow\">";
			ev.close = "</a>";
		} else if (type == "mention_name") {
			long uid = e.value("user_id", (long)0);
			ev.open = "<a href=\"/users/" + std::to_string(uid) + "\">";
			ev.close = "</a>";
		} else {
			/* mention / hashtag / email / phone / custom_emoji / ...:
			 * meaningful but with no distinct visual wrapper here. */
			continue;
		}
		evs.push_back(std::move(ev));
	}
	if (evs.empty())
		return Render::escMultiline(text);

	/* Entities opening at a byte offset, outermost (largest end) first, so
	 * pushing them left-to-right leaves the innermost on top of the stack. */
	std::unordered_map<size_t, std::vector<size_t>> opensAt;
	for (size_t k = 0; k < evs.size(); k++)
		opensAt[evs[k].start].push_back(k);
	for (auto &kv : opensAt)
		std::sort(kv.second.begin(), kv.second.end(),
			  [&](size_t a, size_t b) { return evs[a].end > evs[b].end; });

	std::string out;
	out.reserve(text.size() + evs.size() * 8);
	std::vector<size_t> stack;
	for (size_t i = 0; i <= text.size(); i++) {
		/* Close entities ending here (innermost, i.e. stack top, first). */
		while (!stack.empty() && evs[stack.back()].end == i) {
			out += evs[stack.back()].close;
			stack.pop_back();
		}
		auto it = opensAt.find(i);
		if (it != opensAt.end()) {
			for (size_t k : it->second) {
				out += evs[k].open;
				stack.push_back(k);
			}
		}
		if (i == text.size())
			break;

		switch ((unsigned char)text[i]) {
		case '&':  out += "&amp;";  break;
		case '<':  out += "&lt;";   break;
		case '>':  out += "&gt;";   break;
		case '"':  out += "&quot;"; break;
		case '\'': out += "&#39;";  break;
		case '\n': out += "<br>";   break;
		case '\r':                  break;
		default:   out += text[i];  break;
		}
	}
	return out;
}

/* renderFormatted from a (text, entities) column pair; "" if text is NULL. */
std::string renderFormattedCols(const drogon::orm::Row &r, const char *textCol,
				const char *entCol)
{
	if (r[textCol].isNull())
		return std::string();
	std::string ent = r[entCol].isNull() ? std::string()
					     : r[entCol].as<std::string>();
	return renderFormatted(r[textCol].as<std::string>(), ent);
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
		"u.profile_photo_file_id, u.created_at, u.updated_at, "
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
		u["created_at"]  = r["created_at"].as<std::string>();
		u["updated_at"]  = r["updated_at"].as<std::string>();
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
	user["bio"]               = escColMulti(r, "bio");
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
		"SELECT username, kind, position, is_collectible FROM user_usernames "
		"WHERE user_id = ? ORDER BY kind, position",
		id);
	nlohmann::json usernames = nlohmann::json::array();
	for (const auto &row : un) {
		nlohmann::json e;
		e["username"]    = escCol(row, "username");
		e["kind"]        = row["kind"].as<std::string>();
		e["position"]    = row["position"].as<int>();
		e["collectible"] = row["is_collectible"].as<int>() != 0;
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
		"SELECT username, action, kind, position, is_collectible, created_at "
		"FROM user_hist_usernames_events "
		"WHERE user_id = ? ORDER BY id DESC LIMIT 100",
		id);
	nlohmann::json unEvents = nlohmann::json::array();
	for (const auto &row : ue) {
		nlohmann::json e;
		e["username"]    = escCol(row, "username");
		e["action"]      = row["action"].as<std::string>();
		e["kind"]        = row["kind"].isNull()
					   ? "" : row["kind"].as<std::string>();
		e["collectible"] = !row["is_collectible"].isNull() &&
				   row["is_collectible"].as<int>() != 0;
		e["created_at"]  = row["created_at"].as<std::string>();
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
		e["bio"]        = escColMulti(row, "bio");
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
		"g.created_at, g.updated_at, "
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
		g["created_at"] = r["created_at"].as<std::string>();
		g["updated_at"] = r["updated_at"].as<std::string>();
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
	group["description"] = escColMulti(r, "description");
	group["created_at"]  = r["created_at"].as<std::string>();
	group["updated_at"]  = r["updated_at"].as<std::string>();
	if (!r["photo_file_id"].isNull())
		group["photo_file_id"] = r["photo_file_id"].as<int64_t>();

	nlohmann::json j;
	j["group"] = std::move(group);

	/* Current usernames. */
	auto un = co_await db->execSqlCoro(
		"SELECT username, kind, position, is_collectible FROM group_usernames "
		"WHERE group_id = ? ORDER BY kind, position",
		id);
	nlohmann::json usernames = nlohmann::json::array();
	for (const auto &row : un) {
		nlohmann::json e;
		e["username"]    = escCol(row, "username");
		e["kind"]        = row["kind"].as<std::string>();
		e["collectible"] = row["is_collectible"].as<int>() != 0;
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
		e["description"] = escColMulti(row, "description");
		e["created_at"]  = row["created_at"].as<std::string>();
		descHist.push_back(std::move(e));
	}
	j["desc_hist"] = std::move(descHist);

	/* Username events. */
	auto ue = co_await db->execSqlCoro(
		"SELECT username, action, kind, is_collectible, created_at "
		"FROM group_hist_usernames_events "
		"WHERE group_id = ? ORDER BY id DESC LIMIT 100",
		id);
	nlohmann::json unEvents = nlohmann::json::array();
	for (const auto &row : ue) {
		nlohmann::json e;
		e["username"]    = escCol(row, "username");
		e["action"]      = row["action"].as<std::string>();
		e["kind"]        = row["kind"].isNull()
					   ? "" : row["kind"].as<std::string>();
		e["collectible"] = !row["is_collectible"].isNull() &&
				   row["is_collectible"].as<int>() != 0;
		e["created_at"]  = row["created_at"].as<std::string>();
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
		"m.file_id, m.deleted_at, m.is_forwarded, "
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
		"m.file_id, m.deleted_at, m.is_forwarded, "
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
	m["is_deleted"]   = !r["deleted_at"].isNull();
	if (!r["deleted_at"].isNull())
		m["deleted_at"] = escCol(r, "deleted_at");
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

namespace {

/* A compact human-readable byte size, e.g. "3.4 MB". */
std::string humanSize(uint64_t bytes)
{
	static const char *unit[] = { "B", "KB", "MB", "GB", "TB" };
	double v = (double)bytes;
	int u = 0;
	while (v >= 1024.0 && u < 4) {
		v /= 1024.0;
		u++;
	}
	char buf[32];
	if (u == 0)
		snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
	else
		snprintf(buf, sizeof(buf), "%.1f %s", v, unit[u]);
	return buf;
}

/* Whether `type` names a real files.file_type value (for the page filter). */
bool validFileType(const std::string &t)
{
	return t == "photo" || t == "video" || t == "document" || t == "audio" ||
	       t == "voice" || t == "sticker" || t == "animation" ||
	       t == "unknown";
}

} /* namespace */

drogon::Task<nlohmann::json> listFiles(drogon::orm::DbClientPtr db,
				       int64_t cursor, int limit,
				       std::string type)
{
	bool filter = validFileType(type);

	/* Keyset by id DESC (newest first); id == 0 sentinel is the first page.
	 * The SQL string lives in a named local so it outlives the suspension. */
	std::string q =
		"SELECT id, file_type, file_ext, orig_file_name, file_size, "
		"hit_count, created_at FROM files "
		"WHERE (? = 0 OR id < ?)";
	if (filter)
		q += " AND file_type = ?";
	q += " ORDER BY id DESC LIMIT ?";

	std::optional<drogon::orm::Result> rowsHolder;
	if (filter)
		rowsHolder = co_await db->execSqlCoro(q, cursor, cursor, type,
						      limit + 1);
	else
		rowsHolder = co_await db->execSqlCoro(q, cursor, cursor, limit + 1);
	const drogon::orm::Result &rows = *rowsHolder;

	nlohmann::json files = nlohmann::json::array();
	int64_t lastKey = 0;
	bool haveLast = false;
	int n = 0;
	for (const auto &r : rows) {
		if (n++ >= limit)
			break;
		lastKey = r["id"].as<int64_t>();
		haveLast = true;

		nlohmann::json f;
		f["id"]         = lastKey;
		f["type"]       = r["file_type"].as<std::string>();
		f["ext"]        = escCol(r, "file_ext");
		f["name"]       = escCol(r, "orig_file_name");
		f["size"]       = r["file_size"].as<uint64_t>();
		f["size_h"]     = humanSize(r["file_size"].as<uint64_t>());
		f["hits"]       = r["hit_count"].as<uint64_t>();
		f["created_at"] = escCol(r, "created_at");
		files.push_back(std::move(f));
	}

	nlohmann::json j;
	j["files"] = std::move(files);
	j["type"]  = filter ? type : std::string("all");
	if ((int)rows.size() > limit && haveLast)
		j["next_cursor"] = lastKey;
	else
		j["next_cursor"] = nullptr;
	co_return j;
}

namespace {

/* How a message's media is rendered in the chat view, from its content type. */
const char *mediaRender(const std::string &ctype, const std::string &ext)
{
	if (ctype == "sticker") {
		/* Stickers come in three formats: a static .webp image, a video
		 * .webm (renders like an animation: loops, muted, no controls),
		 * or a .tgs Lottie animation that cannot be played inline, so it
		 * falls back to a download link. */
		if (ext == "webm")
			return "animation";
		if (ext == "tgs")
			return "lottie";
		return "image";
	}
	if (ctype == "photo")
		return "image";
	if (ctype == "video")
		return "video";
	if (ctype == "animation")
		return "animation";
	if (ctype == "audio" || ctype == "voice")
		return "audio";
	return "file"; /* document, unknown */
}

/*
 * Build the rich per-message JSON for the chat view from one result row. The
 * private and group queries alias their columns to a common shape (see
 * chatHistory), so this handles both scopes. The message's surrogate id is
 * returned via rowId so the caller can attach edit history.
 */
nlohmann::json buildChatMessage(const drogon::orm::Row &r, int64_t &rowId,
				bool group)
{
	rowId = r["id"].as<int64_t>();
	bool outgoing = r["is_outgoing"].as<int>() != 0;

	nlohmann::json m;
	m["msg_id"]          = r["message_id"].as<int64_t>(); /* server id / anchor */
	m["date"]            = escCol(r, "date_str");
	m["content_type"]    = r["content_type"].as<std::string>();
	m["is_deleted"]      = !r["deleted_at"].isNull();
	if (!r["deleted_at"].isNull())
		m["deleted_at"] = escCol(r, "deleted_at");
	m["is_outgoing"]     = outgoing;
	/* Attribute incoming group messages to their sender; a 1:1 chat or one's
	 * own messages need no per-bubble name. */
	m["show_sender"]     = group && !outgoing;
	m["is_edited"]       = !r["edit_date"].isNull() && r["edit_date"].as<int64_t>() > 0;
	m["is_channel_post"] = !r["is_channel_post"].isNull() &&
			       r["is_channel_post"].as<int>() != 0;
	m["text"]            = renderFormattedCols(r, "text", "entities");
	m["edits"]           = nlohmann::json::array();

	/* A system/service message (member joined, title changed, ...) renders
	 * as a centered notice rather than a chat bubble. Its `text` already
	 * holds the human-readable description. */
	m["is_service"]      = r["content_type"].as<std::string>() == "service";
	if (!r["service_type"].isNull())
		m["service_type"] = r["service_type"].as<std::string>();

	/* Sender: a chat/channel, a user, the logged-in account, or unknown. */
	nlohmann::json s;
	if (!r["sender_chat_id"].isNull()) {
		int64_t id = r["sender_chat_id"].as<int64_t>();
		s["kind"] = "group";
		s["id"]   = id;
		s["name"] = escOr(r, "sg_title", "#" + std::to_string(id));
		if (!r["sg_photo"].isNull())
			s["photo_file_id"] = r["sg_photo"].as<int64_t>();
	} else if (!r["sender_user_id"].isNull()) {
		int64_t id = r["sender_user_id"].as<int64_t>();
		s["kind"] = "user";
		s["id"]   = id;
		s["name"] = nameOf(r, "su_first", "su_last", "#" + std::to_string(id));
		s["username"] = escCol(r, "su_username");
		if (!r["su_photo"].isNull())
			s["photo_file_id"] = r["su_photo"].as<int64_t>();
	} else if (m["is_outgoing"].get<bool>()) {
		s["kind"] = "self";
		s["name"] = "You";
	} else {
		s["kind"] = "unknown";
		s["name"] = escOr(r, "author_signature", "(unknown)");
	}
	m["sender"] = std::move(s);

	if (!r["author_signature"].isNull() &&
	    !r["author_signature"].as<std::string>().empty())
		m["author_signature"] = escCol(r, "author_signature");

	/* Media attachment. */
	if (!r["file_id"].isNull()) {
		nlohmann::json med;
		med["file_id"] = r["file_id"].as<int64_t>();
		std::string ext = r["f_ext"].isNull() ? std::string()
						      : r["f_ext"].as<std::string>();
		med["render"]  = mediaRender(m["content_type"].get<std::string>(), ext);
		med["name"]    = escCol(r, "f_name");
		if (!r["f_size"].isNull())
			med["size"] = r["f_size"].as<uint64_t>();
		m["media"] = std::move(med);
	}

	/* Reply preview. reply_to_id resolves a same-table target (rich preview);
	 * otherwise reply_to_msg_id marks a reply we cannot preview here. */
	if (!r["r_msg_id"].isNull()) {
		nlohmann::json rep;
		rep["msg_id"]       = r["r_msg_id"].as<int64_t>();
		rep["in_chat"]      = true;
		rep["sender_name"]  = !r["r_sg_title"].isNull()
			? escCol(r, "r_sg_title")
			: nameOf(r, "r_su_first", "r_su_last", "");
		rep["snippet"]      = escCol(r, "r_snippet");
		rep["deleted"]      = !r["r_deleted_at"].isNull();
		rep["content_type"] = r["r_ctype"].isNull() ? "" : r["r_ctype"].as<std::string>();
		m["reply"] = std::move(rep);
	} else if (!r["reply_to_msg_id"].isNull()) {
		nlohmann::json rep;
		rep["msg_id"]  = r["reply_to_msg_id"].as<int64_t>();
		rep["in_chat"] = false;
		m["reply"] = std::move(rep);
	}

	/* Forward origin. */
	if (!r["origin_type"].isNull()) {
		nlohmann::json fw;
		fw["type"]        = r["origin_type"].as<std::string>();
		fw["sender_name"] = escCol(r, "origin_sender_name");
		if (!r["origin_sender_user_id"].isNull())
			fw["user_id"] = r["origin_sender_user_id"].as<int64_t>();
		if (!r["origin_chat_id"].isNull())
			fw["chat_id"] = r["origin_chat_id"].as<int64_t>();
		m["forward"] = std::move(fw);
	}

	return m;
}

} /* namespace */

drogon::Task<std::optional<nlohmann::json>>
chatHeader(drogon::orm::DbClientPtr db, std::string scope, int64_t chatId)
{
	nlohmann::json h;
	if (scope == "group") {
		auto r = co_await db->execSqlCoro(
			"SELECT id, type, title, photo_file_id FROM `groups` WHERE id = ?",
			chatId);
		if (r.empty())
			co_return std::nullopt;
		const auto &g = r[0];
		std::string title = g["title"].isNull() ? "" : g["title"].as<std::string>();
		h["kind"]  = "group";
		h["id"]    = g["id"].as<int64_t>();
		h["type"]  = g["type"].as<std::string>();
		h["title"] = Render::esc(title.empty() ? "(no title)" : title);
		if (!g["photo_file_id"].isNull())
			h["photo_file_id"] = g["photo_file_id"].as<int64_t>();
	} else {
		auto r = co_await db->execSqlCoro(
			"SELECT id, type, first_name, last_name, profile_photo_file_id "
			"FROM users WHERE id = ?",
			chatId);
		if (r.empty())
			co_return std::nullopt;
		const auto &u = r[0];
		h["kind"]  = "user";
		h["id"]    = u["id"].as<int64_t>();
		h["type"]  = u["type"].as<std::string>();
		h["title"] = displayName(u);
		if (!u["profile_photo_file_id"].isNull())
			h["photo_file_id"] = u["profile_photo_file_id"].as<int64_t>();
	}
	co_return h;
}

drogon::Task<nlohmann::json> chatHistory(drogon::orm::DbClientPtr db,
					 std::string scope, int64_t chatId,
					 int limit)
{
	bool group = scope == "group";

	/* One rich row per message. The private query aliases its columns to the
	 * group shape (constant NULL/0 for the group-only columns) so a single
	 * builder handles both. Newest-first here; reversed to oldest-first below. */
	std::string q = group ?
		"SELECT m.id, m.message_id, m.sender_user_id, m.sender_chat_id, "
		"m.is_outgoing, m.is_channel_post, m.author_signature, "
		"IF(m.date>0, FROM_UNIXTIME(m.date), NULL) AS date_str, "
		"m.edit_date, m.content_type, m.text, m.entities, m.service_type, "
		"m.file_id, m.deleted_at, "
		"m.is_forwarded, m.reply_to_id, m.reply_to_chat_id, m.reply_to_msg_id, "
		"su.first_name AS su_first, su.last_name AS su_last, "
		"su.profile_photo_file_id AS su_photo, "
		"(SELECT un.username FROM user_usernames un WHERE un.user_id = m.sender_user_id "
		" AND un.kind='active' ORDER BY un.position LIMIT 1) AS su_username, "
		"sg.title AS sg_title, sg.photo_file_id AS sg_photo, "
		"f.file_type AS f_type, f.file_ext AS f_ext, f.orig_file_name AS f_name, f.file_size AS f_size, "
		"fw.origin_type, fw.origin_sender_user_id, fw.origin_sender_name, "
		"fw.origin_chat_id, fw.origin_message_id, "
		"rm.message_id AS r_msg_id, rm.deleted_at AS r_deleted_at, "
		"rm.content_type AS r_ctype, LEFT(rm.text,120) AS r_snippet, "
		"rsu.first_name AS r_su_first, rsu.last_name AS r_su_last, "
		"rsg.title AS r_sg_title "
		"FROM group_messages m "
		"LEFT JOIN users su ON su.id = m.sender_user_id "
		"LEFT JOIN `groups` sg ON sg.id = m.sender_chat_id "
		"LEFT JOIN files f ON f.id = m.file_id "
		"LEFT JOIN group_message_fwd_info fw ON fw.group_message_id = m.id "
		"LEFT JOIN group_messages rm ON rm.id = m.reply_to_id "
		"LEFT JOIN users rsu ON rsu.id = rm.sender_user_id "
		"LEFT JOIN `groups` rsg ON rsg.id = rm.sender_chat_id "
		"WHERE m.chat_id = ? ORDER BY m.message_id DESC LIMIT ?"
		:
		"SELECT m.id, m.message_id, m.sender_id AS sender_user_id, "
		"NULL AS sender_chat_id, m.is_outgoing, 0 AS is_channel_post, "
		"NULL AS author_signature, "
		"IF(m.date>0, FROM_UNIXTIME(m.date), NULL) AS date_str, "
		"m.edit_date, m.content_type, m.text, m.entities, m.service_type, "
		"m.file_id, m.deleted_at, "
		"m.is_forwarded, m.reply_to_id, m.reply_to_chat_id, m.reply_to_msg_id, "
		"su.first_name AS su_first, su.last_name AS su_last, "
		"su.profile_photo_file_id AS su_photo, "
		"(SELECT un.username FROM user_usernames un WHERE un.user_id = m.sender_id "
		" AND un.kind='active' ORDER BY un.position LIMIT 1) AS su_username, "
		"NULL AS sg_title, NULL AS sg_photo, "
		"f.file_type AS f_type, f.file_ext AS f_ext, f.orig_file_name AS f_name, f.file_size AS f_size, "
		"fw.origin_type, fw.origin_sender_user_id, fw.origin_sender_name, "
		"fw.origin_chat_id, fw.origin_message_id, "
		"rm.message_id AS r_msg_id, rm.deleted_at AS r_deleted_at, "
		"rm.content_type AS r_ctype, LEFT(rm.text,120) AS r_snippet, "
		"rsu.first_name AS r_su_first, rsu.last_name AS r_su_last, "
		"NULL AS r_sg_title "
		"FROM private_messages m "
		"LEFT JOIN users su ON su.id = m.sender_id "
		"LEFT JOIN files f ON f.id = m.file_id "
		"LEFT JOIN private_message_fwd_info fw ON fw.private_message_id = m.id "
		"LEFT JOIN private_messages rm ON rm.id = m.reply_to_id "
		"LEFT JOIN users rsu ON rsu.id = rm.sender_id "
		"WHERE m.chat_id = ? ORDER BY m.message_id DESC LIMIT ?";

	auto rows = co_await db->execSqlCoro(q, chatId, limit);

	std::vector<nlohmann::json> msgs;
	std::vector<int64_t> rowIds;
	std::vector<int64_t> editedIds;
	for (const auto &r : rows) {
		int64_t rid = 0;
		nlohmann::json m = buildChatMessage(r, rid, group);
		if (m["is_edited"].get<bool>())
			editedIds.push_back(rid);
		rowIds.push_back(rid);
		msgs.push_back(std::move(m));
	}

	/* Edit history (snapshot BEFORE each edit) for the edited messages. The
	 * ids are our own integers, so inlining them in IN() is injection-safe and
	 * sidesteps a variadic bind of unknown arity. */
	if (!editedIds.empty()) {
		std::string idlist;
		for (size_t i = 0; i < editedIds.size(); i++) {
			if (i)
				idlist += ",";
			idlist += std::to_string(editedIds[i]);
		}
		std::string fk  = group ? "group_message_id" : "private_message_id";
		std::string tbl = group ? "group_message_edits" : "private_message_edits";
		std::string eq =
			"SELECT " + fk + " AS mid, content_type, "
			"LEFT(text, 4000) AS snippet, entities, file_id, "
			"IF(edit_date>0, FROM_UNIXTIME(edit_date), NULL) AS edit_date_str "
			"FROM " + tbl + " WHERE " + fk + " IN (" + idlist + ") ORDER BY id";
		auto er = co_await db->execSqlCoro(eq);

		std::unordered_map<int64_t, nlohmann::json> editMap;
		for (const auto &row : er) {
			nlohmann::json e;
			e["content_type"] = row["content_type"].as<std::string>();
			e["text"]         = renderFormattedCols(row, "snippet", "entities");
			e["edit_date"]    = escCol(row, "edit_date_str");
			if (!row["file_id"].isNull())
				e["file_id"] = row["file_id"].as<int64_t>();
			int64_t mid = row["mid"].as<int64_t>();
			if (!editMap.count(mid))
				editMap[mid] = nlohmann::json::array();
			editMap[mid].push_back(std::move(e));
		}
		for (size_t i = 0; i < msgs.size(); i++) {
			auto it = editMap.find(rowIds[i]);
			if (it != editMap.end())
				msgs[i]["edits"] = std::move(it->second);
		}
	}

	/* Reverse to oldest-first for rendering; front() is then the oldest. */
	nlohmann::json out = nlohmann::json::array();
	for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
		out.push_back(std::move(*it));

	nlohmann::json j;
	j["oldest_msg_id"] = out.empty() ? 0 : out.front()["msg_id"].get<int64_t>();
	j["messages"] = std::move(out);
	co_return j;
}

} /* namespace tgweb::dao::browse */
