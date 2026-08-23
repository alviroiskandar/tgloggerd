<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org> -->

# gwdiscord

A small Discord bot library: the Gateway v10 client plus the minimum REST
needed to open a gateway connection.

It is a **self-contained project that happens to live in the tgloggerd
repository**. It has no dependency on tgloggerd — no SQL, no tgloggerd
headers, no shared helpers, no knowledge that Telegram exists. `src/discordd`
consumes it; gwdiscord knows nothing about `src/discordd`.

## Rules for working in this directory

These exist so gwdiscord can be lifted into its own git repository later
without archaeology:

1. **A commit that touches `src/gwdiscord/` must touch nothing else.** No
   changes to the root `CMakeLists.txt`, `docker-compose.yml`, migrations, or
   anything under `src/tgloggerd/` in the same commit.
2. **Prefix every such commit with `gwdiscord:`.**
3. **Never `#include` anything from outside this directory** other than the
   C++ standard library and the third-party libraries named in
   `CMakeLists.txt`.
4. **Add functionality only when `discordd` actually needs it.** This is a
   library grown to fit one consumer, not a general-purpose Discord SDK. There
   is no voice support, no sharding, no slash commands, no entity cache, and no
   message-sending API, because nothing needs them yet.

Licence is `GPL-2.0-or-later` — note the `-or-later`, which differs from
tgloggerd's `GPL-2.0-only`. That matters: it keeps the subtree compatible with
Apache-2.0 dependencies such as OpenSSL 3.x, which a GPL-2.0-only work is not.

## Layout

```
include/gwdiscord/     public headers — no third-party type appears in any of them
  Transport.hpp        the seam: WebSocket + HttpClient interfaces
  Gateway.hpp          the Discord client, intents, close-code policy
  Events.hpp           plain structs: Message, User, Attachment, ...
  Log.hpp              a log-sink callback the host provides
  Rest.hpp             GET /gateway/bot
  MockTransport.hpp    a non-network Transport, for tests
src/
  BeastTransport.cpp   *** the ONLY file that includes Boost ***
  MockTransport.cpp    the second Transport implementation
  Gateway.cpp          the protocol state machine — no networking at all
  EventParse.{hpp,cpp} JSON → structs (jsoncpp stays internal)
  Rest.cpp
examples/
  gw_tail.cpp          connect and print messages; doubles as API documentation
  gw_mock_test.cpp     drives the state machine over canned frames
```

## Design, and why

### The transport seam

`Transport.hpp` declares two abstract interfaces — `WebSocket` and
`HttpClient` — and a `Transport` struct holding a factory for each. Everything
above that line is pure protocol logic that never learns how bytes travel.

The rule this enforces is mechanical, not aspirational. It is checked:

```
BeastTransport.cpp.o     boost symbols: 1257
EventParse.cpp.o         boost symbols: 0
Gateway.cpp.o            boost symbols: 0
MockTransport.cpp.o      boost symbols: 0
Rest.cpp.o               boost symbols: 0
gw_tail.cpp.o            boost symbols: 0
```

Boost's include directory is `PRIVATE` in `CMakeLists.txt`, so a consumer
linking `gwdiscord` does not compile against Boost, does not need Boost
headers installed, and does not link a Boost binary — Beast and Asio are
header-only, so there is no Boost runtime dependency either.

The interfaces are **blocking**, deliberately. `Gateway::run()` occupies the
caller's thread, so a backend needs no event loop, no coroutines and no
callback plumbing — which is what makes them cheap to reimplement. The one
concurrency requirement is documented in `Transport.hpp`: `write()` and
`disconnect()` may be called while `read()` is blocked on another thread.

`MockTransport` is not only a testing convenience. It is the proof the seam is
real: a second implementation that drives the whole protocol with zero Boost
symbols. If it ever stops compiling, the abstraction has leaked.

### Threading

One blocking `run()` on the caller's thread, and exactly one internal thread,
for heartbeats. Handlers are invoked on `run()`'s thread, so they never race
each other and need no locking. `stop()` is safe from any thread — including
from inside a handler and from a signal handler — and unblocks `run()` by
tearing the socket down underneath it.

### Snowflakes

Discord serialises every id as a JSON **string**, because the values do not
survive a `double`. gwdiscord parses them to `uint64_t` once, at the edge, so
callers never handle stringly-typed ids. `snowflake_created_ms()` recovers the
creation timestamp without a lookup.

### Close-code policy

