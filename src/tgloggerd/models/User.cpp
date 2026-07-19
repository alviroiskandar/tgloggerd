// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <tgloggerd/DB.hpp>

#include <string>
#include <vector>
#include <utility>
#include <unordered_map>

namespace tgloggerd {

namespace {

const char *user_type_to_string(models::UserType t)
{
	switch (t) {
	case models::UserType::Regular:	return "regular";
	case models::UserType::Deleted:	return "deleted";
	case models::UserType::Bot:	return "bot";
	case models::UserType::Unknown:	return "unknown";
	}
	return "unknown";
}

mysql::Param b(bool v)
{
	return (int64_t)(v ? 1 : 0);
}

} /* namespace */

void DB::upsertUser(const models::User &u)
{
	/*
	 * Note: profile_photo_file_id, the birthday_* columns and the
	 * created_at/updated_at columns are intentionally omitted here; the
	 * photo reference is managed after download, and the birthday arrives
	 * with userFullInfo. The sparse attributes (phone, appearance, ...)
	 * live in user_extra_info, written by upsertUserExtraFromUser below.
	 */
	static const char *sql =
		"INSERT INTO users ("
		" id, first_name, last_name, type, accent_color_id,"
		" is_verified, is_scam, is_fake, is_premium, is_support"
		") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
		" AS new ON DUPLICATE KEY UPDATE"
		" first_name = new.first_name,"
		" last_name = new.last_name,"
		" type = new.type,"
		" accent_color_id = new.accent_color_id,"
		" is_verified = new.is_verified,"
		" is_scam = new.is_scam,"
		" is_fake = new.is_fake,"
		" is_premium = new.is_premium,"
		" is_support = new.is_support";

	db_.transaction([&](mysql::Transaction &tx) {
		/*
		 * Fetch the current name (users) and phone number
		 * (user_extra_info) in one query so we can detect changes
		 * (existing user) or record first-seen values (new user).
		 */
		auto old = tx.query(
			"SELECT u.first_name, u.last_name, e.phone_number"
			" FROM users u"
			" LEFT JOIN user_extra_info e ON e.user_id = u.id"
			" WHERE u.id = ?",
			{ (int64_t)u.id });

		tx.execute(sql, {
			(int64_t)u.id,
			u.first_name,
			u.last_name,
			std::string(user_type_to_string(u.type)),
			(int64_t)u.accent_color_id,
			b(u.is_verified),
			b(u.is_scam),
			b(u.is_fake),
			b(u.is_premium),
			b(u.is_support),
		});

		if (old.empty()) {
			/*
			 * First time seeing this user - record initial
			 * values. Runs after the UPSERT so the FK exists.
			 */
			tx.insert("INSERT INTO user_hist_name"
				  " (user_id, first_name, last_name)"
				  " VALUES (?, ?, ?)",
				  { (int64_t)u.id, u.first_name,
				    u.last_name });
			tx.insert("INSERT INTO user_hist_phone_num"
				  " (user_id, phone_number)"
				  " VALUES (?, ?)",
				  { (int64_t)u.id, u.phone_number });
		} else {
			auto &r = old[0];
			std::string of = r[0].value_or("");
			std::string ol = r[1].value_or("");
			if (of != u.first_name || ol != u.last_name) {
				tx.insert("INSERT INTO user_hist_name"
					  " (user_id, first_name,"
					  " last_name)"
					  " VALUES (?, ?, ?)",
					  { (int64_t)u.id, of, ol });
			}

			std::string op = r[2].value_or("");
			if (op != u.phone_number) {
				tx.insert("INSERT INTO user_hist_phone_num"
					  " (user_id, phone_number)"
					  " VALUES (?, ?)",
					  { (int64_t)u.id, op });
			}
		}

		upsertUserExtraFromUser(tx, u);
		syncUsernames(tx, u);
	});
}

void DB::upsertUserExtraFromUser(mysql::Transaction &tx, const models::User &u)
{
	mysql::Param emoji_id = std::monostate{};
	if (u.emoji_status_custom_emoji_id.has_value())
		emoji_id = (int64_t)*u.emoji_status_custom_emoji_id;

	mysql::Param emoji_exp = std::monostate{};
	if (u.emoji_status_expiration_date.has_value())
		emoji_exp = (int64_t)*u.emoji_status_expiration_date;

	/* Only the columns sourced from the user object; bio/personal_chat_id
	 * are owned by the full-info path and left untouched. */
	tx.execute(
		"INSERT INTO user_extra_info ("
		" user_id, phone_number, background_custom_emoji_id,"
		" profile_accent_color_id, profile_background_custom_emoji_id,"
		" emoji_status_custom_emoji_id, emoji_status_expiration_date,"
		" restriction_reason, language_code, has_sensitive_content,"
		" restricts_new_chats, paid_message_star_count"
		") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
		" AS new ON DUPLICATE KEY UPDATE"
		" phone_number = new.phone_number,"
		" background_custom_emoji_id = new.background_custom_emoji_id,"
		" profile_accent_color_id = new.profile_accent_color_id,"
		" profile_background_custom_emoji_id = new.profile_background_custom_emoji_id,"
		" emoji_status_custom_emoji_id = new.emoji_status_custom_emoji_id,"
		" emoji_status_expiration_date = new.emoji_status_expiration_date,"
		" restriction_reason = new.restriction_reason,"
		" language_code = new.language_code,"
		" has_sensitive_content = new.has_sensitive_content,"
		" restricts_new_chats = new.restricts_new_chats,"
		" paid_message_star_count = new.paid_message_star_count",
		{
			(int64_t)u.id,
			u.phone_number,
			(int64_t)u.background_custom_emoji_id,
			(int64_t)u.profile_accent_color_id,
			(int64_t)u.profile_background_custom_emoji_id,
			emoji_id,
			emoji_exp,
			u.restriction_reason,
			u.language_code,
			b(u.has_sensitive_content),
			b(u.restricts_new_chats),
			(int64_t)u.paid_message_star_count,
		});

	pruneUserExtraIfEmpty(tx, u.id);
}

void DB::upsertUserExtraFromFullInfo(mysql::Transaction &tx,
				     const models::UserFullInfo &fi)
{
	/*
	 * Link the personal chat only when its channel is already stored, so
	 * the FK to groups holds. If it is not stored yet the value is NULL for
	 * now; the channel fetch was kicked off on the TDLib side and a later
	 * full-info refresh links it.
	 */
	mysql::Param personal = std::monostate{};
	if (fi.personal_chat_id != 0) {
		auto g = tx.query("SELECT id FROM `groups` WHERE id = ?",
				  { (int64_t)fi.personal_chat_id });
		if (!g.empty())
			personal = (int64_t)fi.personal_chat_id;
	}

	tx.execute(
		"INSERT INTO user_extra_info (user_id, bio, personal_chat_id)"
		" VALUES (?, ?, ?) AS new ON DUPLICATE KEY UPDATE"
		" bio = new.bio, personal_chat_id = new.personal_chat_id",
		{ (int64_t)fi.user_id, fi.bio, personal });

	pruneUserExtraIfEmpty(tx, fi.user_id);
}

void DB::pruneUserExtraIfEmpty(mysql::Transaction &tx, int64_t user_id)
{
	/* A row is kept only while at least one column differs from its default
	 * (the "empty" sentinel documented per column); otherwise it is dropped
	 * so unset users cost no storage. */
	tx.execute(
		"DELETE FROM user_extra_info WHERE user_id = ?"
		" AND bio = '' AND phone_number = ''"
		" AND background_custom_emoji_id = 0"
		" AND profile_accent_color_id = -1"
		" AND profile_background_custom_emoji_id = 0"
		" AND emoji_status_custom_emoji_id IS NULL"
		" AND emoji_status_expiration_date IS NULL"
		" AND restriction_reason = '' AND language_code = ''"
		" AND has_sensitive_content = 0 AND restricts_new_chats = 0"
		" AND paid_message_star_count = 0"
		" AND personal_chat_id IS NULL",
		{ user_id });
}

void DB::setUserProfilePhoto(int64_t user_id, uint64_t file_id)
{
	db_.transaction([&](mysql::Transaction &tx) {
		trackProfilePhotoChange(tx, user_id, file_id);
		tx.execute("UPDATE users SET profile_photo_file_id = ?"
			   " WHERE id = ?",
			   { (int64_t)file_id, (int64_t)user_id });
	});
}

void DB::syncUsernames(mysql::Transaction &tx, const models::User &u)
{
	struct Entry {
		std::string	kind;         /* "active" or "disabled". */
		int		position = 0;
		bool		collectible = false;
	};

	/*
	 * Load the usernames this user currently owns, so the new set can
	 * be diffed against them to record the individual changes.
	 */
	auto old_rows = tx.query(
		"SELECT username, kind, position, is_collectible FROM user_usernames"
		" WHERE user_id = ?",
		{ (int64_t)u.id });

	std::unordered_map<std::string, Entry> old_map;
	for (auto &r : old_rows) {
		if (!r[0].has_value())
			continue;
		old_map.emplace(*r[0], Entry{
			r[1].value_or(""),
			r[2].has_value() ? std::stoi(*r[2]) : 0,
			r[3].has_value() && *r[3] != "0",
		});
	}

	/*
	 * Build the new set, ONE entry per username. A username is active XOR
	 * disabled (its kind); collectible is orthogonal (a username can be
	 * active AND purchased at fragment.com), so it is a flag, not a kind.
	 * Recording it as a separate "collectible" row is what made the diff
	 * flap: the same name appeared as both active and collectible, and the
	 * unique-key upsert could keep only one, so every resync saw a kind
	 * change. A collectible username that is neither active nor disabled
	 * (owned but unassigned) is recorded as disabled.
	 */
	std::vector<std::pair<std::string, Entry>> new_list;
	std::unordered_map<std::string, size_t> idx;
	auto add = [&](const std::string &name, const char *kind, int pos) {
		idx[name] = new_list.size();
		new_list.push_back({ name, Entry{ kind, pos, false } });
	};
	for (size_t i = 0; i < u.active_usernames.size(); i++)
		add(u.active_usernames[i], "active", (int)i);
	for (size_t i = 0; i < u.disabled_usernames.size(); i++)
		add(u.disabled_usernames[i], "disabled", (int)i);
	for (size_t i = 0; i < u.collectible_usernames.size(); i++) {
		const std::string &name = u.collectible_usernames[i];
		auto it = idx.find(name);
		if (it != idx.end())
			new_list[it->second].second.collectible = true;
		else {
			add(name, "disabled", (int)i);
			new_list.back().second.collectible = true;
		}
	}

	static const char *ev =
		"INSERT INTO user_hist_usernames_events"
		" (user_id, username, action, kind, position, is_collectible)"
		" VALUES (?, ?, ?, ?, ?, ?)";

	/* Additions, status changes, collectible changes and reorders. */
	for (auto &n : new_list) {
		const std::string &uname = n.first;
		const Entry &ne = n.second;
		int64_t coll = ne.collectible ? 1 : 0;
		auto it = old_map.find(uname);
		if (it == old_map.end()) {
			tx.execute(ev, { (int64_t)u.id, uname,
					 std::string("added"), ne.kind,
					 (int64_t)ne.position, coll });
		} else if (it->second.kind != ne.kind) {
			tx.execute(ev, { (int64_t)u.id, uname,
					 std::string("kind_changed"), ne.kind,
					 (int64_t)ne.position, coll });
		} else if (it->second.collectible != ne.collectible) {
			tx.execute(ev, { (int64_t)u.id, uname,
					 std::string("collectible_changed"), ne.kind,
					 (int64_t)ne.position, coll });
		} else if (it->second.position != ne.position) {
			tx.execute(ev, { (int64_t)u.id, uname,
					 std::string("reordered"), ne.kind,
					 (int64_t)ne.position, coll });
		}
	}

	/* Removals: usernames the user no longer owns are released. */
	for (auto &o : old_map) {
		if (idx.find(o.first) != idx.end())
			continue;
		tx.execute(ev, { (int64_t)u.id, o.first,
				 std::string("removed"), std::monostate{},
				 std::monostate{}, std::monostate{} });
		tx.execute("UPDATE user_usernames SET user_id = NULL"
			   " WHERE user_id = ? AND username = ?",
			   { (int64_t)u.id, o.first });
	}

	/*
	 * Upsert the current usernames. The UNIQUE key on username lets a
	 * single statement claim a new username, transfer ownership of an
	 * existing one, and update its kind, position and collectible flag.
	 */
	static const char *ins =
		"INSERT INTO user_usernames"
		" (user_id, username, kind, position, is_collectible)"
		" VALUES (?, ?, ?, ?, ?) AS new ON DUPLICATE KEY UPDATE"
		" user_id = new.user_id, kind = new.kind,"
		" position = new.position, is_collectible = new.is_collectible";
	for (auto &n : new_list) {
		tx.execute(ins, { (int64_t)u.id, n.first, n.second.kind,
				  (int64_t)n.second.position,
				  (int64_t)(n.second.collectible ? 1 : 0) });
	}
}

void DB::trackProfilePhotoChange(mysql::Transaction &tx,
				 int64_t user_id, uint64_t file_id)
{
	auto rows = tx.query(
		"SELECT profile_photo_file_id FROM users WHERE id = ?",
		{ user_id });
	if (rows.empty())
		return;

	auto &val = rows[0][0];
	if (!val.has_value()) {
		/* First profile photo — record it. */
		tx.insert("INSERT INTO user_hist_profile_photo"
			  " (user_id, file_id) VALUES (?, ?)",
			  { user_id, (int64_t)file_id });
		return;
	}

	uint64_t old_id = std::stoull(*val);
	if (old_id == file_id)
		return;

	tx.insert("INSERT INTO user_hist_profile_photo"
		  " (user_id, file_id) VALUES (?, ?)",
		  { user_id, (int64_t)old_id });
}

void DB::upsertUserFullInfo(const models::UserFullInfo &fi)
{
	db_.transaction([&](mysql::Transaction &tx) {
		/*
		 * The user row is created from the user object (upsertUser)
		 * before full info is fetched. If it is somehow not present
		 * yet, skip rather than create a partial row.
		 */
		auto exists = tx.query("SELECT 1 FROM users WHERE id = ?",
				       { (int64_t)fi.user_id });
		if (exists.empty())
			return;

		/*
		 * Snapshot the bio as observed. This is the first point at which
		 * the bio is known (the users row was created without it, from
		 * the plain user object), so recording it here captures the
		 * initial bio and every later change, and never an empty row.
		 */
		recordTextHistory(tx, "user_hist_bio", "user_id", "bio",
				  fi.user_id, fi.bio);

		mysql::Param bday = std::monostate{};
		if (fi.birthday_day.has_value())
			bday = (int64_t)*fi.birthday_day;
		mysql::Param bmon = std::monostate{};
		if (fi.birthday_month.has_value())
			bmon = (int64_t)*fi.birthday_month;
		mysql::Param byear = std::monostate{};
		if (fi.birthday_year.has_value())
			byear = (int64_t)*fi.birthday_year;

		/* Birthday stays on the users row; bio and personal_chat_id go
		 * to user_extra_info. */
		tx.execute(
			"UPDATE users SET birthday_day = ?, birthday_month = ?,"
			" birthday_year = ? WHERE id = ?",
			{ bday, bmon, byear, (int64_t)fi.user_id });

		upsertUserExtraFromFullInfo(tx, fi);
	});
}

} /* namespace tgloggerd */
