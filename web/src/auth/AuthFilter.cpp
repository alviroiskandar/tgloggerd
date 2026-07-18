// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "auth/AuthFilter.hpp"
#include "auth/Session.hpp"

#include <drogon/HttpResponse.h>

namespace tgweb::auth {

void AuthFilter::doFilter(const drogon::HttpRequestPtr &req,
			  drogon::FilterCallback &&fcb,
			  drogon::FilterChainCallback &&fccb)
{
	if (session::isLoggedIn(req)) {
		fccb();
		return;
	}

	auto resp = drogon::HttpResponse::newRedirectionResponse("/login");
	fcb(resp);
}

} /* namespace tgweb::auth */
