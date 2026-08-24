// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/McpAdminController.hpp"

#include "auth/Csrf.hpp"
#include "auth/Session.hpp"
#include "controllers/Common.hpp"
#include "dao/Mcp.hpp"
#include "mcp/Token.hpp"
#include "views/Render.hpp"

#include <cstdlib>
#include <exception>
#include <string>

namespace tgweb::controllers {

namespace {

drogon::HttpResponsePtr jsonResp(const nlohmann::json &j,
				 drogon::HttpStatusCode code = drogon::k200OK)
{
	auto resp = drogon::HttpResponse::newHttpResponse();
	resp->setStatusCode(code);
	resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
	resp->setBody(j.dump());
	return resp;
}

drogon::HttpResponsePtr jsonError(const std::string &msg,
				  drogon::HttpStatusCode code = drogon::k400BadRequest)
{
	return jsonResp({ { "ok", false }, { "error", msg } }, code);
}

} /* namespace */

drogon::Task<drogon::HttpResponsePtr>
McpAdminController::index(drogon::HttpRequestPtr req)
{
	auto ro = drogon::app().getDbClient("ro");
	auto app = drogon::app().getDbClient("app");

	nlohmann::json data = pageBase(req);
	data["title"] = "MCP";

	uint64_t exposed = 0, msgs = 0, tokensLive = 0;
	try {
		auto r = co_await ro->execSqlCoro(
			"SELECT COUNT(1) AS n, "
			"COALESCE(SUM(g.msg_count), 0) AS m "
			"FROM telegram_public_groups p "
			"JOIN `telegram_groups` g ON g.id = p.group_id");
		if (!r.empty()) {
			exposed = r[0]["n"].as<uint64_t>();
			msgs = r[0]["m"].as<uint64_t>();
		}
	} catch (const std::exception &) {
		/* The groups page reports why; the index just shows zero. */
	}
	try {
		auto r = co_await app->execSqlCoro(
			"SELECT COUNT(1) AS n FROM web_mcp_tokens "
			"WHERE revoked_at IS NULL");
		if (!r.empty())
			tokensLive = r[0]["n"].as<uint64_t>();
	} catch (const std::exception &) {
	}

	data["exposed_groups"] = exposed;
	data["exposed_messages"] = msgs;
	data["live_tokens"] = tokensLive;
	co_return htmlPage(views::Render::page("mcp_index.html", data));
}

drogon::Task<drogon::HttpResponsePtr>
McpAdminController::groups(drogon::HttpRequestPtr req)
{
	auto db = drogon::app().getDbClient("ro");

	nlohmann::json data = pageBase(req);
	data["title"] = "MCP exposed groups";
	try {
		data["groups"] = co_await dao::mcp::listAllowed(db);
		data["db_error"] = "";
	} catch (const std::exception &e) {
		data["groups"] = nlohmann::json::array();
		data["db_error"] = views::Render::esc(e.what());
	}
	co_return htmlPage(views::Render::page("mcp_groups.html", data));
}

drogon::Task<drogon::HttpResponsePtr>
McpAdminController::groupsAdd(drogon::HttpRequestPtr req)
{
	if (!auth::csrf::checkSession(req, req->getParameter("csrf")))
		co_return jsonError("Your session expired. Please reload.",
				    drogon::k403Forbidden);

	const std::string idStr = req->getParameter("group_id");
	if (idStr.empty())
		co_return jsonError("Choose a group to expose.");

	const int64_t groupId = std::strtoll(idStr.c_str(), nullptr, 10);
	if (!groupId)
		co_return jsonError("That is not a valid group id.");

	auto s = auth::session::current(req);
	const uint64_t uid = s ? s->uid : 0;

	auto db = drogon::app().getDbClient("ro");
	try {
		if (!co_await dao::mcp::groupExists(db, groupId))
			co_return jsonError(
				"That group is not in the log, so there is "
				"nothing to expose.");

		if (co_await dao::mcp::isAllowed(db, groupId))
			co_return jsonError(
				"That group is already exposed. To change its "
				"note, remove it and add it again.");

		co_await dao::mcp::allowGroup(db, groupId,
					      req->getParameter("note"), uid);
	} catch (const std::exception &e) {
		co_return jsonError(std::string("Database error: ") + e.what(),
				    drogon::k500InternalServerError);
	}
	co_return jsonResp({ { "ok", true } });
}

drogon::Task<drogon::HttpResponsePtr>
McpAdminController::groupsRemove(drogon::HttpRequestPtr req)
{
	if (!auth::csrf::checkSession(req, req->getParameter("csrf")))
		co_return jsonError("Your session expired. Please reload.",
				    drogon::k403Forbidden);

	const std::string idStr = req->getParameter("group_id");
	if (idStr.empty())
		co_return jsonError("Missing group id.");

	auto db = drogon::app().getDbClient("ro");
	try {
		/*
		 * No confirmation gate here beyond the UI's: removing is the
		 * SAFE direction. It can only ever reduce what the MCP server
		 * can see, so making it hard would be backwards.
		 */
		co_await dao::mcp::disallowGroup(
			db, std::strtoll(idStr.c_str(), nullptr, 10));
	} catch (const std::exception &e) {
		co_return jsonError(std::string("Database error: ") + e.what(),
				    drogon::k500InternalServerError);
	}
	co_return jsonResp({ { "ok", true } });
}

drogon::Task<drogon::HttpResponsePtr>
McpAdminController::groupsSearch(drogon::HttpRequestPtr req)
{
	auto db = drogon::app().getDbClient("ro");
	const std::string q = req->getParameter("q");
	const int limit = clampedIntParam(req, "limit", 20, 1, 50);

	nlohmann::json out;
	out["results"] = nlohmann::json::array();
	try {
		for (auto &g : co_await dao::mcp::searchGroups(db, q, limit)) {
			out["results"].push_back({
				{ "id", g["group_id"] },
				{ "text", g["text"] },
				{ "is_public_now", g["is_public_now"] },
			});
		}
	} catch (const std::exception &e) {
		co_return jsonError(std::string("Database error: ") + e.what(),
				    drogon::k500InternalServerError);
	}
	co_return jsonResp(out);
}

drogon::Task<drogon::HttpResponsePtr>
McpAdminController::tokens(drogon::HttpRequestPtr req)
{
	auto db = drogon::app().getDbClient("app");

	nlohmann::json data = pageBase(req);
	data["title"] = "MCP tokens";
	try {
		data["tokens"] = co_await dao::mcp::listTokens(db);
		data["db_error"] = "";
	} catch (const std::exception &e) {
		data["tokens"] = nlohmann::json::array();
		data["db_error"] = views::Render::esc(e.what());
	}
	co_return htmlPage(views::Render::page("mcp_tokens.html", data));
}

drogon::Task<drogon::HttpResponsePtr>
McpAdminController::tokensMint(drogon::HttpRequestPtr req)
{
	if (!auth::csrf::checkSession(req, req->getParameter("csrf")))
		co_return jsonError("Your session expired. Please reload.",
				    drogon::k403Forbidden);

	auto s = auth::session::current(req);
	if (!s)
		co_return jsonError("Not signed in.", drogon::k403Forbidden);

	std::string name = req->getParameter("name");
	if (name.size() > 64)
		name.resize(64);

	const std::string plaintext = mcp::token::mint();

	auto db = drogon::app().getDbClient("app");
	try {
		co_await dao::mcp::createToken(db, s->uid, name,
					       mcp::token::digest(plaintext));
	} catch (const std::exception &e) {
		co_return jsonError(std::string("Database error: ") + e.what(),
				    drogon::k500InternalServerError);
	}

	/*
	 * The ONLY time the plaintext exists outside the client's hands. It is
	 * not stored -- only its digest is -- so it cannot be shown again, and
	 * this response is the one chance to copy it.
	 */
	co_return jsonResp({ { "ok", true }, { "token", plaintext } });
}

drogon::Task<drogon::HttpResponsePtr>
McpAdminController::tokensRevoke(drogon::HttpRequestPtr req)
{
	if (!auth::csrf::checkSession(req, req->getParameter("csrf")))
		co_return jsonError("Your session expired. Please reload.",
				    drogon::k403Forbidden);

	const std::string idStr = req->getParameter("id");
	if (idStr.empty())
		co_return jsonError("Missing token id.");

	auto db = drogon::app().getDbClient("app");
	try {
		co_await dao::mcp::revokeToken(
			db, std::strtoull(idStr.c_str(), nullptr, 10));
	} catch (const std::exception &e) {
		co_return jsonError(std::string("Database error: ") + e.what(),
				    drogon::k500InternalServerError);
	}
	co_return jsonResp({ { "ok", true } });
}

} /* namespace tgweb::controllers */
