// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include <gwmcp/Server.hpp>

#include <gwmcp/Errors.hpp>

#include "Protocol.hpp"

#include <exception>
#include <utility>

namespace gwmcp {

bool isSupportedProtocol(const std::string &version)
{
	return version == PROTOCOL_LATEST || version == PROTOCOL_PREVIOUS;
}

Server::Server(ServerInfo info, ToolRegistry registry)
	: info_(std::move(info)), registry_(std::move(registry))
{
}

Json Server::initializeResult(const Json &params) const
{
	/*
	 * Version negotiation. The spec: echo the client's version when we
	 * support it, otherwise answer with our latest and let the client
	 * decide whether to continue. Note this is NOT an error case -- a
	 * mismatch is a negotiation outcome, not a failure.
	 */
	std::string requested;
	if (params.contains("protocolVersion") &&
	    params["protocolVersion"].is_string())
		requested = params["protocolVersion"].get<std::string>();

	const std::string agreed =
		isSupportedProtocol(requested) ? requested : PROTOCOL_LATEST;

	Json result;
	result["protocolVersion"] = agreed;

	/*
	 * Only "tools" is advertised. listChanged is false because the
	 * registry is immutable once the server is constructed, so promising
	 * notifications we will never send would be a lie a client might wait
	 * on.
	 */
	result["capabilities"] = Json{
		{ "tools", Json{ { "listChanged", false } } },
	};

	Json si;
	si["name"] = info_.name;
	si["version"] = info_.version;
	if (!info_.title.empty())
		si["title"] = info_.title;
	result["serverInfo"] = std::move(si);

	if (!info_.instructions.empty())
		result["instructions"] = info_.instructions;

	return result;
}

Json Server::callTool(const Json &params) const
{
	if (!params.contains("name") || !params["name"].is_string())
		throw RpcError(rpc::INVALID_PARAMS,
			       "tools/call requires a string \"name\"");

	const std::string name = params["name"].get<std::string>();
	const Tool *tool = registry_.find(name);
	if (!tool)
		throw RpcError(rpc::INVALID_PARAMS, "Unknown tool: " + name);

	Json args = Json::object();
	if (params.contains("arguments")) {
		const Json &a = params["arguments"];
		if (a.is_object())
			args = a;
		else if (!a.is_null())
			throw RpcError(rpc::INVALID_PARAMS,
				       "\"arguments\" must be an object");
	}

	Json payload;
	try {
		payload = tool->handler(args);
	} catch (const RpcError &) {
		/* The handler judged the CALL malformed; let it propagate as a
		 * protocol error rather than dressing it as a result. */
		throw;
	} catch (const ToolError &e) {
		return Json{
			{ "content",
			  Json::array({ Json{ { "type", "text" },
					      { "text", e.message() } } }) },
			{ "isError", true },
		};
	} catch (const std::exception &e) {
		/*
		 * An unexpected throw is still the tool's failure, not a
		 * protocol violation -- and it must not escape into the
		 * transport, where it would kill a connection that is serving
		 * other work.
		 */
		return Json{
			{ "content",
			  Json::array({ Json{ { "type", "text" },
					      { "text", std::string("Tool failed: ") +
							       e.what() } } }) },
			{ "isError", true },
		};
	}

	/*
	 * Return the payload twice, deliberately: structuredContent is the
	 * real answer, and content[0] carries the same thing serialised as
	 * text because the spec asks for that for backwards compatibility with
	 * clients that predate structured content.
	 */
	Json result;
	result["content"] = Json::array(
		{ Json{ { "type", "text" }, { "text", payload.dump() } } });
	result["structuredContent"] = std::move(payload);
	result["isError"] = false;
	return result;
}

std::optional<Json> Server::handle(const Json &message) const
{
	const Json id = messageId(message);
	const bool notification = isNotification(message);

	std::string err;
	if (!validateEnvelope(message, err)) {
		if (notification)
			return std::nullopt; /* nothing to reply to */
		return makeError(id, rpc::INVALID_REQUEST, err);
	}

	const std::string method = message["method"].get<std::string>();
	const Json params = paramsObject(message);

	try {
		/*
		 * Notifications first: they produce no reply at all, including
		 * no error reply, so an unknown one is silently accepted. That
		 * is deliberate -- the spec expects unknown notifications to be
		 * ignored rather than rejected.
		 */
		if (notification) {
			/* notifications/initialized, notifications/cancelled,
			 * anything else: acknowledged by doing nothing. */
			return std::nullopt;
		}

		if (method == "initialize")
			return makeResult(id, initializeResult(params));

		if (method == "ping")
			return makeResult(id, Json::object());

		if (method == "tools/list") {
			/* No pagination: the tool count is small and fixed, so
			 * nextCursor is omitted entirely rather than faked. */
			return makeResult(
				id, Json{ { "tools", registry_.listJson() } });
		}

		if (method == "tools/call")
			return makeResult(id, callTool(params));

		return makeError(id, rpc::METHOD_NOT_FOUND,
				 "Unknown method: " + method);
	} catch (const RpcError &e) {
		if (e.hasData())
			return makeError(id, e.code(), e.message(), e.data());
		return makeError(id, e.code(), e.message());
	} catch (const std::exception &e) {
		return makeError(id, rpc::INTERNAL_ERROR, e.what());
	}
}

std::optional<std::string> Server::handleRaw(const std::string &text) const
{
	Json message;
	try {
		message = Json::parse(text);
	} catch (const std::exception &e) {
		/* A parse failure has no recoverable id, hence the null. */
		return makeError(Json(nullptr), rpc::PARSE_ERROR,
				 std::string("Parse error: ") + e.what())
			.dump();
	}

	/*
	 * A batch is a JSON array. Not supported: the 2025-06-18 spec removed
	 * batching, and accepting it would mean guessing at semantics the
	 * current protocol no longer defines.
	 */
	if (message.is_array())
		return makeError(Json(nullptr), rpc::INVALID_REQUEST,
				 "Batch requests are not supported")
			.dump();

	auto resp = handle(message);
	if (!resp)
		return std::nullopt;
	return resp->dump();
}

} /* namespace gwmcp */
