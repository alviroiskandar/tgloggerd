// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/DashboardController.hpp"

#include "controllers/Common.hpp"
#include "dao/Browse.hpp"
#include "views/Render.hpp"

namespace tgweb::controllers {

drogon::Task<drogon::HttpResponsePtr>
DashboardController::index(drogon::HttpRequestPtr req)
{
	auto db = drogon::app().getDbClient("ro");

	nlohmann::json data = pageBase(req);
	data["title"] = "Dashboard";
	data["counts"] = co_await dao::browse::counts(db);

	co_return htmlPage(views::Render::page("dashboard.html", data));
}

} /* namespace tgweb::controllers */
