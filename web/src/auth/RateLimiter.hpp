// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_AUTH_RATELIMITER_HPP
#define TGLOGGERD_WEB_AUTH_RATELIMITER_HPP

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace tgweb::auth {

/*
 * A small fixed-window failure counter to throttle brute-force login attempts,
 * keyed by an opaque string (the login controller keys on ip + username). It
 * counts failures only: a successful login clears the key. When the number of
 * failures within the window reaches the limit, allowed() returns false until
 * the window rolls over.
 *
 * State is in-process memory guarded by a mutex; it is not shared across
 * multiple web instances (acceptable for the single-node deployment).
 */
class RateLimiter {
public:
	RateLimiter(size_t maxFailures, std::chrono::seconds window)
		: maxFailures_(maxFailures), window_(window) {}

	/* True if another attempt for this key is currently permitted. */
	bool allowed(const std::string &key);

	/* Record a failed attempt for this key. */
	void recordFailure(const std::string &key);

	/* Clear a key's failure history (call on successful login). */
	void reset(const std::string &key);

private:
	struct Entry {
		size_t                                count;
		std::chrono::steady_clock::time_point windowStart;
	};

	size_t                                 maxFailures_;
	std::chrono::seconds                   window_;
	std::mutex                             mutex_;
	std::unordered_map<std::string, Entry> entries_;
};

} /* namespace tgweb::auth */

#endif /* TGLOGGERD_WEB_AUTH_RATELIMITER_HPP */
