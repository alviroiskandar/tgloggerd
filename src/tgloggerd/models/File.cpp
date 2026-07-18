// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <tgloggerd/DB.hpp>

#include <string>

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
			"INSERT INTO files (tg_file_id, file_type, file_size,"
			" sha256, file_ext, orig_file_name)"
			" VALUES (?, ?, ?, UNHEX(?), ?, ?) AS new"
			" ON DUPLICATE KEY UPDATE"
			" id = LAST_INSERT_ID(id),"
			" hit_count = hit_count + 1,"
			" orig_file_name = IF(files.orig_file_name = '',"
			" new.orig_file_name, files.orig_file_name)",
			{
				f.tg_file_id,
				f.file_type,
				(int64_t)f.file_size,
				f.sha256_hex,
				ext,
				f.orig_file_name,
			});
	});
	return id;
}

} /* namespace tgloggerd */
