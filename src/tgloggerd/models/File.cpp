// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <tgloggerd/DB.hpp>

#include <string>
#include <optional>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace tgloggerd {

/*
 * The rows a file may already be: the one holding its bytes (sha256), the
 * one recorded under its remote id (tg_file_id) and the one recorded under
 * its unique id (tg_unique_id). Any of them may be missing, and two or all
 * three may be the same row.
 */
namespace {
struct FileCandidates {
	std::optional<uint64_t> by_sha256;
	std::optional<uint64_t> by_file_id;
	std::optional<uint64_t> by_unique_id;
	/* Candidates whose bytes were never seen (sha256 IS NULL). */
	std::vector<uint64_t> meta_only;
};

FileCandidates find_file_candidates(mysql::Transaction &tx,
				    const mysql::Param &sha256_hex,
				    const std::string &tg_file_id,
				    const mysql::Param &tg_unique_id)
{
	/* A NULL parameter matches nothing, so an unknown sha256 or unique
	 * id simply drops out of the lookup. */
	auto rows = tx.query(
		"SELECT id, sha256 = UNHEX(?), tg_file_id = ?,"
		" tg_unique_id = ?, sha256 IS NULL"
		" FROM telegram_files"
		" WHERE sha256 = UNHEX(?) OR tg_file_id = ? OR tg_unique_id = ?",
		{ sha256_hex, tg_file_id, tg_unique_id,
		  sha256_hex, tg_file_id, tg_unique_id });

	auto is_true = [](const std::optional<std::string> &v) {
		return v.has_value() && *v == "1";
	};
	FileCandidates c;
	for (const auto &r : rows) {
		if (!r[0].has_value())
			continue;
		uint64_t id = (uint64_t)std::stoull(*r[0]);
		if (is_true(r[1]))
			c.by_sha256 = id;
		if (is_true(r[2]))
			c.by_file_id = id;
		if (is_true(r[3]))
			c.by_unique_id = id;
		if (is_true(r[4]))
			c.meta_only.push_back(id);
	}
	return c;
}
} /* namespace */

