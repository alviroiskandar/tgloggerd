// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
/*
 * gw_mock_test -- drive the Gateway state machine over the mock transport.
 *
 * No network, no Boost. Two things are being demonstrated:
 *
 *   1. The protocol layer works against ANY implementation of the transport
 *      interfaces, which is what makes the Boost backend replaceable.
 *   2. A fatal close code stops the client instead of reconnecting. That path
 *      cannot be tested against the live service, because provoking it spends
 *      the 1000-IDENTIFY-per-24h budget and can get the bot token reset.
 */
#include <gwdiscord/Gateway.hpp>
#include <gwdiscord/MockTransport.hpp>

#include <cstdio>
#include <string>

using namespace gwdiscord;

static int g_failures;

#define CHECK(cond, what)                                                     \
	do {                                                                  \
		if (!(cond)) {                                                \
			printf("  FAIL: %s\n", (what));                       \
			g_failures++;                                         \
		} else {                                                      \
			printf("  ok:   %s\n", (what));                       \
		}                                                             \
	} while (0)

static const char *HELLO = R"({"op":10,"d":{"heartbeat_interval":45000}})";

static const char *READY = R"({"op":0,"s":1,"t":"READY","d":{
	"session_id":"sess-abc",
	"resume_gateway_url":"wss://resume.example.invalid",
	"user":{"id":"865960934626951168","username":"testbot","bot":true}}})";

/* A webhook-authored reply carrying an attachment: every field a bridge uses. */
static const char *MSG = R"({"op":0,"s":2,"t":"MESSAGE_CREATE","d":{
	"id":"1541191219416399922",
	"channel_id":"1541156106343161896",
	"guild_id":"845302963739033611",
	"webhook_id":"1541156222617649172",
	"content":"hello from the mock",
	"timestamp":"2026-08-24T02:03:04.000000+00:00",
	"author":{"id":"1541156222617649172","username":"Telegram Bridge","bot":true},
	"message_reference":{"message_id":"1541191199321366609",
	                     "channel_id":"1541156106343161896"},
	"attachments":[{"id":"999","filename":"cat.png","size":12345,
	                "content_type":"image/png",
	                "url":"https://cdn.discordapp.com/a/cat.png?ex=1&is=2&hm=3",
	                "width":800,"height":600}]}})";

static LogSink quiet_log(void)
{
	return [](LogLevel, const std::string &) {};
}

/* Happy path: HELLO -> IDENTIFY -> READY -> MESSAGE_CREATE. */
static void test_happy_path(void)
{
	printf("test: happy path\n");

	auto script = std::make_shared<MockScript>();
	script->frames = {HELLO, READY, MSG};
	auto rec = std::make_shared<MockRecord>();

	GatewayConfig cfg;
	cfg.token = "test-token";
	cfg.intents = intents::MESSAGE_LOGGING;

	Gateway gw(cfg, mock_transport(script, rec), quiet_log());

	bool got_ready = false;
	Message seen;
	bool got_msg = false;

	gw.on_ready([&](const Ready &r) {
		got_ready = (r.session_id == "sess-abc" &&
			     r.user.username == "testbot");
	});
	gw.on_message_create([&](const Message &m) {
		seen = m;
		got_msg = true;
		gw.stop(); /* stopping from a handler must be safe */
	});

	StopReason why = gw.run();

	CHECK(why == StopReason::Requested, "run() stopped on request");
	CHECK(got_ready, "READY parsed (session id + user)");
	CHECK(got_msg, "MESSAGE_CREATE dispatched");

	CHECK(seen.id == 1541191219416399922ULL, "snowflake parsed from string");
	CHECK(seen.channel_id == 1541156106343161896ULL, "channel_id parsed");
	CHECK(seen.content == "hello from the mock", "content parsed");
	CHECK(seen.from_webhook(), "webhook_id detected (loop suppression)");
	CHECK(seen.webhook_id == 1541156222617649172ULL, "webhook_id value");
	CHECK(seen.is_reply(), "reply detected");
	CHECK(seen.reference && seen.reference->message_id ==
				       1541191199321366609ULL,
	      "reply target id parsed");
	CHECK(seen.attachments.size() == 1, "attachment parsed");
	CHECK(!seen.attachments.empty() &&
		      seen.attachments[0].filename == "cat.png",
	      "attachment filename");
	CHECK(!seen.attachments.empty() && seen.attachments[0].size == 12345,
	      "attachment size");

	/* The IDENTIFY frame must carry the privileged intent bit. */
	auto sent = rec->sent_copy();
	bool identified = false;
	for (const auto &s : sent) {
		if (s.find("\"op\":2") != std::string::npos &&
		    s.find("33281") != std::string::npos)
			identified = true;
	}
	CHECK(!sent.empty(), "client sent at least one frame");
	CHECK(identified, "IDENTIFY sent with intents=33281");
}

/*
 * A fatal close code must terminate, NOT reconnect. Getting this wrong is how
 * a bot loses its token: 4014 fires immediately on every IDENTIFY, so a naive
 * retry loop exhausts the daily budget within the hour.
 */
static void test_fatal_close_does_not_reconnect(void)
{
	printf("test: fatal close code 4014 stops instead of reconnecting\n");

	auto script = std::make_shared<MockScript>();
	script->frames = {HELLO};
	script->close_when_done = true;
	script->close_code = 4014;
	script->close_reason = "Disallowed intent(s)";
	auto rec = std::make_shared<MockRecord>();

	GatewayConfig cfg;
	cfg.token = "test-token";
	cfg.backoff_min = 1;

	Gateway gw(cfg, mock_transport(script, rec), quiet_log());
	StopReason why = gw.run();

	CHECK(why == StopReason::FatalClose, "run() reports FatalClose");
	CHECK(rec->connect_attempts == 1,
	      "connected exactly once (no retry storm)");
}

static void test_auth_failure(void)
{
	printf("test: close code 4004 reports AuthFailed\n");

	auto script = std::make_shared<MockScript>();
	script->frames = {HELLO};
	script->close_when_done = true;
	script->close_code = 4004;
	auto rec = std::make_shared<MockRecord>();

	GatewayConfig cfg;
	cfg.token = "bad-token";

	Gateway gw(cfg, mock_transport(script, rec), quiet_log());
	CHECK(gw.run() == StopReason::AuthFailed, "run() reports AuthFailed");
	CHECK(rec->connect_attempts == 1, "no retry on a bad token");
}

/* A recoverable close must retry, and must stop when told to. */
static void test_recoverable_close_retries(void)
{
	printf("test: recoverable close retries, then honours max_attempts\n");

	auto script = std::make_shared<MockScript>();
	script->fail_connect = true;
	auto rec = std::make_shared<MockRecord>();

	GatewayConfig cfg;
	cfg.token = "test-token";
	cfg.backoff_min = 1; /* full-jitter sleep is < 1s */
	cfg.backoff_max = 1;
	cfg.max_attempts = 3;

	Gateway gw(cfg, mock_transport(script, rec), quiet_log());
	CHECK(gw.run() == StopReason::Exhausted, "run() reports Exhausted");
	CHECK(rec->connect_attempts == 3, "retried exactly max_attempts times");
}

int main(void)
{
	test_happy_path();
	test_fatal_close_does_not_reconnect();
	test_auth_failure();
	test_recoverable_close_retries();

	if (g_failures) {
		printf("\n%d check(s) FAILED\n", g_failures);
		return 1;
	}
	printf("\nall checks passed\n");
	return 0;
}
