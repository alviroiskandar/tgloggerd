// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "auth/AuthFilter.hpp"
#include "auth/Session.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>
#include <drogon/orm/Result.h>
#include <drogon/orm/Row.h>

#include <cstdint>
#include <memory>

namespace tgweb::auth {

namespace {

/* A redirect to the login page, optionally expiring the session cookie. */
drogon::HttpResponsePtr toLogin(bool clearCookie)
{
	auto resp = drogon::HttpResponse::newRedirectionResponse("/login");
	if (clearCookie)
		session::clear(resp);
	return resp;
}

} /* namespace */

void AuthFilter::doFilter(const drogon::HttpRequestPtr &req,
			  drogon::FilterCallback &&fcb,
			  drogon::FilterChainCallback &&fccb)
{
	auto s = session::current(req);
	if (!s) {
		fcb(toLogin(false));
		return;
	}

	/*
	 * The cookie's signature is valid; confirm its epoch still matches the
	 * account's, so a password change (which bumps web_users.session_epoch)
	 * invalidates every previously issued cookie -- signing the account out
	 * everywhere. One indexed primary-key lookup per authenticated request.
	 * The two continuation callbacks are shared so whichever branch runs owns
	 * a live copy.
	 */
	auto db = drogon::app().getDbClient("app");
	auto pass = std::make_shared<drogon::FilterChainCallback>(std::move(fccb));
	auto fail = std::make_shared<drogon::FilterCallback>(std::move(fcb));
	uint32_t epoch = s->epoch;

	db->execSqlAsync(
		"SELECT session_epoch FROM web_users WHERE id = ?",
		[pass, fail, epoch](const drogon::orm::Result &r) {
			if (!r.empty() &&
			    r[0]["session_epoch"].as<uint32_t>() == epoch)
				(*pass)();
			else
				(*fail)(toLogin(true));
		},
		[fail](const drogon::orm::DrogonDbException &) {
			/*
			 * Transient DB error: bounce to /login but keep the
			 * cookie, so a retry after recovery still works (every
			 * page needs the DB anyway).
			 */
			(*fail)(toLogin(false));
		},
		s->uid);
}

} /* namespace tgweb::auth */
