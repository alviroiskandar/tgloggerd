// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef GWDISCORD__GATEWAY_HPP
#define GWDISCORD__GATEWAY_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "Events.hpp"
#include "Log.hpp"
#include "Transport.hpp"

namespace gwdiscord {

/*
 * Gateway intents. MESSAGE_CONTENT is PRIVILEGED: it must be enabled in the
 * application's Developer Portal page. Without it a bot still receives
 * MESSAGE_CREATE, but `content`, `embeds`, `attachments` and `components` all
 * arrive empty -- and IDENTIFY is rejected outright with close code 4014 if
 * the intent is requested but not granted.
 */
namespace intents {
constexpr uint64_t GUILDS		= 1ULL << 0;
constexpr uint64_t GUILD_MESSAGES	= 1ULL << 9;
constexpr uint64_t MESSAGE_CONTENT	= 1ULL << 15;

/* What a message-logging bridge needs: 1 | 512 | 32768 == 33281. */
constexpr uint64_t MESSAGE_LOGGING =
	GUILDS | GUILD_MESSAGES | MESSAGE_CONTENT;
} /* namespace intents */

/*
 * Close codes Discord will never accept a retry for. Reconnecting on these is
 * not merely useless, it is actively harmful: each attempt spends one of the
 * 1000 IDENTIFYs allowed per 24 hours, and exhausting that budget causes
 * Discord to reset the bot token. Gateway::run() therefore STOPS on these.
 */
bool is_fatal_close_code(uint16_t code);

struct GatewayConfig {
	std::string	token;
	uint64_t	intents = intents::MESSAGE_LOGGING;

	/*
	 * Reconnect backoff, in seconds: the delay doubles from `backoff_min`
	 * up to `backoff_max`, with jitter, and resets once a connection
	 * reaches READY.
	 */
	int		backoff_min = 1;
	int		backoff_max = 64;

	/*
	 * Give up after this many consecutive failed attempts; 0 = never.
	 * Fatal close codes stop immediately regardless of this.
	 */
	int		max_attempts = 0;

	/* Query GET /gateway/bot instead of assuming the default host. */
	bool		use_rest_discovery = true;
};

enum class StopReason {
	Requested,	/* stop() was called. */
	FatalClose,	/* Discord sent a close code we must not retry. */
	AuthFailed,	/* 4004: the token is wrong. */
	Exhausted,	/* max_attempts reached. */
};

/*
 * A Discord Gateway v10 client.
 *
 * Lifecycle: HELLO -> IDENTIFY -> READY, heartbeating throughout, RESUMEing
 * after a recoverable drop and re-IDENTIFYing when the session is invalidated.
 *
 * Threading: run() blocks on the CALLER's thread and does all event dispatch
 * there, so handlers need no locking against each other. Exactly one internal
 * thread is spawned, for heartbeats. stop() is safe to call from any thread,
 * including from inside a handler.
 */
class Gateway {
public:
	Gateway(GatewayConfig cfg, Transport tp, LogSink log);
	~Gateway(void);

	Gateway(const Gateway &) = delete;
	Gateway &operator=(const Gateway &) = delete;

	/*
	 * Handlers run on run()'s thread. Set them before calling run();
	 * they are not synchronised against a running gateway.
	 */
	void on_ready(std::function<void(const Ready &)> cb);
	void on_message_create(std::function<void(const Message &)> cb);
	void on_message_update(std::function<void(const Message &)> cb);
	void on_message_delete(std::function<void(const MessageDelete &)> cb);

	/* Block, reconnecting as needed, until stop() or a fatal condition. */
	StopReason run(void);

	/* Ask run() to return. Safe from any thread; safe before run(). */
	void stop(void);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} /* namespace gwdiscord */

#endif /* #ifndef GWDISCORD__GATEWAY_HPP */
