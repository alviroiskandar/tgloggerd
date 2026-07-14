// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_AUTH_AUTHFILTER_HPP
#define TGLOGGERD_WEB_AUTH_AUTHFILTER_HPP

#include <drogon/HttpFilter.h>

namespace tgweb::auth {

/*
 * Require an authenticated session. Anonymous requests are redirected to
 * /login; authenticated ones fall through to the handler. Attach to every
 * browsing route (everything except /login and static assets).
 */
class AuthFilter : public drogon::HttpFilter<AuthFilter> {
public:
	void doFilter(const drogon::HttpRequestPtr &req,
		      drogon::FilterCallback &&fcb,
		      drogon::FilterChainCallback &&fccb) override;
};

} /* namespace tgweb::auth */

#endif /* TGLOGGERD_WEB_AUTH_AUTHFILTER_HPP */
