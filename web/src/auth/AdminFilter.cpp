// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "auth/AdminFilter.hpp"
#include "auth/Session.hpp"

#include <drogon/HttpResponse.h>

namespace tgweb::auth {

void AdminFilter::doFilter(const drogon::HttpRequestPtr &req,
			   drogon::FilterCallback &&fcb,
			   drogon::FilterChainCallback &&fccb)
{
	if (!session::isLoggedIn(req)) {
		auto resp = drogon::HttpResponse::newRedirectionResponse("/login");
		fcb(resp);
		return;
	}

	if (!session::isAdmin(req)) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::k403Forbidden);
		resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
		resp->setBody("403 Forbidden\n");
		fcb(resp);
		return;
	}

	fccb();
}

} /* namespace tgweb::auth */
