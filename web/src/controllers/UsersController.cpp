// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/UsersController.hpp"

#include "controllers/Common.hpp"
#include "dao/Browse.hpp"
#include "views/Render.hpp"

#include <cstdint>
#include <cstdlib>

namespace tgweb::controllers {

drogon::Task<drogon::HttpResponsePtr>
UsersController::list(drogon::HttpRequestPtr req)
{
	auto db = drogon::app().getDbClient("ro");

	int limit = clampedIntParam(req, "limit", 50, 1, 200);
	int64_t cursor = 0;
	std::string before = req->getParameter("before");
	if (!before.empty())
		cursor = strtoll(before.c_str(), nullptr, 10);

	nlohmann::json data = pageBase(req);
	data["title"] = "Users";
	data["limit"] = limit;

	std::string query = applySearch(data, req, "/users",
					"Search by id, name, or username…");

	nlohmann::json page = co_await dao::browse::listUsers(db, cursor, limit,
							      query);
	data["users"] = page["users"];
	data["next_cursor"] = page["next_cursor"];

	co_return htmlPage(views::Render::page("users.html", data));
}

drogon::Task<drogon::HttpResponsePtr>
UsersController::detail(drogon::HttpRequestPtr req, std::string id)
{
	auto db = drogon::app().getDbClient("ro");

	int64_t uid = strtoll(id.c_str(), nullptr, 10);
	auto found = co_await dao::browse::getUser(db, uid);
	if (!found)
		co_return renderStatus(req, drogon::k404NotFound, "User not found",
				       "No user exists with that id.");

	nlohmann::json data = pageBase(req);
	data.merge_patch(*found);
	data["title"] = "User " + id;

	co_return htmlPage(views::Render::page("user.html", data));
}

} /* namespace tgweb::controllers */
