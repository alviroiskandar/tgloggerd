// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_AUTH_ADMINFILTER_HPP
#define TGLOGGERD_WEB_AUTH_ADMINFILTER_HPP

#include <drogon/HttpFilter.h>

namespace tgweb::auth {

/*
 * Require an admin session. Anonymous requests are redirected to /login;
 * authenticated non-admins get 403. Attach to admin-only surfaces (account
 * management, audit view) on top of AuthFilter.
 */
class AdminFilter : public drogon::HttpFilter<AdminFilter> {
public:
	void doFilter(const drogon::HttpRequestPtr &req,
		      drogon::FilterCallback &&fcb,
		      drogon::FilterChainCallback &&fccb) override;
};

} /* namespace tgweb::auth */

#endif /* TGLOGGERD_WEB_AUTH_ADMINFILTER_HPP */
