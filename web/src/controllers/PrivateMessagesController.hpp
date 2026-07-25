// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_PRIVATEMESSAGESCONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_PRIVATEMESSAGESCONTROLLER_HPP

#include <drogon/HttpController.h>

namespace tgweb::controllers {

/* Advanced-search listing of private (one-to-one) messages. Each row shows the
 * peer user (photo + name + username, linking to their profile). Requires a
 * session. Shares the search framework + search_page.html. */
class PrivateMessagesController
	: public drogon::HttpController<PrivateMessagesController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(PrivateMessagesController::list, "/private_messages",
		      drogon::Get, "tgweb::auth::AuthFilter");
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_PRIVATEMESSAGESCONTROLLER_HPP */
