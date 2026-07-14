// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "dao/Accounts.hpp"

namespace tgweb::dao::accounts {

drogon::Task<void> seedAdmin(drogon::orm::DbClientPtr db,
			     std::string username,
			     std::string passwordHash)
{
	/*
	 * INSERT ... ON DUPLICATE KEY UPDATE makes --seed-admin idempotent: a
	 * fresh run creates the admin, a repeat run resets the password and
	 * re-enables the account. All values are bound parameters.
	 */
	co_await db->execSqlCoro(
		"INSERT INTO web_users (username, password_hash, role, is_active) "
		"VALUES (?, ?, 'admin', 1) "
		"ON DUPLICATE KEY UPDATE "
		"password_hash = VALUES(password_hash), "
		"role = 'admin', is_active = 1",
		username, passwordHash);
	co_return;
}

drogon::Task<std::optional<WebUser>> findByUsername(drogon::orm::DbClientPtr db,
						    std::string username)
{
	auto rows = co_await db->execSqlCoro(
		"SELECT id, username, password_hash, role, is_active "
		"FROM web_users WHERE username = ?",
		username);

	if (rows.empty())
		co_return std::nullopt;

	const auto &r = rows[0];
	WebUser u;
	u.id           = r["id"].as<uint64_t>();
	u.username     = r["username"].as<std::string>();
	u.passwordHash = r["password_hash"].as<std::string>();
	u.role         = r["role"].as<std::string>();
	u.isActive     = r["is_active"].as<int>() != 0;
	co_return u;
}

} /* namespace tgweb::dao::accounts */
