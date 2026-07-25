// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/PrivateMessagesController.hpp"

#include "auth/Session.hpp"
#include "controllers/Common.hpp"
#include "dao/Search.hpp"
#include "views/Render.hpp"

#include <drogon/utils/Utilities.h>

#include <cstdint>
#include <cstdlib>
#include <string>

namespace tgweb::controllers {

/* Advanced-search private messages; mirrors Users/Groups/FilesController (shared
 * core + shared search_page.html). Rows link into the user profiles via their
 * "party" cell, so there is no row-level detail page (detail_base is empty). */
drogon::Task<drogon::HttpResponsePtr>
PrivateMessagesController::list(drogon::HttpRequestPtr req)
{
	std::string rawLimit = req->getParameter("limit");
	if (!rawLimit.empty() &&
	    strtol(rawLimit.c_str(), nullptr, 10) > dao::search::MAX_LIMIT) {
		auto params = req->getParameters();
		params["limit"] = std::to_string(dao::search::MAX_LIMIT);
		std::string qs;
		for (const auto &kv : params) {
			if (!qs.empty())
				qs += "&";
			qs += drogon::utils::urlEncodeComponent(kv.first) + "=" +
			      drogon::utils::urlEncodeComponent(kv.second);
		}
		co_return drogon::HttpResponse::newRedirectionResponse(
			req->getPath() + "?" + qs);
	}

	auto db = drogon::app().getDbClient("ro");

	dao::search::Request sreq;
	sreq.limit  = clampedIntParam(req, "limit", 10, 1, dao::search::MAX_LIMIT);
	sreq.offset = clampedIntParam(req, "offset", 0, 0, dao::search::MAX_OFFSET);
	sreq.sort   = req->getParameter("sort");
	sreq.order  = req->getParameter("order");
	sreq.debug  = (req->getParameter("debug") == "1") &&
		      auth::session::isAdmin(req);

	nlohmann::json data = pageBase(req);
	data["title"] = "Private messages";
	data["entity"] = "private_messages";
	data["detail_base"] = "";     /* rows link via the user "party" cell */
	data["search_error"] = "";

	std::string searchRaw = req->getParameter("search");
	std::string err;
	if (!dao::search::parseConditions(searchRaw, sreq.conds, err)) {
		data["search_error"] = views::Render::esc(err);
		sreq.conds.clear();
		searchRaw.clear();
	}

	nlohmann::json result = co_await dao::search::run(
		db, dao::search::privateMessagesSchema(), std::move(sreq));

	if (result.contains("error")) {
		data["search_error"] =
			views::Render::esc(result["error"].get<std::string>());
		searchRaw.clear();
		dao::search::Request empty;
		empty.limit = clampedIntParam(req, "limit", 10, 1,
					      dao::search::MAX_LIMIT);
		result = co_await dao::search::run(
			db, dao::search::privateMessagesSchema(), std::move(empty));
	}

	enrichSearchPhotos(result);

	int total     = result.value("total", 0);
	int limit     = result.value("limit", 10);
	int offset    = result.value("offset", 0);
	int maxOffset = result.value("max_offset", dao::search::MAX_OFFSET);
	int pages = (limit > 0) ? (total + limit - 1) / limit : 1;
	int reach = (limit > 0) ? (maxOffset / limit) + 1 : 1;
	if (pages > reach)
		pages = reach;
	if (pages < 1)
		pages = 1;
	int cur = (limit > 0) ? offset / limit : 0;

	data["cols"]        = result["cols"];
	data["rows"]        = result["rows"];
	data["ncols"]       = (int)result["cols"].size();
	data["id_index"]    = 0;
	data["photo_index"] = 0;
	data["type_index"]  = -1;
	data["stored_index"] = -1;
	data["schema_json"] = result["fields"].dump();
	data["total"]       = total;
	data["limit"]       = limit;
	data["offset"]      = offset;
	data["max_offset"]  = maxOffset;
	data["sort"]        = result.value("sort", std::string());
	data["order"]       = result.value("order", std::string("desc"));
	data["page_current"] = cur + 1;
	data["page_count"]  = pages;
	data["has_prev"]    = offset > 0;
	data["has_next"]    = cur < pages - 1;
	data["prev_offset"] = (offset - limit < 0) ? 0 : offset - limit;
	data["next_offset"] = (cur + 1) * limit;
	data["q_search"]    = drogon::utils::urlEncodeComponent(searchRaw);
	data["q_sort"] =
		drogon::utils::urlEncodeComponent(result.value("sort", std::string()));
	data["q_order"]     = result.value("order", std::string("desc"));
	data["has_debug"]   = result.contains("debug");
	if (result.contains("debug"))
		data["debug"] = result["debug"];

	co_return htmlPage(views::Render::page("search_page.html", data));
}

} /* namespace tgweb::controllers */
