// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <tgloggerd/DB.hpp>

#include <string>
#include <unordered_map>
#include <cstdint>

namespace tgloggerd {

uint64_t DB::upsertFile(const models::File &f)
{
	/*
	 * De-duplicate by content: the SHA-256 is stored as BINARY(32), so
	 * bind the hex digest and let the server decode it with UNHEX().
	 *
	 * A single atomic INSERT ... ON DUPLICATE KEY UPDATE, rather than a
	 * check-then-insert, so concurrent file-pool workers storing the SAME
	 * file (identical content, or the same message reprocessed by real time
	 * and the backfiller) do not race: the loser takes the ON DUPLICATE KEY
	 * branch instead of failing on a unique key (sha256 or tg_file_id). The
	 * `id = LAST_INSERT_ID(id)` trick makes tx.insert() return the existing
	 * row's id on a duplicate, and the new id on a fresh insert.
	 */
	mysql::Param ext = std::monostate{};
	if (f.file_ext.has_value())
		ext = *f.file_ext;

	uint64_t id = 0;
	db_.transaction([&](mysql::Transaction &tx) {
		id = tx.insert(
			"INSERT INTO telegram_files (tg_file_id, file_type, file_size,"
			" sha256, file_ext, orig_file_name, on_disk)"
			" VALUES (?, ?, ?, UNHEX(?), ?, ?, ?) AS new"
			" ON DUPLICATE KEY UPDATE"
			" id = LAST_INSERT_ID(id),"
			" hit_count = hit_count + 1,"
			" orig_file_name = IF(telegram_files.orig_file_name = '',"
			" new.orig_file_name, telegram_files.orig_file_name),"
			/* Once stored, stay stored; a metadata-only re-store
			 * must not clear a copy that is already on disk. */
			" on_disk = telegram_files.on_disk OR new.on_disk",
			{
				f.tg_file_id,
				f.file_type,
				(int64_t)f.file_size,
				f.sha256_hex,
				ext,
				f.orig_file_name,
				(int64_t)(f.on_disk ? 1 : 0),
			});
	});
	return id;
}

std::unordered_map<std::string, uint64_t> DB::loadFileIndex(void)
{
	auto rows = db_.query("SELECT tg_file_id, id FROM telegram_files");

	std::unordered_map<std::string, uint64_t> out;
	out.reserve(rows.size());
	for (const auto &r : rows) {
		if (!r[0].has_value() || !r[1].has_value())
			continue;
		out.emplace(*r[0], (uint64_t)std::stoull(*r[1]));
	}
	return out;
}

} /* namespace tgloggerd */
