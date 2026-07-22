// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD__DISCORD_CLIENT_HPP
#define TGLOGGERD__DISCORD_CLIENT_HPP

#include <string>

namespace tgloggerd {

struct DiscordResponse {
	long		status = 0; /* HTTP status; 0 = transport error. */
	std::string	body;       /* response body (may hold a Discord error). */
	std::string	error;      /* transport error message when status == 0. */

	bool ok(void) const { return status >= 200 && status < 300; }
};

/* Escape `s` so it can be embedded inside a JSON string literal. */
std::string json_escape(const std::string &s);

/*
 * A minimal Discord webhook HTTP client over libcurl. Thread-safe: every call
 * uses its own easy handle, so it may be invoked concurrently from the thread
 * pool. Call global_init() once at startup (curl_global_init is not
 * thread-safe) before any concurrent use.
 */
class DiscordClient {
public:
	/* Initialize libcurl globally. Call once at startup, single-threaded. */
	static void global_init(void);

	/*
	 * POST `json_body` (Content-Type: application/json) to the webhook `url`.
	 * Retries a few times on HTTP 429, honoring the retry_after hint. Returns
	 * the last response, or a transport error (status == 0).
	 */
	DiscordResponse post_json(const std::string &url,
				  const std::string &json_body);

private:
	DiscordResponse post_once(const std::string &url,
				  const std::string &json_body);
};

} /* namespace tgloggerd */

#endif /* #ifndef TGLOGGERD__DISCORD_CLIENT_HPP */
