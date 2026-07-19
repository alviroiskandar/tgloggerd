// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/FilesController.hpp"

#include "controllers/Common.hpp"
#include "dao/Browse.hpp"
#include "views/Render.hpp"

#include <cstdlib>
#include <string>

namespace tgweb::controllers {

drogon::Task<drogon::HttpResponsePtr>
FilesController::list(drogon::HttpRequestPtr req)
{
	auto db = drogon::app().getDbClient("ro");

	int limit = clampedIntParam(req, "limit", 50, 1, 200);
	int64_t cursor = 0;
	std::string before = req->getParameter("before");
	if (!before.empty())
		cursor = strtoll(before.c_str(), nullptr, 10);

	/* Optional file-type facet; the DAO ignores an unrecognized value. */
	std::string type = req->getParameter("type");

	nlohmann::json data = pageBase(req);
	data["title"] = "Files";
	data["limit"] = limit;

	nlohmann::json page =
		co_await dao::browse::listFiles(db, cursor, limit, type);
	data["files"] = page["files"];
	data["type"] = page["type"];
	data["next_cursor"] = page["next_cursor"];

	co_return htmlPage(views::Render::page("files.html", data));
}

} /* namespace tgweb::controllers */
