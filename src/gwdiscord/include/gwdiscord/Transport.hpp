// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef GWDISCORD__TRANSPORT_HPP
#define GWDISCORD__TRANSPORT_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/*
 * The transport seam.
 *
 * Everything above this header (Gateway, Events, Rest) is pure protocol logic
 * and knows nothing about how bytes reach Discord. Exactly one translation
 * unit in this library implements these interfaces against a concrete network
 * stack -- today src/BeastTransport.cpp, using Boost.Beast. Swapping that
 * stack means writing one new .cpp and changing one line in CMakeLists.txt;
 * no protocol code moves. See README.md, "Replacing the WebSocket backend".
 *
 * These interfaces are deliberately small and BLOCKING. gwdiscord runs the
 * gateway on the caller's thread (see Gateway::run), so an implementation
 * needs no event loop, no coroutines and no callback plumbing.
 */
namespace gwdiscord {

enum class ReadStatus {
	Message,	/* `out` holds one complete application message. */
	Closed,		/* Peer closed; see WebSocket::peer_close(). */
	Error,		/* Transport failure; `err` is set. */
};

struct CloseInfo {
	uint16_t	code = 0;
	std::string	reason;
};

/*
 * A client WebSocket over TLS.
 *
 * THREADING CONTRACT -- an implementation MUST honour this:
 *   - read() may block indefinitely.
 *   - write() may be called from another thread WHILE read() is blocked.
 *     (gwdiscord's heartbeat thread does exactly this.) Implementations must
 *     serialize concurrent writes internally; they need not support two
 *     simultaneous readers or two simultaneous writers.
 *   - disconnect() may be called from another thread to tear the connection
 *     down and make a blocked read() return promptly. It must be safe to call
 *     more than once, and safe to call when never connected.
 */
class WebSocket {
public:
	virtual ~WebSocket(void) = default;

	/*
	 * Connect to wss://<host><target> and perform the WebSocket
	 * handshake. `host` is a bare hostname (no scheme, no port); `target`
	 * is an origin-form path such as "/?v=10&encoding=json". Returns false
	 * and fills `err` on failure.
	 */
	virtual bool connect(const std::string &host, const std::string &target,
			     std::string *err) = 0;

	/* Block until one complete message arrives, or the peer closes. */
	virtual ReadStatus read(std::string &out, std::string *err) = 0;

	/* Send one text message. Safe to call concurrently with read(). */
	virtual bool write(const std::string &payload, std::string *err) = 0;

	/*
	 * Send a Close frame carrying `code`. Discord requires close codes
	 * outside the 1000/1001 range to keep a session resumable, so an
	 * implementation MUST transmit arbitrary uint16_t codes verbatim
	 * rather than clamping them to a standard enumeration.
	 */
	virtual void close(uint16_t code, const std::string &reason) = 0;

	/* The peer's close code/reason; valid once read() returned Closed. */
	virtual CloseInfo peer_close(void) const = 0;

	/* Hard teardown; unblocks a read() in progress. Idempotent. */
	virtual void disconnect(void) = 0;
};

struct HttpResponse {
	long		status = 0;	/* HTTP status; 0 = transport error. */
	std::string	body;
	std::string	error;		/* set when status == 0 */

	bool ok(void) const { return status >= 200 && status < 300; }
};

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

/*
 * The minimum HTTPS the gateway lifecycle needs: GET /gateway/bot, to learn
 * the gateway URL and the session-start limits. Deliberately not a general
 * Discord REST client -- gwdiscord grows that only when discordd needs it.
 */
class HttpClient {
public:
	virtual ~HttpClient(void) = default;

	virtual HttpResponse get(const std::string &host,
				 const std::string &target,
				 const HttpHeaders &headers) = 0;
};

/*
 * A network stack, as a pair of factories. Gateway holds one of these and
 * makes a fresh WebSocket per connection attempt, so a reconnect never reuses
 * a torn-down handle.
 */
struct Transport {
	std::function<std::unique_ptr<WebSocket>(void)>	 ws;
	std::function<std::unique_ptr<HttpClient>(void)> http;

	bool valid(void) const { return ws && http; }
};

/*
 * The production backend (Boost.Beast). Declared here so callers never need to
 * include a Beast header; defined in the single TU that does.
 */
Transport beast_transport(void);

} /* namespace gwdiscord */

#endif /* #ifndef GWDISCORD__TRANSPORT_HPP */
