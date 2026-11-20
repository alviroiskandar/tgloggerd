// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include <tgloggerd/DB.hpp>

#include <string>
#include <stdexcept>
#include <vector>
#include <cstdint>

/*
 * The StorageJanitor's queries (docs/design/storage-cache.md section 9).
 * They all walk idx_files_evict (on_disk, accessed_at): InnoDB appends the
 * primary key to it, so ORDER BY accessed_at, id under on_disk = 1 is an
 * index range read, not a sort.
 */

namespace tgloggerd {

namespace {

uint64_t num(const std::optional<std::string> &v)
{
	return v.has_value() ? (uint64_t)std::stoull(*v) : 0;
}

} /* namespace */

StoreTotals DB::storedTotals(const std::string &before)
{
	std::string sql =
		"SELECT COUNT(1), COALESCE(SUM(file_size), 0)"
		" FROM telegram_files WHERE on_disk = 1";
	std::vector<mysql::Param> params;
	if (!before.empty()) {
		sql += " AND accessed_at < ?";
		params.emplace_back(before);
	}
	auto rows = db_.query(sql, params);
	StoreTotals t;
	if (!rows.empty()) {
		t.files = num(rows[0][0]);
		t.bytes = num(rows[0][1]);
	}
	return t;
}

std::string DB::retentionCutoff(uint32_t days)
{
	auto rows = db_.query("SELECT NOW() - INTERVAL ? DAY",
			      { (uint64_t)days });
	if (rows.empty() || !rows[0][0].has_value())
		throw std::runtime_error("retention cutoff: no result");
	return *rows[0][0];
}

std::vector<EvictCandidate>
DB::listEvictCandidates(const std::string &before, const std::string &from,
			const std::string &after_at, uint64_t after_id,
			size_t limit)
{
	std::string sql =
		"SELECT id, LOWER(HEX(sha256)), file_ext, file_size, accessed_at,"
		" tg_file_id LIKE 'generated:%'"
		" FROM telegram_files WHERE on_disk = 1";
	std::vector<mysql::Param> params;
	if (!before.empty()) {
		sql += " AND accessed_at < ?";
		params.emplace_back(before);
	}
	if (!from.empty()) {
		sql += " AND accessed_at >= ?";
		params.emplace_back(from);
	}
	if (!after_at.empty()) {
		sql += " AND (accessed_at > ? OR (accessed_at = ? AND id > ?))";
		params.emplace_back(after_at);
		params.emplace_back(after_at);
		params.emplace_back(after_id);
	}
	sql += " ORDER BY accessed_at, id LIMIT ?";
	params.emplace_back((uint64_t)limit);

	auto rows = db_.query(sql, params);
	std::vector<EvictCandidate> out;
	out.reserve(rows.size());
	for (const auto &r : rows) {
		EvictCandidate c;
		c.id          = num(r[0]);
		c.sha256      = r[1].value_or("");
		c.ext         = r[2].value_or("");
		c.file_size   = num(r[3]);
		c.accessed_at = r[4].value_or("");
		c.generated   = r[5].value_or("0") == "1";
		out.push_back(std::move(c));
	}
	return out;
}

bool DB::evictFile(uint64_t files_id, const std::string &accessed_at)
{
	/*
	 * The guard: an access (touchFiles) or a re-store (upsertFile) since
	 * the row was listed moved accessed_at, so the row no longer matches
	 * and the file stays. updated_at = updated_at keeps ON UPDATE
	 * CURRENT_TIMESTAMP off: an eviction is not a change to the file.
	 */
	return db_.execute(
		"UPDATE telegram_files SET on_disk = 0, updated_at = updated_at"
		" WHERE id = ? AND on_disk = 1 AND accessed_at = ?",
		{ files_id, accessed_at }) == 1;
}

} /* namespace tgloggerd */
