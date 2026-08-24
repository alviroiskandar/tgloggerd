// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_MCPADMINCONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_MCPADMINCONTROLLER_HPP

#include <drogon/HttpController.h>

namespace tgweb::controllers {

/*
 * Admin pages for the MCP server: which Telegram groups it may expose, and
 * which bearer tokens may call it.
 *
 * Both are security surfaces -- one decides what leaves the archive, the other
 * decides who can ask -- so every route here is AuthFilter + AdminFilter.
 *
 * Note the endpoint itself (/mcp, McpController) is deliberately NOT part of
 * this controller: it is machine-facing, bearer-authenticated, and must not sit
 * behind the session filters these pages use.
 */
class McpAdminController : public drogon::HttpController<McpAdminController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(McpAdminController::index, "/mcp-admin", drogon::Get,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");

	ADD_METHOD_TO(McpAdminController::groups, "/mcp-admin/groups",
		      drogon::Get, "tgweb::auth::AuthFilter",
		      "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(McpAdminController::groupsAdd, "/mcp-admin/groups/add",
		      drogon::Post, "tgweb::auth::AuthFilter",
		      "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(McpAdminController::groupsRemove,
		      "/mcp-admin/groups/remove", drogon::Post,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(McpAdminController::groupsSearch,
		      "/mcp-admin/groups/search", drogon::Get,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");

	ADD_METHOD_TO(McpAdminController::tokens, "/mcp-admin/tokens",
		      drogon::Get, "tgweb::auth::AuthFilter",
		      "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(McpAdminController::tokensMint, "/mcp-admin/tokens/mint",
		      drogon::Post, "tgweb::auth::AuthFilter",
		      "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(McpAdminController::tokensRevoke,
		      "/mcp-admin/tokens/revoke", drogon::Post,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr> index(drogon::HttpRequestPtr req);

	drogon::Task<drogon::HttpResponsePtr> groups(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> groupsAdd(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> groupsRemove(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> groupsSearch(drogon::HttpRequestPtr req);

	drogon::Task<drogon::HttpResponsePtr> tokens(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> tokensMint(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> tokensRevoke(drogon::HttpRequestPtr req);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_MCPADMINCONTROLLER_HPP */
