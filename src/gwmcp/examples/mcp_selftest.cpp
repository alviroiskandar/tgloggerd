// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
/*
 * mcp_selftest -- drive the whole protocol with no I/O whatsoever.
 *
 * Every message here is a C++ object handed straight to Server::handle(). There
 * is no socket, no HTTP, no stdio: that is the point. If this file ever needs a
 * transport to compile or run, the seam in Server.hpp has leaked.
 *
 * It also covers the paths that are awkward to reach from a live client -- a
 * tool that throws, a malformed envelope, an unsupported protocol version.
 */
#include <gwmcp/Errors.hpp>
#include <gwmcp/Server.hpp>

#include <cstdio>
#include <string>

using namespace gwmcp;

static int g_fail;

#define CHECK(cond, what)                                                     \
	do {                                                                  \
		if (!(cond)) {                                                \
			printf("  FAIL: %s\n", (what));                       \
			g_fail++;                                             \
		} else {                                                      \
			printf("  ok:   %s\n", (what));                       \
		}                                                             \
	} while (0)

static Json req(int id, const std::string &method, Json params = Json::object())
{
	return Json{ { "jsonrpc", "2.0" },
		     { "id", id },
		     { "method", method },
		     { "params", std::move(params) } };
}

static Server buildServer(void)
{
	ToolRegistry reg;

	Tool echo;
	echo.name = "echo";
	echo.title = "Echo";
	echo.description = "Return the text it was given.";
	echo.inputSchema = Json{
		{ "type", "object" },
		{ "properties",
		  Json{ { "text", Json{ { "type", "string" } } } } },
		{ "required", Json::array({ "text" }) },
	};
	echo.handler = [](const Json &a) {
		if (!a.contains("text") || !a["text"].is_string())
			throw ToolError("text is required and must be a string");
		return Json{ { "echoed", a["text"] } };
	};
	reg.add(std::move(echo));

	Tool boom;
	boom.name = "boom";
	boom.description = "Always throws, to exercise the error path.";
	boom.handler = [](const Json &) -> Json {
		throw std::runtime_error("kaboom");
	};
	reg.add(std::move(boom));

	ServerInfo info;
	info.name = "gwmcp-selftest";
	info.version = "0.1.0";
	info.instructions = "A test server.";
	return Server(std::move(info), std::move(reg));
}

static void testLifecycle(const Server &s)
{
	printf("test: lifecycle\n");

	auto r = s.handle(req(1, "initialize",
			      Json{ { "protocolVersion", PROTOCOL_LATEST },
				    { "capabilities", Json::object() },
				    { "clientInfo",
				      Json{ { "name", "selftest" },
					    { "version", "1" } } } }));
	CHECK(r.has_value(), "initialize produced a response");
	CHECK(r && (*r)["result"]["protocolVersion"] == PROTOCOL_LATEST,
	      "protocol version echoed back");
	CHECK(r && (*r)["result"]["capabilities"].contains("tools"),
	      "tools capability advertised");
	CHECK(r && (*r)["result"]["serverInfo"]["name"] == "gwmcp-selftest",
	      "serverInfo carried through");
	CHECK(r && (*r)["result"].contains("instructions"),
	      "instructions included when set");

	/* An unknown version must NOT be an error -- it is a negotiation. */
	auto old = s.handle(req(2, "initialize",
				Json{ { "protocolVersion", "1999-01-01" } }));
	CHECK(old && !(*old).contains("error"),
	      "unsupported version negotiates rather than failing");
	CHECK(old && (*old)["result"]["protocolVersion"] == PROTOCOL_LATEST,
	      "unsupported version answered with our latest");

	/* The initialized notification must produce NO reply at all. */
	auto note = s.handle(Json{ { "jsonrpc", "2.0" },
				   { "method", "notifications/initialized" } });
	CHECK(!note.has_value(), "notification produces no response");

	auto unknownNote = s.handle(Json{ { "jsonrpc", "2.0" },
					  { "method", "notifications/whatever" } });
	CHECK(!unknownNote.has_value(), "unknown notification is ignored");

	auto ping = s.handle(req(3, "ping"));
	CHECK(ping && ping->contains("result"), "ping answered");
}

