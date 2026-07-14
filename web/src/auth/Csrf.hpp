// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_AUTH_CSRF_HPP
#define TGLOGGERD_WEB_AUTH_CSRF_HPP

#include <drogon/Session.h>

#include <string>

namespace tgweb::auth::csrf {

/*
 * Return the session's CSRF token, generating and storing a new random one if
 * the session does not have a token yet. Embed this in every state-changing
 * form as a hidden field.
 */
std::string ensure(const drogon::SessionPtr &session);

/*
 * Constant-time comparison of a submitted token against the session's token.
 * Returns false if either token is missing or they differ.
 */
bool check(const drogon::SessionPtr &session, const std::string &submitted);

} /* namespace tgweb::auth::csrf */

#endif /* TGLOGGERD_WEB_AUTH_CSRF_HPP */
