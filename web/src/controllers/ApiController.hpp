// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_APICONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_APICONTROLLER_HPP

#include <drogon/HttpController.h>

#include <string>

namespace tgweb::controllers {

/*
 * JSON API under /v1/. Same data the server-rendered chat view uses, so the
 * chat page can load more messages (infinite scroll), follow a reply off the
 * page, and tail new messages without a full reload. Requires a session (the
 * chat content is private), like the HTML chat pages.
 */
class ApiController : public drogon::HttpController<ApiController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(ApiController::messages,
		      "/v1/chats/{1}/{2}/messages", drogon::Get,
		      "tgweb::auth::AuthFilter");
	METHOD_LIST_END

	/* scope = "group" | "private"; id = chat id. Query: limit, after,
	 * after_ts (same meaning as the HTML chat view). */
	drogon::Task<drogon::HttpResponsePtr> messages(drogon::HttpRequestPtr req,
						       std::string scope,
						       std::string id);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_APICONTROLLER_HPP */
