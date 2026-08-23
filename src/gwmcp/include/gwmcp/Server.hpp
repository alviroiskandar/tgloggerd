// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef GWMCP__SERVER_HPP
#define GWMCP__SERVER_HPP

#include <optional>
#include <string>

#include "Json.hpp"
#include "ToolRegistry.hpp"

namespace gwmcp {

/*
 * Protocol versions this library speaks, newest first. Version negotiation
 * echoes the client's version when it appears here, and otherwise answers with
 * LATEST -- which the spec permits, leaving the client to decide whether it can
 * live with that.
 */
constexpr const char *PROTOCOL_LATEST = "2025-06-18";
constexpr const char *PROTOCOL_PREVIOUS = "2025-03-26";

bool isSupportedProtocol(const std::string &version);

struct ServerInfo {
	std::string	name = "gwmcp";
	std::string	title;
	std::string	version = "0.1.0";

	/*
	 * Optional free text handed to the client at initialize. Good place to
	 * say what the server is for and what it will refuse to do, since a
	 * model sees it before any tool description.
	 */
	std::string	instructions;
};

/*
 * The MCP protocol engine.
 *
 * THE SEAM: this class never touches a socket, a file descriptor, or an HTTP
 * type. Its entire interface is "one JSON-RPC message in, zero-or-one out".
 * Everything transport-shaped -- HTTP method routing, Origin checks, headers,
 * authentication, newline framing, SSE -- belongs to the caller.
 *
 * That is what lets the same engine sit behind a Drogon controller and behind
 * a stdio loop with no code in common, and it is why examples/mcp_selftest.cpp
 * can drive the whole protocol with no I/O at all.
 *
 * Threading: handle() is const and touches no mutable state, so it is safe to
 * call concurrently PROVIDED every registered tool handler is itself
 * thread-safe. gwmcp adds no locking of its own.
 */
class Server {
public:
	Server(ServerInfo info, ToolRegistry registry);

	/*
	 * Handle one parsed JSON-RPC message.
	 *
	 * Returns the response to send back, or std::nullopt when the message
	 * was a NOTIFICATION and therefore has no reply. That distinction is
	 * not cosmetic: over HTTP it is exactly what selects "202 Accepted with
	 * no body" versus "200 with a JSON body", and over stdio it selects
	 * "write nothing".
	 *
	 * Never throws. A malformed message becomes a JSON-RPC error response;
	 * a throwing tool becomes an isError result.
	 */
	std::optional<Json> handle(const Json &message) const;

	/*
	 * Convenience for transports that hold raw bytes: parses, dispatches,
	 * and serialises. A parse failure becomes a -32700 error response, as
	 * the spec requires, rather than an exception. Returns std::nullopt on
	 * the notification path, same as handle().
	 */
	std::optional<std::string> handleRaw(const std::string &text) const;

	const ToolRegistry &registry(void) const { return registry_; }
	const ServerInfo &info(void) const { return info_; }

private:
	Json initializeResult(const Json &params) const;
	Json callTool(const Json &params) const;

	ServerInfo	info_;
	ToolRegistry	registry_;
};

/* Response builders, exposed so a transport can emit protocol errors of its
 * own (e.g. a 400 for a bad MCP-Protocol-Version header) in the same shape. */
Json makeError(const Json &id, int code, const std::string &message);
Json makeError(const Json &id, int code, const std::string &message,
	       const Json &data);
Json makeResult(const Json &id, Json result);

} /* namespace gwmcp */

#endif /* #ifndef GWMCP__SERVER_HPP */