static void testTools(const Server &s)
{
	printf("test: tools\n");

	auto list = s.handle(req(10, "tools/list"));
	CHECK(list && (*list)["result"]["tools"].size() == 2,
	      "tools/list returns both tools");
	CHECK(list && (*list)["result"]["tools"][0]["name"] == "boom",
	      "tools are name-ordered (boom before echo)");
	CHECK(list && (*list)["result"]["tools"][0].contains("inputSchema"),
	      "a tool with no schema still advertises one");
	CHECK(list &&
		      (*list)["result"]["tools"][1]["annotations"]["readOnlyHint"] ==
			      true,
	      "readOnlyHint advertised");
	CHECK(list && !(*list)["result"].contains("nextCursor"),
	      "no fake pagination cursor");

	auto call = s.handle(req(11, "tools/call",
				 Json{ { "name", "echo" },
				       { "arguments",
					 Json{ { "text", "hi" } } } }));
	CHECK(call && (*call)["result"]["structuredContent"]["echoed"] == "hi",
	      "tools/call returns structuredContent");
	CHECK(call && (*call)["result"]["isError"] == false,
	      "successful call is not an error");
	/* The text block must carry the same payload, serialised. */
	CHECK(call &&
		      Json::parse((*call)["result"]["content"][0]["text"]
					  .get<std::string>())["echoed"] == "hi",
	      "content[0] mirrors structuredContent as text");
}

static void testErrorChannels(const Server &s)
{
	printf("test: the two error channels stay separate\n");

	/* A tool that throws ToolError -> a RESULT with isError, not an error. */
	auto bad = s.handle(req(20, "tools/call", Json{ { "name", "echo" },
							{ "arguments",
							  Json::object() } }));
	CHECK(bad && bad->contains("result"),
	      "tool failure is a result, not a protocol error");
	CHECK(bad && (*bad)["result"]["isError"] == true,
	      "tool failure sets isError");
	CHECK(bad && !(*bad)["result"].contains("structuredContent"),
	      "a failed tool returns no structuredContent");

	/* An unexpected exception is contained the same way. */
	auto kab = s.handle(req(21, "tools/call", Json{ { "name", "boom" } }));
	CHECK(kab && kab->contains("result") &&
		      (*kab)["result"]["isError"] == true,
	      "an unexpected throw becomes a tool error, not a crash");

	/* Unknown tool -> PROTOCOL error. */
	auto nt = s.handle(req(22, "tools/call", Json{ { "name", "nope" } }));
	CHECK(nt && nt->contains("error"), "unknown tool is a protocol error");
	CHECK(nt && (*nt)["error"]["code"] == rpc::INVALID_PARAMS,
	      "unknown tool uses -32602");

	auto nm = s.handle(req(23, "no/such/method"));
	CHECK(nm && (*nm)["error"]["code"] == rpc::METHOD_NOT_FOUND,
	      "unknown method uses -32601");

	auto badEnv = s.handle(Json{ { "id", 24 }, { "method", "ping" } });
	CHECK(badEnv && (*badEnv)["error"]["code"] == rpc::INVALID_REQUEST,
	      "missing jsonrpc version is -32600");

	auto badArgs = s.handle(req(25, "tools/call",
				    Json{ { "name", "echo" },
					  { "arguments", "not-an-object" } }));
	CHECK(badArgs && badArgs->contains("error"),
	      "non-object arguments is a protocol error");
}

static void testRaw(const Server &s)
{
	printf("test: raw text entry point\n");

	auto ok = s.handleRaw("{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"ping\"}");
	CHECK(ok.has_value(), "valid raw message answered");

	auto broken = s.handleRaw("{not json");
	CHECK(broken.has_value(), "parse failure still answers");
	CHECK(broken && Json::parse(*broken)["error"]["code"] == rpc::PARSE_ERROR,
	      "parse failure uses -32700");
	CHECK(broken && Json::parse(*broken)["id"].is_null(),
	      "unparseable message answers with a null id");

	auto batch = s.handleRaw("[{\"jsonrpc\":\"2.0\",\"method\":\"ping\"}]");
	CHECK(batch && Json::parse(*batch)["error"]["code"] == rpc::INVALID_REQUEST,
	      "batch is rejected (removed in 2025-06-18)");

	auto note = s.handleRaw(
		"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
	CHECK(!note.has_value(), "raw notification produces no output");
}

static void testRegistry(void)
{
	printf("test: registry\n");

	ToolRegistry reg;
	Tool t;
	t.name = "a";
	t.handler = [](const Json &) { return Json::object(); };
	CHECK(reg.add(t), "first add succeeds");
	CHECK(!reg.add(t), "duplicate name is rejected");

	Tool noName;
	noName.handler = [](const Json &) { return Json::object(); };
	CHECK(!reg.add(noName), "empty name is rejected");

	Tool noHandler;
	noHandler.name = "b";
	CHECK(!reg.add(noHandler), "missing handler is rejected");

	CHECK(reg.size() == 1, "registry holds exactly the valid tool");
	CHECK(reg.find("a") != nullptr, "find locates it");
	CHECK(reg.find("zzz") == nullptr, "find returns nullptr when absent");
}

int main(void)
{
	const Server s = buildServer();

	testLifecycle(s);
	testTools(s);
	testErrorChannels(s);
	testRaw(s);
	testRegistry();

	if (g_fail) {
		printf("\n%d check(s) FAILED\n", g_fail);
		return 1;
	}
	printf("\nall checks passed\n");
	return 0;
}
