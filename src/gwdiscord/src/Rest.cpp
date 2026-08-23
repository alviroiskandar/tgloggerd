// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include <gwdiscord/Rest.hpp>

#include "EventParse.hpp"

namespace gwdiscord {

const char *USER_AGENT =
	"DiscordBot (https://git.gnuweeb.net/GNUWeeb/tgloggerd, 0.1.0)";

std::string host_from_ws_url(const std::string &url)
{
	std::string s = url;

	if (s.rfind("wss://", 0) == 0)
		s.erase(0, 6);
	else if (s.rfind("ws://", 0) == 0)
		s.erase(0, 5);

	/* Drop any path and any :port -- connect() wants a bare hostname. */
	const size_t slash = s.find('/');
	if (slash != std::string::npos)
		s.erase(slash);
	const size_t colon = s.find(':');
	if (colon != std::string::npos)
		s.erase(colon);
	return s;
}

bool fetch_gateway_info(HttpClient &http, const std::string &token,
			GatewayInfo &out, std::string *err)
{
	const HttpHeaders headers = {
		{"Authorization", "Bot " + token},
		{"User-Agent", USER_AGENT},
		{"Accept", "application/json"},
	};

	HttpResponse res =
		http.get(API_HOST, std::string(API_BASE) + "/gateway/bot",
			 headers);

	if (res.status == 0) {
		if (err)
			*err = "GET /gateway/bot: " + res.error;
		return false;
	}
	if (!res.ok()) {
		if (err) {
			*err = "GET /gateway/bot: HTTP " +
			       std::to_string(res.status);
			/*
			 * 401 here means the token is bad; the caller should
			 * not bother opening a gateway connection with it.
			 */
			if (!res.body.empty())
				*err += " " + res.body;
		}
		return false;
	}

	Json::Value v;
	if (!parse_json(res.body, v, err))
		return false;

	out.url = json_str(v, "url");
	if (v.isMember("shards") && v["shards"].isIntegral())
		out.shards = v["shards"].asInt();

	if (v.isMember("session_start_limit") &&
	    v["session_start_limit"].isObject()) {
		const Json::Value &l = v["session_start_limit"];
		if (l.isMember("remaining") && l["remaining"].isIntegral())
			out.session_remaining = l["remaining"].asInt();
		if (l.isMember("total") && l["total"].isIntegral())
			out.session_total = l["total"].asInt();
		if (l.isMember("reset_after") && l["reset_after"].isIntegral())
			out.reset_after_ms = l["reset_after"].asInt();
		if (l.isMember("max_concurrency") &&
		    l["max_concurrency"].isIntegral())
			out.max_concurrency = l["max_concurrency"].asInt();
	}

	if (out.url.empty()) {
		if (err)
			*err = "GET /gateway/bot: response had no url";
		return false;
	}
	return true;
}

} /* namespace gwdiscord */
