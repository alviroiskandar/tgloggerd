// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD__DISCORD_FORWARDER_HPP
#define TGLOGGERD__DISCORD_FORWARDER_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "helpers/log.h"
#include "DiscordClient.hpp"
#include "ThreadPool.hpp"
#include "TDLib.hpp" /* ForwardMessage */

namespace tgloggerd {

class DB;

/*
 * Mirrors live Telegram messages to Discord channels via incoming webhooks.
 *
 * forward() is called on the TDLib event thread and must stay cheap: it looks
 * the chat up in an in-memory cache and hands the HTTP POST to its own thread
 * pool, so the event loop never blocks on the network. The cache is loaded from
 * discord_webhooks at startup and refreshed periodically by a background
 * thread, so edits made in the web UI take effect without a daemon restart.
 * Forwarding failures are logged and never affect logging.
 */
class DiscordForwarder {
public:
	DiscordForwarder(DB *db, log_hd_t *l, size_t threads, size_t queue_cap,
			 int refresh_secs);
	~DiscordForwarder(void);

	DiscordForwarder(const DiscordForwarder &) = delete;
	DiscordForwarder &operator=(const DiscordForwarder &) = delete;

	/* Load the cache once, then start the periodic refresh thread. */
	void start(void);

	/* Stop the refresh thread and drain the HTTP pool. Idempotent. */
	void stop(void);

	/* Forward one live message. Called on the TDLib thread; returns fast. */
	void forward(const ForwardMessage &fm);

private:
	void reload(void);
	void refresh_loop(void);

	DB		*db_;
	log_hd_t	*l_;
	int		refresh_secs_;

	DiscordClient	client_;
	ThreadPool	pool_;

	std::mutex	cache_mtx_;
	std::unordered_map<int64_t, std::vector<std::string>> cache_;

	std::thread			refresh_thr_;
	std::mutex			wake_mtx_;
	std::condition_variable		wake_cv_;
	std::atomic<bool>		stop_{false};
	bool				started_ = false;
};

} /* namespace tgloggerd */

#endif /* #ifndef TGLOGGERD__DISCORD_FORWARDER_HPP */
