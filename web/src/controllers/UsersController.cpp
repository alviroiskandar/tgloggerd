// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/UsersController.hpp"

#include "auth/FileToken.hpp"
#include "auth/Session.hpp"
#include "controllers/Common.hpp"
#include "dao/Browse.hpp"
#include "dao/Search.hpp"
#include "views/Render.hpp"

#include <drogon/utils/Utilities.h>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

namespace tgweb::controllers {

/*
 * The advanced-search listing. Server-renders the first page (so a shared
 * ?search=... URL reproduces without JS and SEO/no-JS still works), then
 * users-search.js takes over the condition builder, sorting and paging by
 * calling the /v1/search/users API. Both render identical rows.
 */
drogon::Task<drogon::HttpResponsePtr>
UsersController::list(drogon::HttpRequestPtr req)
{
	auto db = drogon::app().getDbClient("ro");

	dao::search::Request sreq;
	sreq.limit  = clampedIntParam(req, "limit", 50, 1, dao::search::MAX_LIMIT);
	sreq.offset = clampedIntParam(req, "offset", 0, 0, dao::search::MAX_OFFSET);
	sreq.sort   = req->getParameter("sort");
	sreq.order  = req->getParameter("order");
	sreq.debug  = (req->getParameter("debug") == "1") &&
		      auth::session::isAdmin(req);

	nlohmann::json data = pageBase(req);
	data["title"] = "Users";
	data["search_error"] = "";
	data["debug_json"]   = "";

	std::string searchRaw = req->getParameter("search");
	std::string err;
	if (!dao::search::parseConditions(searchRaw, sreq.conds, err)) {
		data["search_error"] = views::Render::esc(err);
		sreq.conds.clear();
		searchRaw.clear();
	}

	nlohmann::json result = co_await dao::search::run(
		db, dao::search::usersSchema(), std::move(sreq));

	/* A syntactically valid but semantically bad search (unknown field,
	 * disallowed operator): show the note and fall back to browse-all. */
	if (result.contains("error")) {
		data["search_error"] =
			views::Render::esc(result["error"].get<std::string>());
		searchRaw.clear();
		dao::search::Request empty;
		empty.limit = clampedIntParam(req, "limit", 50, 1,
					      dao::search::MAX_LIMIT);
		result = co_await dao::search::run(
			db, dao::search::usersSchema(), std::move(empty));
	}

	int total  = result.value("total", 0);
	int limit  = result.value("limit", 50);
	int offset = result.value("offset", 0);
	int pages  = (limit > 0) ? (total + limit - 1) / limit : 1;
	if (pages < 1)
		pages = 1;

	data["rows"]        = result["rows"];
	data["columns"]     = result["columns"];
	data["schema_json"] = result["fields"].dump(); /* server constants: safe raw */
	data["total"]       = total;
	data["limit"]       = limit;
	data["offset"]      = offset;
	data["sort"]        = result.value("sort", std::string());
	data["order"]       = result.value("order", std::string("desc"));
	data["page_current"] = (limit > 0) ? (offset / limit) + 1 : 1;
	data["page_count"]  = pages;
	data["has_prev"]    = offset > 0;
	data["has_next"]    = offset + limit < total;
	data["prev_offset"] = (offset - limit < 0) ? 0 : offset - limit;
	data["next_offset"] = offset + limit;
	data["q_search"]    = drogon::utils::urlEncodeComponent(searchRaw);
	data["q_sort"] =
		drogon::utils::urlEncodeComponent(result.value("sort", std::string()));
	data["q_order"]     = result.value("order", std::string("desc"));
	if (result.contains("debug"))
		data["debug_json"] = result["debug"].dump(2);

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

drogon::Task<drogon::HttpResponsePtr>
UsersController::chat(drogon::HttpRequestPtr req, std::string id)
{
	auto db = drogon::app().getDbClient("ro");

	int64_t uid = strtoll(id.c_str(), nullptr, 10);
	auto header = co_await dao::browse::chatHeader(db, "private", uid);
	if (!header)
		co_return renderStatus(req, drogon::k404NotFound, "User not found",
				       "No user exists with that id.");

	int limit = clampedIntParam(req, "limit", 30, 1, 100);
	std::optional<int64_t> after;
	std::string afterParam = req->getParameter("after");
	if (!afterParam.empty())
		after = strtoll(afterParam.c_str(), nullptr, 10);
	std::optional<int64_t> afterTs;
	std::string afterTsParam = req->getParameter("after_ts");
	if (!afterTsParam.empty())
		afterTs = strtoll(afterTsParam.c_str(), nullptr, 10);

	nlohmann::json data = pageBase(req);
	data["scope"] = "private";
	data["chat"]  = *header;
	data["title"] = (*header)["title"];

	nlohmann::json hist = co_await dao::browse::chatHistory(
		db, "private", uid, limit, after, afterTs);
	data["messages"]    = hist["messages"];
	data["limit"]       = hist["limit"];
	data["has_older"]   = hist.contains("older_after");
	data["older_after"] = hist.contains("older_after") ? hist["older_after"]
							   : nlohmann::json(0);
	data["has_newer"]   = hist.contains("newer_after");
	data["newer_after"] = hist.contains("newer_after") ? hist["newer_after"]
							   : nlohmann::json(0);
	data["oldest_msg_id"] = hist.value("oldest_msg_id", (int64_t)0);
	data["newest_msg_id"] = hist.value("newest_msg_id", (int64_t)0);

	co_return htmlPage(views::Render::page("chat.html", data));
}

} /* namespace tgweb::controllers */
