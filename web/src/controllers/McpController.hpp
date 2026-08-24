// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_MCPCONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_MCPCONTROLLER_HPP

#include <drogon/HttpController.h>

namespace tgweb::controllers {

/*
 * The MCP endpoint: the Streamable HTTP transport for the gwmcp server.
 *
 * Deliberately carries NO filters. Every other route here authenticates with a
 * session cookie and redirects to /login when it is missing -- useless to a
 * machine client, which cannot post a login form and would receive a 302 where
 * it expects JSON. This route authenticates with `Authorization: Bearer`
 * instead, checked in the handler, and answers 401 rather than redirecting.
 * That mirrors /files/<token>, the other filter-free route, which likewise
 * carries its own credential.
 *
 * The transport is intentionally minimal. A server that never initiates
 * messages may answer every POST with application/json and refuse GET, so there
 * is no SSE, no session store and no streaming here -- see src/gwmcp/README.md.
 */
class McpController : public drogon::HttpController<McpController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(McpController::post, "/mcp", drogon::Post);
	/* The spec requires the endpoint to answer GET and DELETE, even if only
	 * to decline them; both are handled rather than 404ing. */
	ADD_METHOD_TO(McpController::get, "/mcp", drogon::Get);
	ADD_METHOD_TO(McpController::del, "/mcp", drogon::Delete);
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr> post(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> get(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> del(drogon::HttpRequestPtr req);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_MCPCONTROLLER_HPP */
