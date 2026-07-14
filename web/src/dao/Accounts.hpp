// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_DAO_ACCOUNTS_HPP
#define TGLOGGERD_WEB_DAO_ACCOUNTS_HPP

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <string>

namespace tgweb::dao {

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

} /* namespace accounts */

} /* namespace tgweb::dao */

#endif /* TGLOGGERD_WEB_DAO_ACCOUNTS_HPP */
