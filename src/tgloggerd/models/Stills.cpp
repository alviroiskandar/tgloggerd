// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include <tgloggerd/DB.hpp>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace tgloggerd {

namespace {

/* The kinds a still is generated for; a sticker only when it is .webm. */
constexpr const char *kKinds =
	"('video','animation','video_note','sticker')";

/*
 * The generated-still selection of one message table @t, whose rows
 * telegram_message_files points at through @key: FROM and WHERE, the
 * table aliased m, its main file v. @index is the index the message rows
 * come off: the (content_type, date) one for the whole pass, the file_id
 * one for a single video (then @by_file adds the file_id condition). The
 * video row is read by its primary key, the message files by their
 * unique key (key, role, position) and their file rows by primary key.
 *
 * A message is a candidate while it is live, is one of the kinds above
 * (a sticker when its file is .webm), its main file's bytes are in the
 * store, and it has no cover and no thumbnail, except a thumbnail that
 * is a generated still evicted from the store: that one is generated
 * again. A thumbnail row whose file row is gone (file_id NULL) does not
 * count either.
 */
std::string still_from(const char *t, const char *key, bool by_file)
{
	const std::string tt = t;
	const std::string short_name = tt.substr(sizeof("telegram_") - 1);
	const std::string index = by_file
		? "fk_" + short_name + "_file"
		: "idx_" + short_name + "_ctype_date";
	return " FROM " + tt + " m FORCE INDEX (" + index + ")"
	       " JOIN telegram_files v ON v.id = m.file_id"
	       " WHERE m.content_type IN " + std::string(kKinds) +
	       (by_file ? " AND m.file_id = ?" : "") +
	       " AND m.deleted_at IS NULL"
	       " AND v.on_disk = 1 AND v.sha256 IS NOT NULL"
	       " AND (m.content_type <> 'sticker' OR v.file_ext = 'webm')"
	       " AND NOT EXISTS (SELECT 1 FROM telegram_message_files x"
	       " LEFT JOIN telegram_files t ON t.id = x.file_id"
	       " WHERE x." + std::string(key) + " = m.id"
	       " AND x.role IN ('thumbnail','cover')"
	       " AND NOT (x.role = 'thumbnail' AND t.on_disk = 0"
	       " AND t.tg_file_id LIKE 'generated:%'))";
}

const char *kStillColumns =
	"SELECT m.file_id, m.content_type,"
	" JSON_UNQUOTE(JSON_EXTRACT(m.content_extra, '$.duration')),"
	" LOWER(HEX(v.sha256)), v.file_ext";

StillCandidate candidate_of(const mysql::Row &r)
{
	StillCandidate c;
	c.video_file_id = std::stoull(*r[0]);
	c.kind = r[1].value_or("");
	c.duration = r[2].has_value() ? std::strtod(r[2]->c_str(), nullptr)
				      : 0.0;
	if (!(c.duration > 0))
		c.duration = 0;
	c.sha256_hex = r[3].value_or("");
	c.ext = r[4].value_or("");
	return c;
}

} /* namespace */

std::string DB::stillCandidatesSql(void)
{
	return std::string(kStillColumns) +
	       still_from("telegram_group_messages", "group_message_id",
			  false) +
	       " UNION ALL " + kStillColumns +
	       still_from("telegram_private_messages", "private_message_id",
			  false);
}

std::vector<StillCandidate> DB::listStillCandidates(void)
{
	auto rows = db_.query(stillCandidatesSql());

	/* One per video, however many messages carry it; the longest
	 * duration any of them states. */
	std::vector<StillCandidate> out;
	std::unordered_map<uint64_t, size_t> at;
	for (const auto &r : rows) {
		if (!r[0].has_value())
			continue;
		StillCandidate c = candidate_of(r);
		auto it = at.find(c.video_file_id);
		if (it == at.end()) {
			at.emplace(c.video_file_id, out.size());
			out.push_back(std::move(c));
		} else if (c.duration > out[it->second].duration) {
			out[it->second].duration = c.duration;
		}
	}
	/* Newest videos first: the ones most likely to be looked at. */
	std::sort(out.begin(), out.end(),
		  [](const StillCandidate &a, const StillCandidate &b) {
			  return a.video_file_id > b.video_file_id;
		  });
	return out;
}

std::vector<StillCount> DB::countStillCandidates(void)
{
	auto rows = db_.query(
		"SELECT 'group', m.content_type, COUNT(*),"
		" COUNT(DISTINCT m.file_id)" +
		still_from("telegram_group_messages", "group_message_id",
			   false) +
		" GROUP BY m.content_type"
		" UNION ALL SELECT 'private', m.content_type, COUNT(*),"
		" COUNT(DISTINCT m.file_id)" +
		still_from("telegram_private_messages", "private_message_id",
			   false) +
		" GROUP BY m.content_type");

	std::vector<StillCount> out;
	for (const auto &r : rows)
		if (r[0].has_value() && r[1].has_value() && r[2].has_value() &&
		    r[3].has_value())
			out.push_back(StillCount{ *r[0], *r[1],
						  std::stoull(*r[2]),
						  std::stoull(*r[3]) });
	return out;
}

