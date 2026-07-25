// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_GROUPMESSAGESCONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_GROUPMESSAGESCONTROLLER_HPP

#include <drogon/HttpController.h>

namespace tgweb::controllers {

/* Advanced-search listing of group messages. Each row shows the group photo
 * (linking to the group) and the sender's photo + name (linking to the user).
 * Requires a session. Shares the search framework + search_page.html. */
class GroupMessagesController
	: public drogon::HttpController<GroupMessagesController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(GroupMessagesController::list, "/group_messages",
		      drogon::Get, "tgweb::auth::AuthFilter");
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_GROUPMESSAGESCONTROLLER_HPP */
