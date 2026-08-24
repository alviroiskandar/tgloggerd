// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_DAO_AUDIT_HPP
#define TGLOGGERD_WEB_DAO_AUDIT_HPP

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <optional>
#include <string>

namespace tgweb::dao::audit {

/*
 * Append a security event to web_audit over the "app" database. uid is
 * std::nullopt for anonymous or failed actions (e.g. login_fail for an unknown
 * username). Throws drogon::orm::DrogonDbException on a database error.
 */
drogon::Task<void> log(drogon::orm::DbClientPtr db,
		       std::optional<uint64_t> uid,
		       std::string action,
		       std::string ip,
		       std::string detail);

} /* namespace tgweb::dao::audit */

#endif /* TGLOGGERD_WEB_DAO_AUDIT_HPP */
