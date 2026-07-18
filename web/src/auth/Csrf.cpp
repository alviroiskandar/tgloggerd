// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "auth/Csrf.hpp"

#include "Config.hpp"
#include "auth/Session.hpp"
#include "auth/Token.hpp"

#include <sodium.h>

namespace tgweb::auth::csrf {

namespace {

bool secure(void)
{
	static const bool v = tgweb::env("WEB_SECURE_COOKIE", "1") != "0";
	return v;
}

/* Constant-time equality; false when either side is empty or lengths differ. */
bool ctEqual(const std::string &a, const std::string &b)
{
	if (a.empty() || a.size() != b.size())
		return false;
	return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

} /* namespace */

std::string forSession(const drogon::HttpRequestPtr &req)
{
	std::string s = session::rawCookie(req);
	if (s.empty())
		return std::string();
	/* Keyed tag over the session cookie: unforgeable without WEB_APP_KEY,
	 * and it rotates whenever the session cookie changes (re-login). */
	return token::tag("csrf\n" + s);
}

bool checkSession(const drogon::HttpRequestPtr &req,
		  const std::string &submitted)
{
	return ctEqual(forSession(req), submitted);
}

std::string newLoginToken(void)
{
	return token::make(token::randomToken());
}

void setLoginCookie(const drogon::HttpResponsePtr &resp,
		    const std::string &tok)
{
	drogon::Cookie c(kCookie, tok);
	c.setHttpOnly(true);
	c.setSecure(secure());
	c.setSameSite(drogon::Cookie::SameSite::kLax);
	c.setPath("/login");
	c.setMaxAge(1800); /* enough time to complete the sign-in. */
	resp->addCookie(std::move(c));
}

bool checkLogin(const drogon::HttpRequestPtr &req,
		const std::string &submitted)
{
	std::string cookie = req->getCookie(kCookie);
	/* Signature valid (not attacker-injected) AND echoed back in the form. */
	if (!token::open(cookie).has_value())
		return false;
	return ctEqual(cookie, submitted);
}

} /* namespace tgweb::auth::csrf */
