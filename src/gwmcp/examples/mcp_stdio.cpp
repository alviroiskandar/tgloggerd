// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
/*
 * mcp_stdio -- a complete, working MCP server over the stdio transport.
 *
 * It exists for two reasons. It is runnable documentation: this is the entire
 * API a consumer touches. And it is the second transport, which is what proves
 * the seam is real -- the same Server drives an HTTP endpoint in the web app
 * and this newline loop, with no code shared between them and none of the
 * library changed.
 *
 * Point any MCP client at it:
 *     "command": "/path/to/mcp_stdio"
 */
#include <gwmcp/Errors.hpp>
#include <gwmcp/Server.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <string>

using namespace gwmcp;

int main(void)
{
	ToolRegistry reg;

	Tool now;
	now.name = "now";
	now.title = "Current time";
	now.description = "Return the server's current UTC time. Takes no "
			  "arguments.";
	now.inputSchema = Json{ { "type", "object" },
				{ "properties", Json::object() } };
	now.outputSchema = Json{
		{ "type", "object" },
		{ "properties",
		  Json{ { "utc", Json{ { "type", "string" } } },
			{ "unix", Json{ { "type", "integer" } } } } },
		{ "required", Json::array({ "utc", "unix" }) },
	};
	now.handler = [](const Json &) {
		const auto t = std::chrono::system_clock::to_time_t(
			std::chrono::system_clock::now());
		char buf[64];
		struct tm tm_utc;
		gmtime_r(&t, &tm_utc);
		strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
		return Json{ { "utc", std::string(buf) },
			     { "unix", (long long)t } };
	};
	reg.add(std::move(now));

	Tool add;
	add.name = "add";
	add.title = "Add";
	add.description = "Add two numbers.";
	add.inputSchema = Json{
		{ "type", "object" },
		{ "properties", Json{ { "a", Json{ { "type", "number" } } },
				      { "b", Json{ { "type", "number" } } } } },
		{ "required", Json::array({ "a", "b" }) },
	};
	add.handler = [](const Json &args) {
		/* Handlers validate their own arguments; see Tool.hpp. */
		if (!args.contains("a") || !args["a"].is_number() ||
		    !args.contains("b") || !args["b"].is_number())
			throw ToolError("both \"a\" and \"b\" must be numbers");
		return Json{ { "sum", args["a"].get<double>() +
					      args["b"].get<double>() } };
	};
	reg.add(std::move(add));

	ServerInfo info;
	info.name = "gwmcp-stdio-example";
	info.title = "gwmcp stdio example";
	info.version = "0.1.0";
	info.instructions = "An example server exposing a clock and a calculator.";

	const Server server(std::move(info), std::move(reg));

	/*
	 * The stdio framing rule, in full: one JSON message per line, no
	 * embedded newlines, and NOTHING may be written to stdout that is not
	 * a protocol message -- so diagnostics go to stderr.
	 */
	std::string line;
	while (std::getline(std::cin, line)) {
		if (line.empty())
			continue;

		auto reply = server.handleRaw(line);
		if (!reply)
			continue; /* a notification: write nothing */

		std::cout << *reply << "\n";
		std::cout.flush();
	}
	return 0;
}
