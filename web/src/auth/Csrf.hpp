// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_AUTH_CSRF_HPP
#define TGLOGGERD_WEB_AUTH_CSRF_HPP

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <string>

namespace tgweb::auth::csrf {

/*
 * Stateless CSRF protection, with no server-side store.
 *
 * For an authenticated request the token is derived from the (signed, secret)
 * session cookie via HMAC, so it is unique per session and cannot be produced
 * without the key. The login form has no session yet, so it uses a separate
 * signed cookie whose value is echoed back in the form (double submit).
 */

/* Pre-auth (login) CSRF cookie name. */
constexpr const char *kCookie = "tgw_csrf";

/* --- Authenticated forms (derived from the session cookie). --- */

/* The CSRF token for the current session, or "" when not logged in. */
std::string forSession(const drogon::HttpRequestPtr &req);

/* Constant-time check of a submitted token against forSession(req). */
bool checkSession(const drogon::HttpRequestPtr &req,
		  const std::string &submitted);

/* --- Login form (pre-authentication). --- */

/* A fresh signed login token to embed in the form and set as the cookie. */
std::string newLoginToken(void);

/* Set the pre-auth CSRF cookie to `token` on the response. */
void setLoginCookie(const drogon::HttpResponsePtr &resp,
		    const std::string &token);

/*
 * Check a submitted login token: the pre-auth cookie must be present, carry a
 * valid signature, and equal the submitted value (constant time).
 */
bool checkLogin(const drogon::HttpRequestPtr &req,
		const std::string &submitted);

} /* namespace tgweb::auth::csrf */

#endif /* TGLOGGERD_WEB_AUTH_CSRF_HPP */
