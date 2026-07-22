// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_DAO_ACCOUNTS_HPP
#define TGLOGGERD_WEB_DAO_ACCOUNTS_HPP

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <optional>
#include <string>

namespace tgweb::dao {

/* A web login account (web_users row). */
struct WebUser {
	uint64_t    id;
	std::string username;
	std::string passwordHash;
	std::string role;      /* "admin" or "viewer". */
	bool        isActive;
	uint32_t    epoch;     /* session_epoch: bumped to invalidate cookies. */
};

/*
 * Account data access over the "app" database (tgloggerd_web). Callers pass in
 * the "app" DbClient, keeping this decoupled from how the client is obtained.
 */
namespace accounts {

/*
 * Create an active admin account, or, if the username already exists, reset
 * its password and re-enable it as an admin. Awaits Drogon's async client, so
 * it runs on the event loop. Throws drogon::orm::DrogonDbException on error.
 */
drogon::Task<void> seedAdmin(drogon::orm::DbClientPtr db,
			     std::string username,
			     std::string passwordHash);

/*
 * Look up an account by username. Returns std::nullopt if no such row exists.
 * The caller checks isActive and verifies the password.
 */
drogon::Task<std::optional<WebUser>> findByUsername(drogon::orm::DbClientPtr db,
						    std::string username);

/*
 * Look up an account by id (the trusted identity carried in the session
 * cookie). Returns std::nullopt if no such row exists.
 */
drogon::Task<std::optional<WebUser>> findById(drogon::orm::DbClientPtr db,
					      uint64_t id);

/*
 * Replace an account's password hash and, atomically, bump its session_epoch so
 * every previously issued session cookie is invalidated. `passwordHash` must
 * already be a self-describing argon2id string (see auth::hashPassword).
 * Returns the new epoch (for re-issuing the caller's own cookie). Throws
 * drogon::orm::DrogonDbException on a database error.
 */
drogon::Task<uint32_t> setPassword(drogon::orm::DbClientPtr db, uint64_t id,
				   std::string passwordHash);

} /* namespace accounts */

} /* namespace tgweb::dao */

#endif /* TGLOGGERD_WEB_DAO_ACCOUNTS_HPP */
