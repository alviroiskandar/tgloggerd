// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "dao/Audit.hpp"

namespace tgweb::dao::audit {

drogon::Task<void> log(drogon::orm::DbClientPtr db,
		       std::optional<uint64_t> uid,
		       std::string action,
		       std::string ip,
		       std::string detail)
{
	co_await db->execSqlCoro(
		"INSERT INTO web_audit (web_user_id, action, ip, detail) "
		"VALUES (?, ?, ?, ?)",
		uid, action, ip, detail);
	co_return;
}

} /* namespace tgweb::dao::audit */
