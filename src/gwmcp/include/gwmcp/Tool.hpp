// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef GWMCP__TOOL_HPP
#define GWMCP__TOOL_HPP

#include <functional>
#include <string>

#include "Json.hpp"

namespace gwmcp {

/*
 * One callable tool.
 *
 * This struct IS the extension point. gwmcp ships no tools of its own and
 * knows nothing about what any of them do -- a consumer describes its tools
 * and hands them over, which is what keeps this library free of Telegram,
 * Discord, SQL and tgloggerd.
 */
struct Tool {
	/* Unique identifier the client calls by; required. */
	std::string	name;

	/* Optional human-readable display name. */
	std::string	title;

	/*
	 * What the tool does, in prose. This is the primary thing a model
	 * reads when deciding whether to call it, so it is worth writing
	 * properly -- including what the tool will NOT return.
	 */
	std::string	description;

	/*
	 * JSON Schema for `arguments`, advertised verbatim by tools/list.
	 * Should be an object schema. gwmcp does not validate arguments
	 * against it -- see the note on handler below.
	 */
	Json		inputSchema;

	/*
	 * Optional JSON Schema for the result. Declaring one is a promise:
	 * the spec says a server MUST then produce conforming structured
	 * results, so leave it null unless the shape is genuinely fixed.
	 */
	Json		outputSchema;

	/*
	 * Advertised as annotations.readOnlyHint. A hint, not enforcement --
	 * the spec is explicit that clients must treat annotations from an
	 * untrusted server as untrusted.
	 */
	bool		readOnlyHint = true;

	/*
	 * The implementation. Receives the raw `arguments` object (an empty
	 * object when the caller sent none) and returns the result payload,
	 * which the server places in structuredContent.
	 *
	 * Throw ToolError for a failure the caller should see as a tool
	 * result; throw RpcError for a malformed call. Any other exception is
	 * caught and reported as a tool error rather than escaping into the
	 * transport -- one broken tool must not take down the connection.
	 *
	 * Handlers must validate their own arguments: gwmcp deliberately does
	 * not implement a JSON Schema validator, because a half-correct one is
	 * worse than none and the consumer knows its own types.
	 */
	std::function<Json(const Json &arguments)> handler;
};

} /* namespace gwmcp */

#endif /* #ifndef GWMCP__TOOL_HPP */