uint64_t DB::upsertFile(const models::File &f, std::string *row_ext)
{
	/*
	 * De-duplicate by content: the SHA-256 is stored as BINARY(32), so
	 * bind the hex digest and let the server decode it with UNHEX().
	 *
	 * The file may already have a row three ways, and they can be three
	 * different rows: same bytes (sha256), same remote id (tg_file_id),
	 * or same unique id (tg_unique_id) -- the last one typically a row
	 * recorded metadata-only (upsertFileMeta) that is now downloaded.
	 * A single INSERT ... ON DUPLICATE KEY UPDATE would update whichever
	 * row InnoDB meets first and then fail on another unique key while
	 * filling in sha256 or tg_unique_id. So look the candidates up first
	 * and update the right one:
	 *
	 *  - the row holding the same bytes wins (content dedup, as before);
	 *    a metadata-only candidate is pointed at it through alias_of,
	 *    so what was linked to it becomes servable too;
	 *  - else the row with the same unique id, else the one with the
	 *    same remote id, takes the bytes: sha256 and file_ext are filled
	 *    in and on_disk is raised;
	 *  - else a new row is inserted.
	 *
	 * The INSERT keeps the atomic ON DUPLICATE KEY UPDATE form, so
	 * concurrent file-pool workers storing the SAME file (identical
	 * content, or the same message reprocessed by real time and the
	 * backfiller) do not race: the loser, which saw no candidate, takes
	 * the ON DUPLICATE KEY branch instead of failing on a unique key.
	 * The `id = LAST_INSERT_ID(id)` trick makes tx.insert() return the
	 * existing row's id on a duplicate, and the new id on a fresh insert.
	 */
	mysql::Param ext = std::monostate{};
	if (f.file_ext.has_value())
		ext = *f.file_ext;
	mysql::Param uid = std::monostate{};
	if (!f.tg_unique_id.empty())
		uid = f.tg_unique_id;
	mysql::Param hex = f.sha256_hex;
	const int64_t on_disk = f.on_disk ? 1 : 0;

	uint64_t id = 0;
	db_.transaction([&](mysql::Transaction &tx) {
		FileCandidates c = find_file_candidates(tx, hex, f.tg_file_id,
							uid);
		std::optional<uint64_t> target = c.by_sha256;
		if (!target)
			target = c.by_unique_id;
		if (!target)
			target = c.by_file_id;

		if (!target) {
			id = tx.insert(
				"INSERT INTO telegram_files (tg_file_id,"
				" tg_unique_id, file_type, file_size, sha256,"
				" file_ext, orig_file_name, on_disk)"
				" VALUES (?, ?, ?, ?, UNHEX(?), ?, ?, ?) AS new"
				" ON DUPLICATE KEY UPDATE"
				" id = LAST_INSERT_ID(id),"
				" hit_count = hit_count + 1,"
				" orig_file_name = IF(telegram_files.orig_file_name = '',"
				" new.orig_file_name, telegram_files.orig_file_name),"
				/* Once stored, stay stored; a metadata-only
				 * re-store must not clear a copy that is
				 * already on disk. */
				" on_disk = telegram_files.on_disk OR new.on_disk,"
				/* A file seen again is in use: keep it from
				 * eviction. */
				" accessed_at = NOW()",
				{
					f.tg_file_id,
					uid,
					f.file_type,
					(int64_t)f.file_size,
					hex,
					ext,
					f.orig_file_name,
					on_disk,
				});
			return;
		}

		id = *target;
		/*
		 * Fill in what the target lacks. sha256 is free to take when
		 * no row holds these bytes (else that row is the target and
		 * COALESCE keeps its value); the unique id only when no other
		 * row holds it, or the UNIQUE key would reject the update.
		 */
		mysql::Param fill_uid = uid;
		if (c.by_unique_id && *c.by_unique_id != id)
			fill_uid = std::monostate{};
		tx.execute(
			"UPDATE telegram_files SET"
			" hit_count = hit_count + 1,"
			" sha256 = COALESCE(sha256, UNHEX(?)),"
			" tg_unique_id = COALESCE(tg_unique_id, ?),"
			" file_ext = COALESCE(file_ext, ?),"
			" orig_file_name = IF(orig_file_name = '', ?, orig_file_name),"
			" on_disk = on_disk OR ?,"
			" accessed_at = NOW()"
			" WHERE id = ?",
			{ hex, fill_uid, ext, f.orig_file_name, on_disk,
			  (int64_t)id });

		/* The same file recorded metadata-only under another row:
		 * point it at the row with the bytes (one hop, see
		 * migration 000032). */
		for (uint64_t other : c.meta_only) {
			if (other == id)
				continue;
			tx.execute("UPDATE telegram_files SET alias_of = ?"
				   " WHERE id = ? AND alias_of IS NULL",
				   { (int64_t)id, (int64_t)other });
		}
	});
	if (row_ext) {
		auto rows = db_.query("SELECT COALESCE(file_ext, '')"
				      " FROM telegram_files WHERE id = ?",
				      { (int64_t)id });
		*row_ext = rows.empty() ? std::string()
					: rows[0][0].value_or("");
	}
	return id;
}

