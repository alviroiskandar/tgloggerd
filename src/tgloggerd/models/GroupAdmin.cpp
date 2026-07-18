// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <tgloggerd/DB.hpp>

#include <array>
#include <string>
#include <vector>
#include <unordered_map>

namespace tgloggerd {

namespace {

mysql::Param b(bool v)
{
	return (int64_t)(v ? 1 : 0);
}

const char *admin_status(const models::GroupAdmin &a)
{
	return a.is_owner ? "creator" : "administrator";
}

/* The 17 chatAdministratorRights flags in a fixed order shared by the
 * SQL column lists, the change comparison, and the parameter binding. */
std::array<bool, 17> admin_rights(const models::GroupAdmin &a)
{
	return {{ a.can_manage_chat, a.can_change_info, a.can_post_messages,
		  a.can_edit_messages, a.can_delete_messages, a.can_invite_users,
		  a.can_restrict_members, a.can_pin_messages, a.can_manage_topics,
		  a.can_promote_members, a.can_manage_video_chats,
		  a.can_post_stories, a.can_edit_stories, a.can_delete_stories,
		  a.can_manage_direct_messages, a.can_manage_tags,
		  a.is_anonymous }};
}

void push_rights(std::vector<mysql::Param> &p, const models::GroupAdmin &a)
{
	for (bool r : admin_rights(a))
		p.push_back(b(r));
}

} /* namespace */

/*
 * Replace the stored administrator set of a group with a freshly fetched
 * one, recording every change. Mirrors syncGroupUsernames: diff the new
 * set against the current rows, then emit added/updated/removed events.
 *
 * The caller must only invoke this with a genuinely fetched list; an empty
 * list means "the group has no admins", which would (correctly) remove all
 * stored admins, so an errored fetch must never reach here.
 */
void DB::syncGroupAdmins(const models::GroupAdminList &list)
{
	static const char *upsert_sql =
		"INSERT INTO group_admins ("
		" group_id, user_id, status, custom_title, inviter_user_id,"
		" joined_date, can_manage_chat, can_change_info, can_post_messages,"
		" can_edit_messages, can_delete_messages, can_invite_users,"
		" can_restrict_members, can_pin_messages, can_manage_topics,"
		" can_promote_members, can_manage_video_chats, can_post_stories,"
		" can_edit_stories, can_delete_stories, can_manage_direct_messages,"
		" can_manage_tags, is_anonymous"
		") VALUES ("
		" ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?"
		") AS new ON DUPLICATE KEY UPDATE"
		" status = new.status, custom_title = new.custom_title,"
		" inviter_user_id = new.inviter_user_id, joined_date = new.joined_date,"
		" can_manage_chat = new.can_manage_chat, can_change_info = new.can_change_info,"
		" can_post_messages = new.can_post_messages, can_edit_messages = new.can_edit_messages,"
		" can_delete_messages = new.can_delete_messages, can_invite_users = new.can_invite_users,"
		" can_restrict_members = new.can_restrict_members, can_pin_messages = new.can_pin_messages,"
		" can_manage_topics = new.can_manage_topics, can_promote_members = new.can_promote_members,"
		" can_manage_video_chats = new.can_manage_video_chats, can_post_stories = new.can_post_stories,"
		" can_edit_stories = new.can_edit_stories, can_delete_stories = new.can_delete_stories,"
		" can_manage_direct_messages = new.can_manage_direct_messages,"
		" can_manage_tags = new.can_manage_tags, is_anonymous = new.is_anonymous";

	static const char *hist_sql =
		"INSERT INTO group_admin_hist ("
		" group_id, user_id, action, status, custom_title,"
		" can_manage_chat, can_change_info, can_post_messages, can_edit_messages,"
		" can_delete_messages, can_invite_users, can_restrict_members, can_pin_messages,"
		" can_manage_topics, can_promote_members, can_manage_video_chats, can_post_stories,"
		" can_edit_stories, can_delete_stories, can_manage_direct_messages, can_manage_tags,"
		" is_anonymous"
		") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

	int64_t gid = list.group_id;

	db_.transaction([&](mysql::Transaction &tx) {
		/*
		 * Current stored admins. Column order: user_id, status,
		 * custom_title, then the 17 rights (indices 3..19).
		 */
		auto old_rows = tx.query(
			"SELECT user_id, status, custom_title,"
			" can_manage_chat, can_change_info, can_post_messages,"
			" can_edit_messages, can_delete_messages, can_invite_users,"
			" can_restrict_members, can_pin_messages, can_manage_topics,"
			" can_promote_members, can_manage_video_chats, can_post_stories,"
			" can_edit_stories, can_delete_stories, can_manage_direct_messages,"
			" can_manage_tags, is_anonymous"
			" FROM group_admins WHERE group_id = ?",
			{ gid });

		std::unordered_map<int64_t, const mysql::Row *> old_map;
		for (auto &r : old_rows) {
			if (r[0].has_value())
				old_map.emplace(std::stoll(*r[0]), &r);
		}

		std::unordered_map<int64_t, const models::GroupAdmin *> new_map;
		for (const auto &a : list.admins)
			new_map.emplace(a.user_id, &a);

		/* Additions and updates. */
		for (const auto &a : list.admins) {
			auto it = old_map.find(a.user_id);
			bool is_new = (it == old_map.end());
			bool changed = false;

			if (!is_new) {
				const mysql::Row &old = *it->second;
				if (old[1].value_or("") != admin_status(a) ||
				    old[2].value_or("") != a.custom_title) {
					changed = true;
				} else {
					auto nr = admin_rights(a);
					for (int i = 0; i < 17 && !changed; i++) {
						bool ob = old[3 + i].value_or("0") == "1";
						if (ob != nr[(size_t)i])
							changed = true;
					}
				}
			}

			/* reserve() with the exact final size before any
			 * push_back so the vector never reallocates; besides the
			 * micro-optimization, it stops GCC's -Wmaybe-uninitialized
			 * false positive on moving the std::string variant
			 * elements during a growth that now cannot happen. */
			std::vector<mysql::Param> up;
			up.reserve(6 + 17);
			up.push_back(gid);
			up.push_back(a.user_id);
			up.push_back(std::string(admin_status(a)));
			up.push_back(a.custom_title);
			up.push_back(a.inviter_user_id);
			up.push_back(a.joined_date);
			push_rights(up, a);
			tx.execute(upsert_sql, up);

			if (is_new || changed) {
				std::vector<mysql::Param> h;
				h.reserve(5 + 17);
				h.push_back(gid);
				h.push_back(a.user_id);
				h.push_back(std::string(is_new ? "added" : "updated"));
				h.push_back(std::string(admin_status(a)));
				h.push_back(a.custom_title);
				push_rights(h, a);
				tx.execute(hist_sql, h);
			}
		}

		/* Removals: stored admins no longer present. */
		for (auto &r : old_rows) {
			if (!r[0].has_value())
				continue;
			int64_t uid = std::stoll(*r[0]);
			if (new_map.count(uid))
				continue;

			std::vector<mysql::Param> h;
			h.reserve(5 + 17);
			h.push_back(gid);
			h.push_back(uid);
			h.push_back(std::string("removed"));
			h.push_back(r[1].value_or("administrator"));
			h.push_back(r[2].value_or(""));
			for (int i = 0; i < 17; i++)
				h.push_back((int64_t)(r[3 + i].value_or("0") == "1"));
			tx.execute(hist_sql, h);

			tx.execute("DELETE FROM group_admins"
				   " WHERE group_id = ? AND user_id = ?",
				   { gid, uid });
		}
	});
}

} /* namespace tgloggerd */
