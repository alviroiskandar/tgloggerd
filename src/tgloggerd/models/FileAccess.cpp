// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include <tgloggerd/DB.hpp>

#include <string>
#include <vector>
#include <cstdint>

namespace tgloggerd {

std::optional<FileStatus> DB::getFileStatus(uint64_t files_id)
{
	auto rows = db_.query(
		"SELECT id, file_type, file_ext, file_size, on_disk, alias_of,"
		" tg_unique_id IS NOT NULL, UNIX_TIMESTAMP(accessed_at),"
		" tg_file_id"
		" FROM telegram_files WHERE id = ?",
		{ files_id });
	if (rows.empty())
		return std::nullopt;

	const auto &r = rows[0];
	auto num = [](const std::optional<std::string> &v) -> uint64_t {
		return v.has_value() ? (uint64_t)std::stoull(*v) : 0;
	};
	FileStatus st;
	st.id            = num(r[0]);
	st.file_type     = r[1].value_or("unknown");
	st.ext           = r[2].value_or("");
	st.file_size     = num(r[3]);
	st.on_disk       = r[4].has_value() && *r[4] == "1";
	if (r[5].has_value())
		st.alias_of = num(r[5]);
	st.has_unique_id = r[6].has_value() && *r[6] == "1";
	st.accessed_at   = (int64_t)num(r[7]);
	st.generated     = isGeneratedFileId(r[8].value_or(""));
	return st;
}

uint64_t DB::touchFiles(const std::vector<uint64_t> &files_ids)
{
	if (files_ids.empty())
		return 0;

	/*
	 * One statement for the batch. Rows touched within the last hour
	 * are skipped, so a file viewed over and over costs no write; the
	 * eviction order cannot tell an hour apart anyway. Setting
	 * updated_at to itself stops ON UPDATE CURRENT_TIMESTAMP, which
	 * would otherwise make every view look like a change to the row.
	 */
	std::string sql =
		"UPDATE telegram_files SET accessed_at = NOW(),"
		" updated_at = updated_at"
		" WHERE accessed_at < NOW() - INTERVAL 1 HOUR AND id IN (";
	std::vector<mysql::Param> params;
	params.reserve(files_ids.size());
	for (size_t i = 0; i < files_ids.size(); i++) {
		sql += i ? ",?" : "?";
		params.emplace_back(files_ids[i]);
	}
	sql += ")";
	return db_.execute(sql, params);
}

} /* namespace tgloggerd */
