// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_AUTH_SESSION_HPP
#define TGLOGGERD_WEB_AUTH_SESSION_HPP

#include <drogon/Session.h>

#include <cstdint>
#include <string>

namespace tgweb::auth::session {

/* Session keys for the authenticated identity. */
constexpr const char *kUid      = "uid";       /* web_users.id (uint64_t).   */
constexpr const char *kUsername = "username";  /* Login name (std::string).  */
constexpr const char *kRole     = "role";      /* "admin" or "viewer".       */

inline bool isLoggedIn(const drogon::SessionPtr &s)
{
	return s && s->find(kUid);
}

inline bool isAdmin(const drogon::SessionPtr &s)
{
	return s && s->getOptional<std::string>(kRole).value_or("") == "admin";
}

/* Store the authenticated identity and rotate the session id (anti-fixation). */
inline void login(const drogon::SessionPtr &s, uint64_t uid,
		  const std::string &username, const std::string &role)
{
	/* insert() does not overwrite; erase first so re-login is well defined. */
	s->erase(kUid);
	s->erase(kUsername);
	s->erase(kRole);
	s->insert(kUid, uid);
	s->insert(kUsername, username);
	s->insert(kRole, role);
	s->changeSessionIdToClient();
}

} /* namespace tgweb::auth::session */

#endif /* TGLOGGERD_WEB_AUTH_SESSION_HPP */
