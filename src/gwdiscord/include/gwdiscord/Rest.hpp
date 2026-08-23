// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef GWDISCORD__REST_HPP
#define GWDISCORD__REST_HPP

#include <string>

#include "Transport.hpp"

namespace gwdiscord {

constexpr const char *API_HOST = "discord.com";
constexpr const char *API_BASE = "/api/v10";

/*
 * Discord requires a descriptive User-Agent on REST calls and may reject
 * requests without one. Override via GatewayConfig-adjacent call sites if this
 * library is vendored elsewhere.
 */
extern const char *USER_AGENT;

/* GET /gateway/bot -- the documented way to obtain a gateway URL. */
struct GatewayInfo {
	std::string	url;			/* e.g. wss://gateway.discord.gg */
	int		shards = 1;
	int		max_concurrency = 1;
	/* Remaining IDENTIFYs in the current 24h window; -1 if unknown. */
	int		session_remaining = -1;
	int		session_total = -1;
	int		reset_after_ms = -1;
};

/*
 * Fetch GET /gateway/bot with `token`. Returns false and sets `err` on
 * failure; the caller may then fall back to the default gateway host.
 */
bool fetch_gateway_info(HttpClient &http, const std::string &token,
			GatewayInfo &out, std::string *err);

/*
 * Split "wss://gateway.discord.gg" (or a bare host) into a hostname suitable
 * for WebSocket::connect(). Returns the input unchanged if no scheme present.
 */
std::string host_from_ws_url(const std::string &url);

} /* namespace gwdiscord */

#endif /* #ifndef GWDISCORD__REST_HPP */
