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
	/*
	 * Normalise an over-large ?limit= in the page URL by redirecting to the
	 * capped value (the JSON API just clamps silently, but the page URL is
	 * user-facing and shareable, so it should read the effective value).
	 */
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
	data["title"] = "Users";
	data["entity"] = "users";
	data["detail_base"] = "/users";
	data["search_error"] = "";

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
		empty.limit = clampedIntParam(req, "limit", 10, 1,
					      dao::search::MAX_LIMIT);
		result = co_await dao::search::run(
			db, dao::search::usersSchema(), std::move(empty));
	}

	/* Tokenise photo cells so the SSR rows match the JSON API's rows. */
	enrichSearchPhotos(result);

	/* Positions of the id and photo columns (for cross-referencing links). */
	int idIdx = 0, photoIdx = 0;
	const auto &cols = result["cols"];
	for (size_t i = 0; i < cols.size(); i++) {
		std::string t = cols[i].value("type", std::string());
		if (t == "id")
			idIdx = (int)i;
		else if (t == "photo")
			photoIdx = (int)i;
	}

	int total     = result.value("total", 0);
	int limit     = result.value("limit", 50);
	int offset    = result.value("offset", 0);
	int maxOffset = result.value("max_offset", dao::search::MAX_OFFSET);

	/* Cap the page count at the deepest reachable page: offsets beyond
	 * maxOffset are clamped by the API, so paging past that would land on the
	 * same rows. Every offered page then maps to a distinct offset. */
	int pages = (limit > 0) ? (total + limit - 1) / limit : 1;
	int reach = (limit > 0) ? (maxOffset / limit) + 1 : 1;
	if (pages > reach)
		pages = reach;
	if (pages < 1)
		pages = 1;
	int cur = (limit > 0) ? offset / limit : 0;

	data["cols"]        = result["cols"];
	data["rows"]        = result["rows"];
	data["ncols"]       = (int)cols.size();
	data["id_index"]    = idIdx;
	data["photo_index"] = photoIdx;
	data["schema_json"] = result["fields"].dump(); /* server constants: safe raw */
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

drogon::Task<drogon::HttpResponsePtr>
UsersController::history(drogon::HttpRequestPtr req, std::string id)
{
	auto db = drogon::app().getDbClient("ro");

	int64_t uid = strtoll(id.c_str(), nullptr, 10);
	int limit  = clampedIntParam(req, "limit", 100, 1, 500);
	int offset = clampedIntParam(req, "offset", 0, 0, 100000);

	auto hist = co_await dao::browse::userHistory(db, uid, limit, offset);
	if (!hist)
		co_return renderStatus(req, drogon::k404NotFound, "User not found",
				       "No user exists with that id.");

	nlohmann::json data = pageBase(req);
	data["title"] = "History · user " + id;
	data["uid"]   = uid;

	auto header = co_await dao::browse::chatHeader(db, "private", uid);
	data["chat"] = header ? *header : nlohmann::json::object();

	int off = (*hist)["offset"].get<int>();
	int lim = (*hist)["limit"].get<int>();
	data["entries"]     = (*hist)["entries"];
	data["limit"]       = lim;
	data["offset"]      = off;
	data["has_more"]    = (*hist)["has_more"];
	data["has_prev"]    = off > 0;
	data["prev_offset"] = (off - lim < 0) ? 0 : off - lim;
	data["next_offset"] = off + lim;

	co_return htmlPage(views::Render::page("user_history.html", data));
}

} /* namespace tgweb::controllers */
