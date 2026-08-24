<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org> -->

# gwmcp

A small [Model Context Protocol](https://modelcontextprotocol.io) server library:
JSON-RPC framing, the MCP lifecycle, and a tool registry.

It is a **self-contained project that happens to live in the tgloggerd repository**. It
has no dependency on tgloggerd — no SQL, no tgloggerd headers, no shared helpers — and no
knowledge of Telegram, Discord, or any other thing a tool might talk to. Consumers
register their own tools; gwmcp ships none.

## Rules for working in this directory

These exist so gwmcp can be lifted into its own git repository later without archaeology:

1. **A commit that touches `src/gwmcp/` must touch nothing else.** No changes to the web
   app, the root `CMakeLists.txt`, migrations, or the `Dockerfile` in the same commit.
2. **Prefix every such commit with `gwmcp:`.**
3. **Never `#include` anything from outside this directory** other than the C++ standard
   library and nlohmann/json.
4. **Add functionality only when a consumer actually needs it.** This is a library grown
   to fit its users, not a complete MCP implementation.

Licence is `GPL-2.0-or-later`, matching `src/gwdiscord` and, since the relicense,
tgloggerd itself. `COPYING` and `COPYING.GPLv3` sit here rather than only at the
repository root so that rule 1 above actually holds — the directory keeps its
terms when it is lifted out.

## Layout

```
include/gwmcp/       public headers
  Json.hpp           using Json = nlohmann::json
  Tool.hpp           the tool descriptor — the extension point
  ToolRegistry.hpp   name -> Tool
  Server.hpp         the protocol engine
  Errors.hpp         JSON-RPC codes, RpcError, ToolError
src/
  Server.cpp         lifecycle, dispatch, version negotiation
  ToolRegistry.cpp
  Protocol.{hpp,cpp} JSON-RPC envelope validation and response shapes (internal)
examples/
  mcp_selftest.cpp   drives the whole protocol with no I/O; the ctest target
  mcp_stdio.cpp      a complete stdio server; doubles as API documentation
```

## Design

### The transport seam

`Server` never touches a socket, a file descriptor, or an HTTP type. Its entire interface
is:

```cpp
std::optional<Json> Server::handle(const Json &message) const;
```

One JSON-RPC message in, zero-or-one out. `std::nullopt` means *that was a notification*
— which is precisely the distinction a transport needs:

| Transport | `nullopt` means | a value means |
|---|---|---|
| HTTP | `202 Accepted`, empty body | `200`, JSON body |
| stdio | write nothing | write one line |

Everything transport-shaped — HTTP method routing, `Origin` validation, headers,
authentication, newline framing, SSE — belongs to the caller. That is what lets the same
engine sit behind a Drogon controller and behind `examples/mcp_stdio.cpp` with no code in
common.

`examples/mcp_selftest.cpp` is the proof: it drives initialize, tools/list, tools/call and
every error path with **no I/O at all**. If that file ever needs a transport to compile,
the seam has leaked.

The isolation is checked, not asserted:

```
Protocol.cpp.o       drogon/tgweb/mysql symbols: 0
Server.cpp.o         drogon/tgweb/mysql symbols: 0
ToolRegistry.cpp.o   drogon/tgweb/mysql symbols: 0
```

### Why the JSON type is public

`gwdiscord` hides jsoncpp behind plain structs. gwmcp deliberately does the opposite and
puts `nlohmann::json` in its public API, because MCP is JSON to its core: a tool's
`inputSchema` *is* JSON Schema, its arguments are arbitrary caller-supplied JSON, and its
result is arbitrary JSON. Abstracting that away would mean reinventing a JSON value type
for no gain.

### The two error channels

MCP has two, and conflating them is a spec violation, so they are two C++ types:

| Throw | Becomes | For |
|---|---|---|
| `RpcError` | `{"error":{"code":…}}` | unknown method or tool, malformed params — *you called me wrong* |
| `ToolError` | `{"result":{…,"isError":true}}` | the call was fine, the work failed — *here is what went wrong* |

The distinction matters to a model: only the second is something it can read and retry
differently. Any other exception escaping a handler is caught and reported as a tool
error, so one broken tool cannot take down a connection serving other work.

### Deliberate omissions

- **No JSON Schema validation of arguments.** Handlers validate their own; a half-correct
  validator is worse than none, and the consumer knows its own types.
- **No batching.** Removed from the protocol in 2025-06-18; accepting it would mean
  guessing at semantics the current spec no longer defines. Rejected with `-32600`.
- **No pagination in `tools/list`.** `nextCursor` is omitted rather than faked.
- **`listChanged: false`.** The registry is immutable once the `Server` is constructed, so
  advertising notifications we will never send would be a lie a client might wait on.
- **No resources, no prompts, no logging capability.** Add them when something needs them.

## Adding a tool

```cpp
#include <gwmcp/Server.hpp>
#include <gwmcp/Errors.hpp>

gwmcp::ToolRegistry reg;

gwmcp::Tool t;
t.name        = "add";
t.title       = "Add";
t.description = "Add two numbers.";           /* a model reads this to decide */
t.inputSchema = { {"type","object"},
                  {"properties", {{"a", {{"type","number"}}},
                                  {"b", {{"type","number"}}}}},
                  {"required", {"a","b"}} };
t.handler = [](const gwmcp::Json &args) {
        if (!args.contains("a") || !args["a"].is_number())
                throw gwmcp::ToolError("\"a\" must be a number");
        return gwmcp::Json{ {"sum", args["a"].get<double>() +
                                    args["b"].get<double>()} };
};
reg.add(std::move(t));

gwmcp::ServerInfo info;
info.name = "my-server";
gwmcp::Server server(std::move(info), std::move(reg));
```

Populate the registry once at startup and then treat it as immutable: lookups happen on
request threads with no locking. `handle()` is `const` and holds no mutable state, so it
is safe to call concurrently **provided every handler is itself thread-safe**.

Write the `description` properly — including what the tool will *not* return. It is the
main thing a model reads when deciding whether to call it.

## Adding a transport

Implement two things around `Server`:

1. **Framing** — get one JSON-RPC message from the wire, hand it to `handle()` (or
   `handleRaw()` if you have bytes rather than a parsed object).
2. **The notification case** — when the result is `std::nullopt`, send whatever your
   transport's "acknowledged, no reply" is.

That is the whole contract. `examples/mcp_stdio.cpp` is ~40 lines of it.

For **Streamable HTTP** specifically, the spec requires one endpoint serving POST and GET,
but a server that never initiates messages may answer every POST with
`Content-Type: application/json`, return **405 on GET** (explicitly permitted: *"or else
return HTTP 405 Method Not Allowed, indicating that the server does not offer an SSE
stream"*), 405 on DELETE, and omit `Mcp-Session-Id` entirely — so no SSE and no session
store are needed. The transport must still: validate the `Origin` header (DNS-rebinding
defence), answer `400` to an unsupported `MCP-Protocol-Version`, assume `2025-03-26` when
that header is absent, and return `202` with an empty body for notifications.

## Building

Standalone:

```bash
cmake -S src/gwmcp -B build/gwmcp -DCMAKE_BUILD_TYPE=Release
cmake --build build/gwmcp -j"$(nproc)"
ctest --test-dir build/gwmcp
```

Embedded:

```cmake
add_subdirectory(path/to/gwmcp gwmcp)
target_link_libraries(your_target PRIVATE gwmcp)
```

Requires `nlohmann/json`. It is found automatically (CMake config, or the
`nlohmann-json3-dev` header path); set `GWMCP_NLOHMANN_INCLUDE_DIR` to point at a copy you
already vendor instead — the versions must match, since the type crosses the API boundary.

Examples build only when gwmcp is the top-level project; force them with
`-DGWMCP_BUILD_EXAMPLES=ON`.

## Trying it

```bash
printf '%s\n' \
 '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"c","version":"1"}}}' \
 '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
 '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
 '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"add","arguments":{"a":2,"b":40}}}' \
 | ./build/gwmcp/mcp_stdio
```

Four replies come back for five messages — the notification correctly produces none.
