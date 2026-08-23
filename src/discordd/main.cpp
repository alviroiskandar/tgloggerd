// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
/*
 * discordd -- the Discord daemon.
 *
 * Logs Discord messages into the tgloggerd database and forwards the ones on a
 * configured route to a Telegram chat. Configuration comes entirely from the
 * environment; see .env.example.
 */
#include "DiscordD.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static discordd::DiscordD *g_app = nullptr;

static void on_signal(int sig)
{
	(void)sig;
	if (g_app)
		g_app->stop();
}

static const char *env_or(const char *name, const char *dflt)
{
	const char *v = getenv(name);
	return (v && *v) ? v : dflt;
}

static bool env_required(const char *name, std::string &out)
{
	const char *v = getenv(name);
	if (!v || !*v) {
		fprintf(stderr, "%s is not set\n", name);
		return false;
	}
	out = v;
	return true;
}

static int parse_log_level(const char *s)
{
	const std::string v = s ? s : "info";
	if (v == "error")
		return 0;
	if (v == "warn" || v == "warning")
		return 1;
	if (v == "debug")
		return 3;
	return 2; /* info */
}

int main(void)
{
	discordd::Config cfg;
	std::string api_id;

	if (!env_required("DISCORD_BOT_TOKEN", cfg.discord_bot_token))
		return 1;
	/*
	 * TDLib needs an api_id/api_hash pair even for a bot session, so
	 * discordd reuses the daemon's rather than asking for a second one.
	 */
	if (!env_required("TG_API_ID", api_id))
		return 1;
	if (!env_required("TG_API_HASH", cfg.api_hash))
		return 1;

	cfg.api_id = atoi(api_id.c_str());
	cfg.data_dir = env_or("DISCORDD_DATA_DIR", "data/users/discordd");
	cfg.log_level = parse_log_level(getenv("DISCORDD_LOG_LEVEL"));
	cfg.route_refresh_secs = atoi(env_or("DISCORDD_ROUTE_REFRESH_SECS", "30"));
	if (cfg.route_refresh_secs < 5)
		cfg.route_refresh_secs = 5;

	cfg.db.host = env_or("TG_DB_HOST", "127.0.0.1");
	cfg.db.port = (uint16_t)atoi(env_or("TG_DB_PORT", "3306"));
	cfg.db.pool_size = (size_t)atoi(env_or("DISCORDD_DB_CONNS", "4"));
	if (!env_required("TG_DB_USER", cfg.db.user))
		return 1;
	if (!env_required("TG_DB_PASSWORD", cfg.db.password))
		return 1;
	if (!env_required("TG_DB_NAME", cfg.db.database))
		return 1;

	discordd::DiscordD app(std::move(cfg));
	g_app = &app;

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGTERM, &sa, nullptr);

	const int rc = app.run();
	g_app = nullptr;
	return rc;
}