This is the part worth being careful about. Codes **4004, 4010, 4011, 4012,
4013 and 4014** are permanent misconfigurations. Reconnecting on them is not
merely useless — every attempt spends one of the 1000 IDENTIFYs allowed per 24
hours, and exhausting that budget causes Discord to **reset the bot token**.

The most likely way to hit this is mundane: forget to enable MESSAGE_CONTENT
in the Developer Portal, get closed with 4014 on every IDENTIFY, and a naive
5-second retry loop burns the daily budget in about 83 minutes. So
`is_fatal_close_code()` is consulted before any reconnect, and `run()` returns
`StopReason::FatalClose` instead of retrying. `gw_mock_test` covers this
directly, because provoking it against the live service is exactly the thing
that costs you the token.

Everything else backs off with full jitter and resumes. On a missed heartbeat
ACK the connection is closed with code **4000** rather than 1000/1001 —
Discord keeps a session resumable only for non-standard close codes.

### What the library does not do

No voice, no sharding (one connection covers up to 2500 guilds), no slash
commands or interactions, no entity cache, no message sending. `MESSAGE_DELETE`
carries only ids and never content, so a consumer that wants to render a
deletion must persist the message at ingest. Attachment CDN URLs are
HMAC-signed and expire; fetch the bytes promptly.

## Replacing the WebSocket backend

The Boost dependency is one file and one CMake variable. To move to another
stack — libwebsockets, IXWebSocket, drogon's `WebSocketClient`, a rebuilt
libcurl with `--enable-websockets`, or a hand-rolled RFC 6455 client:

1. **Write `src/YourTransport.cpp`.** Implement `gwdiscord::WebSocket` and
   `gwdiscord::HttpClient`, and expose one factory function returning a
   `Transport`. Copy `BeastTransport.cpp` as the shape to fill in; it is about
   250 lines, most of it TLS setup and error mapping.

2. **Honour the three contracts** documented in `Transport.hpp`, all of which
   the Gateway depends on:
   - `write()` is callable while `read()` blocks on another thread, and
     concurrent writes are serialised internally.
   - `disconnect()` makes a blocked `read()` return promptly, is idempotent,
     and is safe when never connected.
   - `close()` transmits **arbitrary** `uint16_t` codes verbatim. A backend
     that clamps close codes to a standard enumeration cannot keep a Discord
     session resumable, and one that hides the peer's close code entirely
     cannot implement the fatal-code policy above — that is a real limitation
     of some libraries, so check it before choosing one.

3. **Point the build at it:**
   ```
   cmake -S src/gwdiscord -B build/gwdiscord \
         -DGWDISCORD_TRANSPORT_SRC=src/YourTransport.cpp
   ```
   or edit the `GWDISCORD_TRANSPORT_SRC` default in `CMakeLists.txt`, and
   adjust the dependency lookup near it. Nothing else changes: no protocol
   code, no headers, no consumer.

4. **Verify the seam held.** Rebuild and confirm the symbol count above —
   your backend's object file should be the only one carrying its library's
   symbols. Then run `gw_mock_test`, which exercises the protocol without any
   transport at all.

Declare the new factory in `Transport.hpp` next to `beast_transport()` if it
is meant to be selectable at runtime; otherwise keeping it internal is fine.

## Building

Standalone:

```bash
cmake -S src/gwdiscord -B build/gwdiscord -DCMAKE_BUILD_TYPE=Release
cmake --build build/gwdiscord -j"$(nproc)"
ctest --test-dir build/gwdiscord
```

Embedded, from `src/discordd/CMakeLists.txt`:

```cmake
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../gwdiscord gwdiscord)
target_link_libraries(discordd PRIVATE gwdiscord)
```

Examples build only when gwdiscord is the top-level project; force them with
`-DGWDISCORD_BUILD_EXAMPLES=ON`.

Requires `libboost-dev` (headers only), `libjsoncpp-dev`, OpenSSL and pthreads.

## Trying it

```bash
DISCORD_BOT_TOKEN=... ./build/gwdiscord/gw_tail [channel_id]
```

Prints `MESSAGE_CREATE`, `MESSAGE_UPDATE` and `MESSAGE_DELETE` as they arrive,
optionally filtered to one channel. The bot needs the **MESSAGE_CONTENT**
privileged intent enabled in the Developer Portal — without it `content`,
`embeds` and `attachments` all arrive empty, and requesting the intent without
having enabled it is closed with 4014.
