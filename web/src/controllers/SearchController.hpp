// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_SEARCHCONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_SEARCHCONTROLLER_HPP

#include <drogon/HttpController.h>

#include <string>

namespace tgweb::controllers {

/*
 * Advanced-search JSON API. Requires a session (AuthFilter). The heavy lifting
 * -- the injection-safe query builder and the per-entity field registry -- is
 * in dao::search; this controller only parses the request, mints the tokenized
 * photo URLs the client cannot compute, and shapes the JSON/HTTP envelope.
 */
class SearchController : public drogon::HttpController<SearchController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(SearchController::users, "/v1/search/users", drogon::Get,
		      "tgweb::auth::AuthFilter");
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr> users(drogon::HttpRequestPtr req);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_SEARCHCONTROLLER_HPP */
