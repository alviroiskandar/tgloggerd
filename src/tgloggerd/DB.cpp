// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <tgloggerd/DB.hpp>

#include <string>

namespace tgloggerd {

DB::DB(const mysql::Config &cfg)
	: db_(cfg)
{
}

DB::~DB(void) = default;

void DB::ping(void)
{
	db_.query("SELECT 1");
}

void DB::recordTextHistory(mysql::Transaction &tx, const char *table,
			   const char *fk_column, const char *value_column,
			   int64_t entity_id, const std::string &value)
{
	/*
	 * Never store an empty snapshot: a user bio or group description is
	 * unknown when its row is first created (it arrives later with full
	 * info), and recording the empty placeholder is what filled these
	 * tables with blank rows.
	 */
	if (value.empty())
		return;

	/* Skip if unchanged since the last snapshot, so repeated full-info
	 * refreshes of the same text do not pile up duplicates. */
	std::string sel = std::string("SELECT ") + value_column + " FROM " +
		table + " WHERE " + fk_column + " = ? ORDER BY id DESC LIMIT 1";
	auto last = tx.query(sel, { entity_id });
	if (!last.empty() && last[0][0].value_or("") == value)
		return;

	std::string ins = std::string("INSERT INTO ") + table + " (" +
		fk_column + ", " + value_column + ") VALUES (?, ?)";
	tx.insert(ins, { entity_id, value });
}

} /* namespace tgloggerd */
