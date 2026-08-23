// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
/*
 * gw_tail -- connect to Discord and print messages as they arrive.
 *
 * Doubles as the library's usage documentation: this is the whole API surface
 * a consumer touches. Note what is absent -- no Boost header, no JSON type, no
 * transport detail.
 *
 *   DISCORD_BOT_TOKEN=... ./gw_tail [channel_id]
 *
 * With a channel id, only that channel is printed.
 */
#include <gwdiscord/Gateway.hpp>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace gwdiscord;

static Gateway *g_gw = nullptr;
static std::atomic<bool> g_signalled{false};

static void on_signal(int sig)
{
	(void)sig;
	g_signalled = true;
	if (g_gw)
		g_gw->stop(); /* safe from a signal-adjacent context */
}

static void print_message(const char *what, const Message &m,
			  Snowflake only_channel)
{
	if (only_channel && m.channel_id != only_channel)
		return;

	printf("\n=== %s ===\n", what);
	printf("  id         : %llu\n", (unsigned long long)m.id);
	printf("  channel    : %llu   guild: %llu\n",
	       (unsigned long long)m.channel_id,
	       (unsigned long long)m.guild_id);
	printf("  author     : %s (id=%llu bot=%s)\n", m.author.username.c_str(),
	       (unsigned long long)m.author.id, m.author.bot ? "yes" : "no");

	if (m.from_webhook()) {
		printf("  webhook_id : %llu  <- posted by a webhook\n",
		       (unsigned long long)m.webhook_id);
	}
	printf("  content    : \"%s\"%s\n", m.content.c_str(),
	       m.content.empty() ? "  <- empty: MESSAGE_CONTENT intent?" : "");

	if (m.is_reply()) {
		printf("  reply to   : %llu\n",
		       (unsigned long long)m.reference->message_id);
	}
	for (const auto &a : m.attachments) {
		printf("  attachment : %s (%llu bytes) %s\n",
		       a.filename.c_str(), (unsigned long long)a.size,
		       a.url.c_str());
	}
	fflush(stdout);
}

int main(int argc, char **argv)
{
	const char *tok = getenv("DISCORD_BOT_TOKEN");
	if (!tok || !*tok) {
		fprintf(stderr, "DISCORD_BOT_TOKEN is not set\n");
		return 1;
	}

	Snowflake only_channel = 0;
	if (argc > 1)
		only_channel = strtoull(argv[1], nullptr, 10);

	GatewayConfig cfg;
	cfg.token = tok;
	cfg.intents = intents::MESSAGE_LOGGING;

	LogSink log = [](LogLevel lvl, const std::string &msg) {
		fprintf(stderr, "[gw/%s] %s\n", log_level_name(lvl),
			msg.c_str());
	};

	Gateway gw(cfg, beast_transport(), log);
	g_gw = &gw;

	gw.on_ready([](const Ready &r) {
		printf("connected as %s; waiting for messages\n",
		       r.user.username.c_str());
		fflush(stdout);
	});
	gw.on_message_create([&](const Message &m) {
		print_message("MESSAGE_CREATE", m, only_channel);
	});
	gw.on_message_update([&](const Message &m) {
		print_message("MESSAGE_UPDATE", m, only_channel);
	});
	gw.on_message_delete([&](const MessageDelete &d) {
		if (only_channel && d.channel_id != only_channel)
			return;
		printf("\n=== MESSAGE_DELETE ===\n  id: %llu channel: %llu\n",
		       (unsigned long long)d.id,
		       (unsigned long long)d.channel_id);
		fflush(stdout);
	});

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	StopReason why = gw.run();
	g_gw = nullptr;

	switch (why) {
	case StopReason::Requested:
		printf("stopped on request\n");
		return 0;
	case StopReason::AuthFailed:
		fprintf(stderr, "authentication failed: bad bot token\n");
		return 2;
	case StopReason::FatalClose:
		fprintf(stderr, "stopped: Discord sent a non-retryable close code\n");
		return 3;
	case StopReason::Exhausted:
		fprintf(stderr, "stopped: too many failed attempts\n");
		return 4;
	}
	return 0;
}
