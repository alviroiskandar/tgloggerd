// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_USERSCONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_USERSCONTROLLER_HPP

#include <drogon/HttpController.h>

#include <string>

namespace tgweb::controllers {

/* Browse users and a single user's profile/history. Requires a session. */
class UsersController : public drogon::HttpController<UsersController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(UsersController::list, "/users", drogon::Get,
		      "tgweb::auth::AuthFilter");
	ADD_METHOD_TO(UsersController::detail, "/users/{1}", drogon::Get,
		      "tgweb::auth::AuthFilter");
	ADD_METHOD_TO(UsersController::chat, "/users/{1}/chat", drogon::Get,
		      "tgweb::auth::AuthFilter");
	ADD_METHOD_TO(UsersController::history, "/users/{1}/history",
		      drogon::Get, "tgweb::auth::AuthFilter");
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> detail(drogon::HttpRequestPtr req,
						     std::string id);
	drogon::Task<drogon::HttpResponsePtr> chat(drogon::HttpRequestPtr req,
						   std::string id);
	drogon::Task<drogon::HttpResponsePtr> history(drogon::HttpRequestPtr req,
						      std::string id);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_USERSCONTROLLER_HPP */