models::KnownFile DB::upsertFileMeta(const models::File &f)
{
	mysql::Param ext = std::monostate{};
	if (f.file_ext.has_value())
		ext = *f.file_ext;
	mysql::Param uid = std::monostate{};
	if (!f.tg_unique_id.empty())
		uid = f.tg_unique_id;
	const mysql::Param no_sha = std::monostate{};

	models::KnownFile out;
	db_.transaction([&](mysql::Transaction &tx) {
		/*
		 * The unique id is the dedup key here (there are no bytes to
		 * hash), the remote id a fallback for rows written before it
		 * was recorded. See upsertFile for why the candidates are
		 * looked up before writing.
		 */
		FileCandidates c = find_file_candidates(tx, no_sha,
							f.tg_file_id, uid);
		std::optional<uint64_t> target = c.by_unique_id;
		if (!target)
			target = c.by_file_id;

		if (!target) {
			/* ON DUPLICATE KEY UPDATE: a concurrent writer may
			 * have recorded the same file since the lookup. */
			out.id = tx.insert(
				"INSERT INTO telegram_files (tg_file_id,"
				" tg_unique_id, file_type, file_size, sha256,"
				" file_ext, orig_file_name, on_disk)"
				" VALUES (?, ?, ?, ?, NULL, ?, ?, 0) AS new"
				" ON DUPLICATE KEY UPDATE"
				" id = LAST_INSERT_ID(id),"
				" hit_count = hit_count + 1,"
				" orig_file_name = IF(telegram_files.orig_file_name = '',"
				" new.orig_file_name, telegram_files.orig_file_name),"
				" accessed_at = NOW()",
				{
					f.tg_file_id,
					uid,
					f.file_type,
					(int64_t)f.file_size,
					ext,
					f.orig_file_name,
				});
		} else {
			out.id = *target;
			mysql::Param fill_uid = uid;
			if (c.by_unique_id && *c.by_unique_id != out.id)
				fill_uid = std::monostate{};
			tx.execute(
				"UPDATE telegram_files SET"
				" hit_count = hit_count + 1,"
				" tg_unique_id = COALESCE(tg_unique_id, ?),"
				/* The blob of a row with bytes is named
				 * <sha256>[.file_ext] (blobPath): its
				 * extension is fixed once they are stored. */
				" file_ext = IF(sha256 IS NULL,"
				" COALESCE(file_ext, ?), file_ext),"
				" orig_file_name = IF(orig_file_name = '', ?, orig_file_name),"
				/* Seen again: in use, keep it from eviction. */
				" accessed_at = NOW()"
				" WHERE id = ?",
				{ fill_uid, ext, f.orig_file_name,
				  (int64_t)out.id });
		}

		/* The row may be a stored one: tell the index which. */
		auto rows = tx.query("SELECT sha256 IS NULL FROM telegram_files"
				     " WHERE id = ?", { (int64_t)out.id });
		out.meta_only = rows.empty() || !rows[0][0].has_value() ||
				*rows[0][0] != "0";
	});
	return out;
}

std::unordered_map<std::string, models::KnownFile> DB::loadFileIndex(void)
{
	/* Follow alias_of (one hop, see migration 000032): a metadata-only
	 * row whose bytes turned out to be another row's maps to that row,
	 * so its remote id does not look metadata-only after a restart. */
	auto rows = db_.query(
		"SELECT f.tg_file_id, COALESCE(a.id, f.id),"
		" IF(a.id IS NULL, f.sha256, a.sha256) IS NULL"
		" FROM telegram_files f"
		" LEFT JOIN telegram_files a ON a.id = f.alias_of");

	std::unordered_map<std::string, models::KnownFile> out;
	out.reserve(rows.size());
	for (const auto &r : rows) {
		if (!r[0].has_value() || !r[1].has_value())
			continue;
		models::KnownFile k;
		k.id = (uint64_t)std::stoull(*r[1]);
		k.meta_only = r[2].has_value() && *r[2] == "1";
		out.emplace(*r[0], k);
	}
	return out;
}

std::unordered_map<std::string, models::KnownFile>
DB::loadUniqueIndex(void)
{
	/* Follow alias_of (one hop, see migration 000032): the leftover
	 * metadata-only row keeps the unique id, the target has the bytes. */
	auto rows = db_.query(
		"SELECT f.tg_unique_id, COALESCE(a.id, f.id),"
		" IF(a.id IS NULL, f.sha256, a.sha256) IS NULL"
		" FROM telegram_files f"
		" LEFT JOIN telegram_files a ON a.id = f.alias_of"
		" WHERE f.tg_unique_id IS NOT NULL");

	std::unordered_map<std::string, models::KnownFile> out;
	out.reserve(rows.size());
	for (const auto &r : rows) {
		if (!r[0].has_value() || !r[1].has_value())
			continue;
		models::KnownFile k;
		k.id = (uint64_t)std::stoull(*r[1]);
		k.meta_only = r[2].has_value() && *r[2] == "1";
		out.emplace(*r[0], k);
	}
	return out;
}

} /* namespace tgloggerd */
