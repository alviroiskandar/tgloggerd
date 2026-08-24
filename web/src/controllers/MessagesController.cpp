// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/MessagesController.hpp"

#include "controllers/Common.hpp"
#include "dao/Browse.hpp"
#include "views/Render.hpp"

#include <cstdint>
#include <cstdlib>
#include <optional>

namespace tgweb::controllers {

namespace {

bool validScope(const std::string &s)
{
	return s == "private" || s == "group";
}

} /* namespace */

drogon::Task<drogon::HttpResponsePtr>
MessagesController::list(drogon::HttpRequestPtr req, std::string scope)
{
	if (!validScope(scope))
		co_return renderStatus(req, drogon::k404NotFound, "Not found",
				       "Unknown message scope.");

	auto db = drogon::app().getDbClient("ro");

	int limit = clampedIntParam(req, "limit", 50, 1, 200);
	int64_t cursor = 0;
	std::string before = req->getParameter("before");
	if (!before.empty())
		cursor = strtoll(before.c_str(), nullptr, 10);

	std::optional<int64_t> chatId;
	std::string chatParam = req->getParameter("chat_id");
	if (!chatParam.empty())
		chatId = strtoll(chatParam.c_str(), nullptr, 10);

	nlohmann::json data = pageBase(req);
	data["title"] = scope == "group" ? "Group messages" : "Private messages";
	data["scope"] = scope;
	data["limit"] = limit;
	data["has_chat"] = chatId.has_value();
	if (chatId)
		data["chat_id"] = *chatId;

	nlohmann::json page =
		co_await dao::browse::listMessages(db, scope, chatId, cursor, limit);
	data["messages"] = page["messages"];
	data["next_cursor"] = page["next_cursor"];

	co_return htmlPage(views::Render::page("messages.html", data));
}

drogon::Task<drogon::HttpResponsePtr>
MessagesController::detail(drogon::HttpRequestPtr req, std::string scope,
			   std::string id)
{
	if (!validScope(scope))
		co_return renderStatus(req, drogon::k404NotFound, "Not found",
				       "Unknown message scope.");

	auto db = drogon::app().getDbClient("ro");

	int64_t mid = strtoll(id.c_str(), nullptr, 10);
	auto found = co_await dao::browse::getMessage(db, scope, mid);
	if (!found)
		co_return renderStatus(req, drogon::k404NotFound, "Message not found",
				       "No message exists with that id.");

	nlohmann::json data = pageBase(req);
	data.merge_patch(*found);
	data["title"] = "Message " + id;

	co_return htmlPage(views::Render::page("message.html", data));
}

} /* namespace tgweb::controllers */
