// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <tgloggerd/DB.hpp>

#include <string>
#include <vector>

namespace tgloggerd {

std::vector<models::BackfillState> DB::loadBackfillState(void)
{
	auto rows = db_.query(
		"SELECT chat_id, scope, cursor_msg_id, done, priority"
		" FROM chat_backfill_state");

	std::vector<models::BackfillState> out;
	out.reserve(rows.size());
	for (const auto &r : rows) {
		if (!r[0].has_value())
			continue;
		models::BackfillState st;
		st.chat_id = std::stoll(*r[0]);
		st.scope   = r[1].value_or("group");
		if (r[2].has_value())
			st.cursor_msg_id = std::stoll(*r[2]);
		st.done = r[3].has_value() && *r[3] == "1";
		st.priority = r[4].has_value() && *r[4] == "1";
		out.push_back(std::move(st));
	}
	return out;
}

void DB::upsertBackfillState(const models::BackfillState &st)
{
	mysql::Param cursor_param = std::monostate{};
	if (st.cursor_msg_id.has_value())
		cursor_param = (int64_t)*st.cursor_msg_id;

	db_.execute(
		"INSERT INTO chat_backfill_state"
		" (chat_id, scope, cursor_msg_id, done, priority, last_fetch_at)"
		" VALUES (?, ?, ?, ?, ?, NOW()) AS new ON DUPLICATE KEY UPDATE"
		" scope = new.scope, cursor_msg_id = new.cursor_msg_id,"
		" done = new.done, priority = new.priority, last_fetch_at = NOW()",
		{ (int64_t)st.chat_id, st.scope, cursor_param,
		  (int64_t)(st.done ? 1 : 0), (int64_t)(st.priority ? 1 : 0) });
}

} /* namespace tgloggerd */
