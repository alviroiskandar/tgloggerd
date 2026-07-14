// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_MESSAGESCONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_MESSAGESCONTROLLER_HPP

#include <drogon/HttpController.h>

#include <string>

namespace tgweb::controllers {

/*
 * Browse messages. {1} is the scope ("private" or "group"). Requires a
 * session.
 *   GET /messages/{scope}          keyset-paginated list (optional ?chat_id).
 *   GET /messages/{scope}/{id}     one message with edits/fwd/reply.
 */
class MessagesController : public drogon::HttpController<MessagesController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(MessagesController::list, "/messages/{1}", drogon::Get,
		      "tgweb::auth::AuthFilter");
	ADD_METHOD_TO(MessagesController::detail, "/messages/{1}/{2}", drogon::Get,
		      "tgweb::auth::AuthFilter");
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req,
						   std::string scope);
	drogon::Task<drogon::HttpResponsePtr> detail(drogon::HttpRequestPtr req,
						     std::string scope,
						     std::string id);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_MESSAGESCONTROLLER_HPP */
