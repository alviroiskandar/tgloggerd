// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/McpController.hpp"

#include "dao/Mcp.hpp"
#include "mcp/Token.hpp"
#include "mcp/telegram/Tools.hpp"

#include <gwmcp/Errors.hpp>
#include <gwmcp/Server.hpp>

#include <trantor/net/EventLoopThreadPool.h>

#include <exception>
#include <memory>
#include <mutex>
#include <string>

namespace tgweb::controllers {

namespace {

/*
 * The server and the threads its tools run on, built once on first use.
 *
 * Tool handlers use the BLOCKING database API, which must not run on an HTTP
 * event loop -- a slow query would stall every other request sharing that loop.
 * So they are dispatched onto a small pool of their own, where blocking is the
 * expected behaviour and the only thing delayed is another MCP call.
 */
struct McpRuntime {
	std::unique_ptr<gwmcp::Server> server;
	std::unique_ptr<trantor::EventLoopThreadPool> pool;
};

McpRuntime &runtime(void)
{
	static McpRuntime rt;
	static std::once_flag once;

	std::call_once(once, [] {
		gwmcp::ToolRegistry reg;
		mcp::telegram::registerTools(reg,
					     drogon::app().getDbClient("ro"));
		/* Discord tools will register here as a second call. */

		gwmcp::ServerInfo info;
		info.name = "tgloggerd";
		info.title = "tgloggerd archive";
		info.version = "0.1.0";
		info.instructions =
			"Read-only access to a Telegram message archive. Only "
			"groups an administrator has explicitly exposed are "
			"readable; private groups and direct messages are not "
			"available through any tool. Call telegram_list_groups "
			"first to see what can be queried.";

		rt.server = std::make_unique<gwmcp::Server>(std::move(info),
							    std::move(reg));

		const char *n = getenv("MCP_THREADS");
		size_t threads = n ? (size_t)atoi(n) : 2;
		if (threads < 1)
			threads = 1;
		if (threads > 16)
			threads = 16;
		rt.pool = std::make_unique<trantor::EventLoopThreadPool>(
			threads, "mcp");
		rt.pool->start();
	});
	return rt;
}

drogon::HttpResponsePtr jsonBody(const std::string &body,
				 drogon::HttpStatusCode code = drogon::k200OK)
{
	auto resp = drogon::HttpResponse::newHttpResponse();
	resp->setStatusCode(code);
	resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
	resp->setBody(body);
	return resp;
}

drogon::HttpResponsePtr rpcError(int code, const std::string &msg,
				 drogon::HttpStatusCode status)
{
	return jsonBody(gwmcp::makeError(nlohmann::json(nullptr), code, msg).dump(),
			status);
}

/*
 * Origin policy.
 *
 * The spec requires servers to validate Origin as a DNS-rebinding defence. That
 * threat is a browser being tricked into talking to a server it should not
 * reach -- and the guidance is written for the common case of an MCP server
 * bound to localhost with no authentication at all.
 *
 * This endpoint is neither. Two things already stand in the way of a hostile
 * page, and both are stronger than an Origin check:
 *
 *   1. A bearer token is required. An attacker's page does not have one.
 *   2. No CORS headers are ever sent. A cross-origin page therefore cannot READ
 *      a response even if it manages to send a request, and the
 *      application/json content type forces a preflight this server does not
 *      answer, so it cannot usually send one either.
 *
 * So the default is to ALLOW. The previous default -- refuse anything carrying
 * an Origin unless an allowlist was configured -- rejected every legitimate
 * browser-based and Electron client with a 403 raised BEFORE authentication was
 * looked at, which surfaced to the user as an unexplained sign-in failure. That
 * is a bad trade: it broke real clients to defend against an attack the token
 * already prevents.
 *
 * Setting MCP_ALLOWED_ORIGINS restores strict checking for operators who want
 * it, and is worth doing if this endpoint is ever exposed without a token.
 */
bool originAllowed(const drogon::HttpRequestPtr &req)
{
	const std::string origin = req->getHeader("origin");
	if (origin.empty())
		return true; /* not a browser: nothing to forge */

	const char *allowed = getenv("MCP_ALLOWED_ORIGINS");
	if (!allowed || !*allowed)
		return true; /* no allowlist configured: see above */

	const std::string list = allowed;
	size_t p = 0;
	while (p <= list.size()) {
		const size_t c = list.find(',', p);
		const size_t e = (c == std::string::npos) ? list.size() : c;
		std::string item = list.substr(p, e - p);
		while (!item.empty() && item.front() == ' ')
			item.erase(item.begin());
		while (!item.empty() && (item.back() == ' ' || item.back() == '\r'))
			item.pop_back();
		if (item == "*" || item == origin)
			return true;
		if (c == std::string::npos)
			break;
		p = c + 1;
	}
	return false;
}

/*
 * NO WWW-Authenticate HEADER ON 401 -- deliberately, and it must stay that way.
 *
 * The MCP authorization spec makes OAuth 2.1 OPTIONAL ("Authorization is
 * OPTIONAL for MCP implementations"), but it makes the header MEAN something
 * specific: a 401 carrying WWW-Authenticate is the signal that the server is an
 * OAuth protected resource, and clients "MUST parse WWW-Authenticate headers
 * and respond appropriately" -- by fetching
 * /.well-known/oauth-protected-resource, discovering an authorization server,
 * and attempting Dynamic Client Registration.
 *
 * This server implements none of that. Sending the header therefore advertised
 * a flow that does not exist: Claude Desktop probed the discovery endpoints,
 * got 404s, and failed with "Couldn't register with tgloggerd's sign-in
 * service... add an OAuth Client ID" -- never reaching the token it had been
 * given. Omitting the header leaves the 401 as a plain "no valid credential",
 * which is what it is.
 *
 * Adding OAuth later means adding the discovery endpoints and the header
 * together, never the header alone.
 */

/*
 * The presented credential: header first, then query string.
 *
 * The header is the right way and is preferred whenever present. The query
 * string exists because a number of MCP clients accept only a URL and offer no
 * way to set a header, and without it those clients cannot connect at all.
 *
 * It is genuinely weaker, and the difference is worth stating: a query string
 * is recorded in reverse-proxy and CDN access logs, kept in browser history,
 * and leaked in the Referer header of any outbound link -- none of which
 * happens to a header. Mitigations are that tokens are per-client, named,
 * individually revocable, and stored only as a hash. The practical advice, in
 * web/docs/mcp.md, is to mint a SEPARATE token for query-string use so it can
 * be revoked without disturbing header-based clients.
 */
std::string credential(const drogon::HttpRequestPtr &req)
{
	const std::string h = mcp::token::fromAuthorizationHeader(
		req->getHeader("authorization"));
	if (!h.empty())
		return h;

	/* Drogon parses the query string regardless of the body's content
	 * type, so this works on a JSON POST. */
	return req->getParameter(mcp::token::QUERY_PARAM);
}

/*
 * The negotiated protocol version, per the spec: an absent header means the
 * client predates the header, so assume 2025-03-26; an unsupported one is a
 * 400 rather than a negotiation, because by this point negotiation is over.
 */
bool protocolVersionOk(const drogon::HttpRequestPtr &req, std::string &err)
{
	const std::string v = req->getHeader("mcp-protocol-version");
	if (v.empty())
		return true;
	if (gwmcp::isSupportedProtocol(v))
		return true;
	err = "Unsupported MCP-Protocol-Version: " + v;
	return false;
}

} /* namespace */

drogon::Task<drogon::HttpResponsePtr>
McpController::post(drogon::HttpRequestPtr req)
{
	if (!originAllowed(req))
		co_return rpcError(gwmcp::rpc::INVALID_REQUEST,
				   "Origin not allowed", drogon::k403Forbidden);

	std::string verr;
	if (!protocolVersionOk(req, verr))
		co_return rpcError(gwmcp::rpc::INVALID_REQUEST, verr,
				   drogon::k400BadRequest);

	/* ---- authenticate ---- */
	const std::string bearer = credential(req);
	if (bearer.empty() || !mcp::token::looksLikeToken(bearer)) {
		auto resp = rpcError(
			gwmcp::rpc::INVALID_REQUEST,
			"Missing or malformed token. Send "
			"\"Authorization: Bearer <token>\", or append "
			"?key=<token> if your client cannot set headers.",
			drogon::k401Unauthorized);
		co_return resp;
	}

	auto appDb = drogon::app().getDbClient("app");
	std::optional<dao::mcp::TokenOwner> owner;
	try {
		owner = co_await dao::mcp::resolveToken(
			appDb, mcp::token::digest(bearer));
	} catch (const std::exception &e) {
		co_return rpcError(gwmcp::rpc::INTERNAL_ERROR,
				   std::string("auth backend error: ") + e.what(),
				   drogon::k500InternalServerError);
	}
	if (!owner) {
		co_return rpcError(gwmcp::rpc::INVALID_REQUEST,
				   "Invalid or revoked token",
				   drogon::k401Unauthorized);
	}

	/* Best-effort; a failed stamp must not fail the request. */
	try {
		co_await dao::mcp::touchToken(appDb, owner->tokenId);
	} catch (const std::exception &) {
	}

	/* ---- dispatch ---- */
	const std::string body(req->getBody());
	if (body.empty()) {
		/*
		 * An empty POST is a reachability probe, not an error worth a
		 * 4xx -- and the status matters more than it looks.
		 *
		 * Claude's connector sends exactly this before doing anything
		 * else. Answering 400 made it conclude the endpoint needed
		 * authorization it had not satisfied, so it fell into OAuth
		 * discovery, found no metadata endpoints, and reported that it
		 * could not register with a "sign-in service" that does not
		 * exist -- while holding a valid token the whole time.
		 *
		 * 200 with a JSON-RPC error object is the conventional
		 * JSON-RPC-over-HTTP answer anyway: the transport succeeded,
		 * the payload was unusable. It is also already what a
		 * malformed-but-non-empty body gets, since handleRaw() turns a
		 * parse failure into a -32700 returned at 200. Treating empty
		 * as a fourth kind of bad payload just makes the two agree.
		 */
		co_return jsonBody(
			gwmcp::makeError(nlohmann::json(nullptr),
					 gwmcp::rpc::PARSE_ERROR,
					 "Empty request body")
				.dump());
	}

	McpRuntime &rt = runtime();
	const gwmcp::Server *server = rt.server.get();

	/*
	 * Hop to an MCP thread so the blocking queries inside the tools cannot
	 * stall an HTTP loop, and come back with the answer.
	 */
	std::optional<std::string> reply;
	try {
		reply = co_await drogon::queueInLoopCoro<std::optional<std::string>>(
			rt.pool->getNextLoop(),
			[server, body] { return server->handleRaw(body); });
	} catch (const std::exception &e) {
		co_return rpcError(gwmcp::rpc::INTERNAL_ERROR, e.what(),
				   drogon::k500InternalServerError);
	}

	/*
	 * No reply means the client sent a notification or a response. The spec
	 * is specific: 202 Accepted with NO body.
	 */
	if (!reply) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::k202Accepted);
		resp->setBody("");
		co_return resp;
	}
	co_return jsonBody(*reply);
}

drogon::Task<drogon::HttpResponsePtr>
McpController::get(drogon::HttpRequestPtr req)
{
	(void)req;
	/*
	 * A GET opens the server-to-client SSE stream. This server never
	 * initiates messages, so the spec's own alternative applies: return 405
	 * to say the endpoint offers no stream. Clients treat that as "POST
	 * only", not as an error.
	 */
	auto resp = drogon::HttpResponse::newHttpResponse();
	resp->setStatusCode(drogon::k405MethodNotAllowed);
	resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
	resp->addHeader("Allow", "POST");
	resp->setBody("This MCP endpoint does not offer an SSE stream.\n");
	co_return resp;
}

drogon::Task<drogon::HttpResponsePtr>
McpController::del(drogon::HttpRequestPtr req)
{
	(void)req;
	/* Sessions are not used, so there is nothing for a client to end. */
	auto resp = drogon::HttpResponse::newHttpResponse();
	resp->setStatusCode(drogon::k405MethodNotAllowed);
	resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
	resp->addHeader("Allow", "POST");
	resp->setBody("This MCP endpoint is stateless; there is no session to "
		      "terminate.\n");
	co_return resp;
}

} /* namespace tgweb::controllers */
