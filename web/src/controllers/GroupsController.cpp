// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/GroupsController.hpp"

#include "controllers/Common.hpp"
#include "dao/Browse.hpp"
#include "views/Render.hpp"

#include <cstdint>
#include <cstdlib>
#include <optional>

namespace tgweb::controllers {

drogon::Task<drogon::HttpResponsePtr>
GroupsController::list(drogon::HttpRequestPtr req)
{
	auto db = drogon::app().getDbClient("ro");

	int limit = clampedIntParam(req, "limit", 50, 1, 200);
	int64_t cursor = 0;
	std::string before = req->getParameter("before");
	if (!before.empty())
		cursor = strtoll(before.c_str(), nullptr, 10);

	nlohmann::json data = pageBase(req);
	data["title"] = "Groups";
	data["limit"] = limit;

	nlohmann::json fields = nlohmann::json::array();
	auto addField = [&](const char *v, const char *l) {
		nlohmann::json o;
		o["value"] = v;
		o["label"] = l;
		fields.push_back(std::move(o));
	};
	addField("all", "All fields");
	addField("id", "ID");
	addField("title", "Title");
	addField("username", "Username");
	addField("description", "Description");

	std::string field;
	std::string query = applySearch(data, req, "/groups",
					"Search groups…", std::move(fields),
					field);

	nlohmann::json page = co_await dao::browse::listGroups(db, cursor, limit,
							       query, field);
	data["groups"] = page["groups"];
	data["next_cursor"] = page["next_cursor"];

	co_return htmlPage(views::Render::page("groups.html", data));
}

drogon::Task<drogon::HttpResponsePtr>
GroupsController::detail(drogon::HttpRequestPtr req, std::string id)
{
	auto db = drogon::app().getDbClient("ro");

	int64_t gid = strtoll(id.c_str(), nullptr, 10);
	auto found = co_await dao::browse::getGroup(db, gid);
	if (!found)
		co_return renderStatus(req, drogon::k404NotFound, "Group not found",
				       "No group exists with that id.");

	nlohmann::json data = pageBase(req);
	data.merge_patch(*found);
	data["title"] = "Group " + id;

	co_return htmlPage(views::Render::page("group.html", data));
}

drogon::Task<drogon::HttpResponsePtr>
GroupsController::admins(drogon::HttpRequestPtr req, std::string id)
{
	auto db = drogon::app().getDbClient("ro");

	int64_t gid = strtoll(id.c_str(), nullptr, 10);
	auto found = co_await dao::browse::getGroupAdmins(db, gid);
	if (!found)
		co_return renderStatus(req, drogon::k404NotFound, "Group not found",
				       "No group exists with that id.");

	nlohmann::json data = pageBase(req);
	data.merge_patch(*found);
	data["title"] = "Admins of group " + id;

	co_return htmlPage(views::Render::page("group_admins.html", data));
}

drogon::Task<drogon::HttpResponsePtr>
GroupsController::chat(drogon::HttpRequestPtr req, std::string id)
{
	auto db = drogon::app().getDbClient("ro");

	int64_t gid = strtoll(id.c_str(), nullptr, 10);
	auto header = co_await dao::browse::chatHeader(db, "group", gid);
	if (!header)
		co_return renderStatus(req, drogon::k404NotFound, "Group not found",
				       "No group exists with that id.");

	int limit = clampedIntParam(req, "limit", 30, 1, 100);
	std::optional<int64_t> after;
	std::string afterParam = req->getParameter("after");
	if (!afterParam.empty())
		after = strtoll(afterParam.c_str(), nullptr, 10);

	nlohmann::json data = pageBase(req);
	data["scope"] = "group";
	data["chat"]  = *header;
	data["title"] = (*header)["title"];

	nlohmann::json hist =
		co_await dao::browse::chatHistory(db, "group", gid, limit, after);
	data["messages"]    = hist["messages"];
	data["limit"]       = hist["limit"];
	data["has_older"]   = hist.contains("older_after");
	data["older_after"] = hist.contains("older_after") ? hist["older_after"]
							   : nlohmann::json(0);
	data["has_newer"]   = hist.contains("newer_after");
	data["newer_after"] = hist.contains("newer_after") ? hist["newer_after"]
							   : nlohmann::json(0);

	co_return htmlPage(views::Render::page("chat.html", data));
}

} /* namespace tgweb::controllers */
