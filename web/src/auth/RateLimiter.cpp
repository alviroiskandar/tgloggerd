// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "auth/RateLimiter.hpp"

namespace tgweb::auth {

bool RateLimiter::allowed(const std::string &key)
{
	auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> lck(mutex_);

	auto it = entries_.find(key);
	if (it == entries_.end())
		return true;

	/* Expired window: forget the key and allow. */
	if (now - it->second.windowStart >= window_) {
		entries_.erase(it);
		return true;
	}

	return it->second.count < maxFailures_;
}

void RateLimiter::recordFailure(const std::string &key)
{
	auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> lck(mutex_);

	auto it = entries_.find(key);
	if (it == entries_.end() || now - it->second.windowStart >= window_) {
		entries_[key] = Entry{1, now};
		return;
	}

	it->second.count++;
}

void RateLimiter::reset(const std::string &key)
{
	std::lock_guard<std::mutex> lck(mutex_);
	entries_.erase(key);
}

} /* namespace tgweb::auth */
