// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_AUTHCONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_AUTHCONTROLLER_HPP

#include <drogon/HttpController.h>

namespace tgweb::controllers {

/*
 * Login and logout. GET /login shows the form (redirecting already-authenticated
 * users to /); POST /login verifies credentials (CSRF-protected, rate-limited)
 * and establishes the session; POST /logout clears it (CSRF-protected).
 */
class AuthController : public drogon::HttpController<AuthController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(AuthController::getLogin, "/login", drogon::Get);
	ADD_METHOD_TO(AuthController::postLogin, "/login", drogon::Post);
	ADD_METHOD_TO(AuthController::postLogout, "/logout", drogon::Post);
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr>
	getLogin(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr>
	postLogin(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr>
	postLogout(drogon::HttpRequestPtr req);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_AUTHCONTROLLER_HPP */
