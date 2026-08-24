// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_GROUPSCONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_GROUPSCONTROLLER_HPP

#include <drogon/HttpController.h>

#include <string>

namespace tgweb::controllers {

/* Browse groups and a single group's admins/history. Requires a session. */
class GroupsController : public drogon::HttpController<GroupsController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(GroupsController::list, "/groups", drogon::Get,
		      "tgweb::auth::AuthFilter");
	ADD_METHOD_TO(GroupsController::detail, "/groups/{1}", drogon::Get,
		      "tgweb::auth::AuthFilter");
	ADD_METHOD_TO(GroupsController::admins, "/groups/{1}/admins", drogon::Get,
		      "tgweb::auth::AuthFilter");
	ADD_METHOD_TO(GroupsController::chat, "/groups/{1}/chat", drogon::Get,
		      "tgweb::auth::AuthFilter");
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> detail(drogon::HttpRequestPtr req,
						     std::string id);
	drogon::Task<drogon::HttpResponsePtr> admins(drogon::HttpRequestPtr req,
						     std::string id);
	drogon::Task<drogon::HttpResponsePtr> chat(drogon::HttpRequestPtr req,
						   std::string id);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_GROUPSCONTROLLER_HPP */
