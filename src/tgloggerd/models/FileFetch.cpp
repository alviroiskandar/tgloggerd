// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include <tgloggerd/DB.hpp>

#include <string>
#include <vector>
#include <cstdint>

namespace tgloggerd {

std::optional<FetchRow> DB::getFileForFetch(uint64_t files_id)
{
	auto rows = db_.query(
		"SELECT id, tg_file_id, tg_unique_id, file_type, file_size,"
		" LOWER(HEX(sha256)), file_ext, orig_file_name, on_disk,"
		" alias_of"
		" FROM telegram_files WHERE id = ?",
		{ files_id });
	if (rows.empty())
		return std::nullopt;

	const auto &r = rows[0];
	auto num = [](const std::optional<std::string> &v) -> uint64_t {
		return v.has_value() ? (uint64_t)std::stoull(*v) : 0;
	};
	FetchRow f;
	f.id             = num(r[0]);
	f.tg_file_id     = r[1].value_or("");
	f.tg_unique_id   = r[2].value_or("");
	f.file_type      = r[3].value_or("unknown");
	f.file_size      = num(r[4]);
	f.sha256_hex     = r[5].value_or("");
	f.ext            = r[6].value_or("");
	f.orig_file_name = r[7].value_or("");
	f.on_disk        = r[8].has_value() && *r[8] == "1";
	if (r[9].has_value())
		f.alias_of = num(r[9]);
	return f;
}

std::vector<models::FileSource> DB::listFileSources(uint64_t files_id,
						    size_t limit)
{
	using Kind = models::FileSource::Kind;
	if (limit == 0)
		return {};

	/*
	 * Each branch walks the file_id (or photo_file_id) index backwards
	 * from the newest row and stops at @limit, so a sticker sent a
	 * million times costs no more than one sent once. Newest by surrogate
	 * id, which follows insertion; close enough to message order, and
	 * any live reference will do. The message branches name their index:
	 * left to itself the optimizer may take idx_*_deleted_at for
	 * "deleted_at IS NULL", which matches nearly every row, and walk the
	 * whole table backwards looking for the file.
	 */
	const std::string n = std::to_string(limit);
	/*
	 * A medium of a rich message is found again by its unique id, not
	 * its position, which an edit can change; one without a unique id
	 * falls back to its position.
	 */
	const std::string rich =
		"mf.role = 'rich' AND f.tg_unique_id IS NOT NULL";
	auto rows = db_.query(
		"(SELECT 0, chat_id, message_id, NULL, NULL FROM telegram_group_messages"
		"  FORCE INDEX (fk_group_messages_file)"
		"  WHERE file_id = ? AND deleted_at IS NULL"
		"  ORDER BY id DESC LIMIT " + n + ")"
		" UNION ALL "
		"(SELECT 1, chat_id, message_id, NULL, NULL FROM telegram_private_messages"
		"  FORCE INDEX (fk_private_messages_file)"
		"  WHERE file_id = ? AND deleted_at IS NULL"
		"  ORDER BY id DESC LIMIT " + n + ")"
		" UNION ALL "
		"(SELECT 2, m.chat_id, m.message_id, NULL, NULL FROM telegram_link_previews lp"
		"  JOIN telegram_group_messages m ON m.id = lp.group_message_id"
		"  WHERE lp.photo_file_id = ? AND m.deleted_at IS NULL"
		"  ORDER BY lp.id DESC LIMIT " + n + ")"
		" UNION ALL "
		"(SELECT 3, m.chat_id, m.message_id, NULL, NULL FROM telegram_link_previews lp"
		"  JOIN telegram_private_messages m ON m.id = lp.private_message_id"
		"  WHERE lp.photo_file_id = ? AND m.deleted_at IS NULL"
		"  ORDER BY lp.id DESC LIMIT " + n + ")"
		" UNION ALL "
		"(SELECT IF(" + rich + ", 10, 6), m.chat_id, m.message_id,"
		"  IF(" + rich + ", f.tg_unique_id, mf.role), mf.position"
		"  FROM telegram_message_files mf"
		"  JOIN telegram_group_messages m ON m.id = mf.group_message_id"
		"  LEFT JOIN telegram_files f ON f.id = mf.file_id"
		"  WHERE mf.file_id = ? AND m.deleted_at IS NULL"
		"  ORDER BY mf.id DESC LIMIT " + n + ")"
		" UNION ALL "
		"(SELECT IF(" + rich + ", 10, 7), m.chat_id, m.message_id,"
		"  IF(" + rich + ", f.tg_unique_id, mf.role), mf.position"
		"  FROM telegram_message_files mf"
		"  JOIN telegram_private_messages m ON m.id = mf.private_message_id"
		"  LEFT JOIN telegram_files f ON f.id = mf.file_id"
		"  WHERE mf.file_id = ? AND m.deleted_at IS NULL"
		"  ORDER BY mf.id DESC LIMIT " + n + ")"
		" UNION ALL "
		"(SELECT 4, id, 0, NULL, NULL FROM telegram_groups WHERE photo_file_id = ?"
		"  LIMIT " + n + ")"
		" UNION ALL "
		"(SELECT 5, id, 0, NULL, NULL FROM telegram_users"
		"  WHERE profile_photo_file_id = ? LIMIT " + n + ")"
		/*
		 * A photo that is no longer current is found in the user's
		 * or the group's photo list on Telegram, by its unique id.
		 */
		" UNION ALL "
		"(SELECT 8, h.user_id, 0, f.tg_unique_id, NULL"
		"  FROM telegram_user_hist_profile_photo h"
		"  JOIN telegram_files f ON f.id = h.file_id"
		"  WHERE h.file_id = ? AND f.tg_unique_id IS NOT NULL"
		"  LIMIT " + n + ")"
		" UNION ALL "
		"(SELECT 9, h.group_id, 0, f.tg_unique_id, NULL"
		"  FROM telegram_group_hist_photo h"
		"  JOIN telegram_files f ON f.id = h.file_id"
		"  WHERE h.file_id = ? AND f.tg_unique_id IS NOT NULL"
		"  LIMIT " + n + ")"
		/* An animated photo's video, found the same way. */
		" UNION ALL "
		"(SELECT 8, h.user_id, 0, f.tg_unique_id, NULL"
		"  FROM telegram_user_hist_profile_photo h"
		"  JOIN telegram_files f ON f.id = h.video_file_id"
		"  WHERE h.video_file_id = ? AND f.tg_unique_id IS NOT NULL"
		"  LIMIT " + n + ")"
		" UNION ALL "
		"(SELECT 9, h.group_id, 0, f.tg_unique_id, NULL"
		"  FROM telegram_group_hist_photo h"
		"  JOIN telegram_files f ON f.id = h.video_file_id"
		"  WHERE h.video_file_id = ? AND f.tg_unique_id IS NOT NULL"
		"  LIMIT " + n + ")"
		/*
		 * A custom emoji's sticker is found by asking for the custom
		 * emoji again, and a message effect's file by asking for the
		 * effect and picking the file by its unique id. Every file
		 * column of both tables has an index (its foreign key's); the
		 * effects table holds one row per effect Telegram offers, a
		 * few dozen, so the OR over its four columns costs nothing.
		 */
		" UNION ALL "
		"(SELECT 11, custom_emoji_id, 0, NULL, NULL FROM telegram_custom_emoji"
		"  WHERE file_id = ? LIMIT " + n + ")"
		" UNION ALL "
		"(SELECT 12, e.effect_id, 0, f.tg_unique_id, NULL"
		"  FROM telegram_message_effects e"
		"  JOIN telegram_files f ON f.id = ?"
		"  WHERE f.tg_unique_id IS NOT NULL AND"
		"   (e.static_icon_file_id = f.id OR e.select_animation_file_id = f.id"
		"    OR e.effect_animation_file_id = f.id OR e.sticker_file_id = f.id)"
		"  LIMIT " + n + ")"
		/*
		 * A community's photo is found by asking for its full info
		 * again and picking the photo size by its unique id
		 * (idx_community_photo).
		 */
		" UNION ALL "
		"(SELECT 13, c.id, 0, f.tg_unique_id, NULL"
		"  FROM telegram_communities c"
		"  JOIN telegram_files f ON f.id = c.photo_file_id"
		"  WHERE c.photo_file_id = ? AND c.have_access = 1"
		"   AND f.tg_unique_id IS NOT NULL"
		"  LIMIT " + n + ")",
		{ files_id, files_id, files_id, files_id, files_id, files_id,
		  files_id, files_id, files_id, files_id, files_id, files_id,
		  files_id, files_id, files_id });

	std::vector<models::FileSource> out;
	for (const auto &r : rows) {
		if (out.size() >= limit)
			break;
		if (!r[0].has_value() || !r[1].has_value())
			continue;
		models::FileSource s;
		switch (std::stoi(*r[0])) {
		case 0:
		case 1:
			s.kind = Kind::Message;
			break;
		case 2:
		case 3:
			s.kind = Kind::LinkPreview;
			break;
		case 4:
			s.kind = Kind::GroupPhoto;
			break;
		case 8:
			s.kind = Kind::UserPhotoHistory;
			s.unique_id = r[3].value_or("");
			break;
		case 9:
			s.kind = Kind::GroupPhotoHistory;
			s.unique_id = r[3].value_or("");
			break;
		case 10:
			s.kind = Kind::RichMedia;
			s.unique_id = r[3].value_or("");
			break;
		case 11:
			s.kind = Kind::CustomEmoji;
			s.object_id = std::stoll(*r[1]);
			break;
		case 12:
			s.kind = Kind::MessageEffect;
			s.object_id = std::stoll(*r[1]);
			s.unique_id = r[3].value_or("");
			break;
		case 13:
			s.kind = Kind::Community;
			s.object_id = std::stoll(*r[1]);
			s.unique_id = r[3].value_or("");
			break;
		case 6:
		case 7:
			s.kind = Kind::MessageExtra;
			s.role = r[3].value_or("");
			s.position = r[4].has_value() ? std::stoi(*r[4]) : 0;
			break;
		default:
			s.kind = Kind::UserPhoto;
			break;
		}
		s.chat_id = std::stoll(*r[1]);
		s.message_id = r[2].has_value() ? std::stoll(*r[2]) : 0;
		out.push_back(s);
	}
	return out;
}

} /* namespace tgloggerd */
