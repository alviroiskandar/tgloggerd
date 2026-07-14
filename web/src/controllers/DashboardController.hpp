// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_DASHBOARDCONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_DASHBOARDCONTROLLER_HPP

#include <drogon/HttpController.h>

namespace tgweb::controllers {

/* The landing page: overall counts. Requires an authenticated session. */
class DashboardController : public drogon::HttpController<DashboardController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(DashboardController::index, "/", drogon::Get,
		      "tgweb::auth::AuthFilter");
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr> index(drogon::HttpRequestPtr req);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_DASHBOARDCONTROLLER_HPP */
