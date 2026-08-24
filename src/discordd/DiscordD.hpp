// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef DISCORDD__DISCORDD_HPP
#define DISCORDD__DISCORDD_HPP

#include <mysql/ConnectionPool.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace discordd {

struct Config {
	/* Discord side. */
	std::string	discord_bot_token;

	/* Telegram side: TDLib needs these even for a bot session. */
	int32_t		api_id = 0;
	std::string	api_hash;
	std::string	data_dir;	/* parent of the per-bot session dirs */

	mysql::Config	db;

	/* How often the route table is re-read, in seconds. */
	int		route_refresh_secs = 30;

	int		log_level = 2;	/* 0=error 1=warn 2=info 3=debug */
};

/*
 * The Discord -> Telegram bridge.
 *
 * Receives messages through gwdiscord, logs every one that belongs to a
 * configured route, and forwards it to that route's Telegram chat through
 * that route's bot. Edits and deletes follow the message.
 */
class DiscordD {
public:
	explicit DiscordD(Config cfg);
	~DiscordD(void);

	DiscordD(const DiscordD &) = delete;
	DiscordD &operator=(const DiscordD &) = delete;

	/* Blocks until stop() or a fatal Discord condition. 0 on clean exit. */
	int run(void);

	/* Safe from any thread, including a signal handler. */
	void stop(void);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} /* namespace discordd */

#endif /* #ifndef DISCORDD__DISCORDD_HPP */
