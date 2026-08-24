// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_AUTH_SESSION_HPP
#define TGLOGGERD_WEB_AUTH_SESSION_HPP

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <cstdint>
#include <optional>
#include <string>

namespace tgweb::auth::session {

/*
 * Near-stateless authentication: the identity lives in a signed cookie, so
 * logins survive restarts with no server-side session store. The cookie is a
 * token::make() token over "<uid>\n<exp>\n<epoch>\n<role>\n<username>", so the
 * client cannot alter any field without invalidating the signature. `exp` is an
 * absolute Unix expiry (0 = no expiry). `epoch` is the account's session epoch
 * at issue time; a caller (the AuthFilter) compares it against the live
 * web_users.session_epoch so a password change -- which bumps that column --
 * invalidates every previously issued cookie.
 */

constexpr const char *kCookie = "tgw_session";

struct Session {
	uint64_t    uid = 0;
	std::string username;
	std::string role;      /* "admin" or "viewer". */
	int64_t     exp = 0;   /* absolute Unix expiry; 0 = never. */
	uint32_t    epoch = 0; /* account session epoch carried by the cookie. */
};

/* The verified, unexpired session in the request's cookie, or std::nullopt. */
std::optional<Session> current(const drogon::HttpRequestPtr &req);

/* The raw signed cookie value (used to derive the CSRF token), or "". */
std::string rawCookie(const drogon::HttpRequestPtr &req);

bool isLoggedIn(const drogon::HttpRequestPtr &req);
bool isAdmin(const drogon::HttpRequestPtr &req);

/* Set the signed session cookie on the response (call on successful login, or
 * to re-issue with a bumped epoch after a password change). */
void issue(const drogon::HttpResponsePtr &resp, uint64_t uid,
	   const std::string &username, const std::string &role,
	   uint32_t epoch);

/* Expire the session cookie on the response (call on logout). */
void clear(const drogon::HttpResponsePtr &resp);

} /* namespace tgweb::auth::session */

#endif /* TGLOGGERD_WEB_AUTH_SESSION_HPP */