std::string DB::stillCandidateSql(void)
{
	return "(" + std::string(kStillColumns) +
	       still_from("telegram_group_messages", "group_message_id",
			  true) +
	       " LIMIT 1) UNION ALL (" + kStillColumns +
	       still_from("telegram_private_messages", "private_message_id",
			  true) +
	       " LIMIT 1)";
}

std::optional<StillCandidate> DB::stillCandidate(uint64_t video_file_id)
{
	auto rows = db_.query(stillCandidateSql(),
			      { video_file_id, video_file_id });
	for (const auto &r : rows)
		if (r[0].has_value())
			return candidate_of(r);
	return std::nullopt;
}

std::optional<uint64_t> DB::findGeneratedStill(uint64_t video_file_id)
{
	auto rows = db_.query(
		"SELECT id FROM telegram_files"
		" WHERE generated_from_file_id = ? AND on_disk = 1"
		" AND tg_file_id LIKE 'generated:%'"
		" ORDER BY id DESC LIMIT 1",
		{ video_file_id });
	if (rows.empty() || !rows[0][0].has_value())
		return std::nullopt;
	return std::stoull(*rows[0][0]);
}

uint64_t DB::upsertGeneratedStill(const std::string &sha256_hex,
				  const std::string &ext, uint64_t size,
				  bool on_disk, uint64_t video_file_id)
{
	/*
	 * Keyed by its bytes like any file. The same bytes met again (the
	 * same still taken again after an eviction, or the same picture from
	 * another video) are the same row: it is stored again and keeps the
	 * video it was first taken from. Bytes that happen to equal a file
	 * from Telegram are that row, which is not marked generated.
	 */
	uint64_t id = 0;
	db_.transaction([&](mysql::Transaction &tx) {
		id = tx.insert(
			"INSERT INTO telegram_files (tg_file_id, tg_unique_id,"
			" file_type, file_size, sha256, file_ext, on_disk,"
			" generated_from_file_id)"
			" VALUES (?, NULL, 'thumbnail', ?, UNHEX(?), ?, ?, ?) AS new"
			" ON DUPLICATE KEY UPDATE"
			" id = LAST_INSERT_ID(id),"
			" on_disk = telegram_files.on_disk OR new.on_disk,"
			" generated_from_file_id = IF(telegram_files.tg_file_id"
			" LIKE 'generated:%', COALESCE("
			"telegram_files.generated_from_file_id,"
			" new.generated_from_file_id),"
			" telegram_files.generated_from_file_id),"
			" accessed_at = NOW()",
			{ std::string(kGeneratedFilePrefix) + sha256_hex,
			  (int64_t)size, sha256_hex, ext,
			  (int64_t)(on_disk ? 1 : 0), (int64_t)video_file_id });
	});
	return id;
}

/*
 * Link still @still_id as role thumbnail, position 0, of every live
 * message of table @t (key @key) whose main file is video @video_file_id.
 * An existing thumbnail is replaced only when it is a generated still
 * too, or its file row is gone; one from Telegram stays, and a Telegram
 * thumbnail stored later replaces this one (setMessageExtraFile).
 */
static uint64_t link_still(mysql::Database &db, const char *t, const char *key,
			   uint64_t video_file_id, uint64_t still_id)
{
	const std::string tt = t;
	const std::string index =
		"fk_" + tt.substr(sizeof("telegram_") - 1) + "_file";
	return db.execute(
		"INSERT INTO telegram_message_files (" + std::string(key) +
		", role, position, file_id)"
		" SELECT * FROM (SELECT m.id, 'thumbnail' AS role, 0 AS position,"
		" ? AS file_id FROM " + tt + " m FORCE INDEX (" + index + ")"
		" WHERE m.file_id = ? AND m.deleted_at IS NULL"
		" AND m.content_type IN " + kKinds + ") AS new"
		" ON DUPLICATE KEY UPDATE file_id = IF("
		"telegram_message_files.file_id IS NULL OR (SELECT f.tg_file_id"
		" FROM telegram_files f"
		" WHERE f.id = telegram_message_files.file_id)"
		" LIKE 'generated:%', new.file_id,"
		" telegram_message_files.file_id)",
		{ (int64_t)still_id, (int64_t)video_file_id });
}

uint64_t DB::linkGeneratedStill(uint64_t video_file_id, uint64_t still_id)
{
	return link_still(db_, "telegram_group_messages", "group_message_id",
			  video_file_id, still_id) +
	       link_still(db_, "telegram_private_messages",
			  "private_message_id", video_file_id, still_id);
}

std::vector<std::pair<int64_t, int64_t>>
DB::listStillMessages(uint64_t still_id)
{
	auto rows = db_.query(
		"SELECT m.chat_id, m.message_id FROM telegram_message_files x"
		" JOIN telegram_group_messages m ON m.id = x.group_message_id"
		" WHERE x.file_id = ? AND x.role = 'thumbnail'"
		" UNION ALL SELECT m.chat_id, m.message_id"
		" FROM telegram_message_files x"
		" JOIN telegram_private_messages m ON m.id = x.private_message_id"
		" WHERE x.file_id = ? AND x.role = 'thumbnail'",
		{ still_id, still_id });
	std::vector<std::pair<int64_t, int64_t>> out;
	for (const auto &r : rows)
		if (r[0].has_value() && r[1].has_value())
			out.emplace_back(std::stoll(*r[0]), std::stoll(*r[1]));
	return out;
}

} /* namespace tgloggerd */
