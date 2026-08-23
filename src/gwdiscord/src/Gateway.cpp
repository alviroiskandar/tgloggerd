// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
/*
 * Discord Gateway v10 state machine.
 *
 * Contains no networking code whatsoever: every byte in or out goes through
 * the gwdiscord::WebSocket / HttpClient interfaces, so this file compiles
 * unchanged no matter which stack implements them.
 */
#include <gwdiscord/Gateway.hpp>
#include <gwdiscord/Rest.hpp>

#include "EventParse.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>

namespace gwdiscord {

/* Gateway opcodes we act on. */
enum : int {
	OP_DISPATCH		= 0,
	OP_HEARTBEAT		= 1,
	OP_IDENTIFY		= 2,
	OP_RESUME		= 6,
	OP_RECONNECT		= 7,
	OP_INVALID_SESSION	= 9,
	OP_HELLO		= 10,
	OP_HEARTBEAT_ACK	= 11,
};

constexpr const char *DEFAULT_GATEWAY_HOST = "gateway.discord.gg";
constexpr const char *GATEWAY_TARGET = "/?v=10&encoding=json";

/*
 * Close codes that must never be retried. 4004 (bad token), 4010 (bad shard),
 * 4011 (sharding required), 4012 (bad API version), 4013 (bad intents) and
 * 4014 (disallowed -- i.e. a privileged intent that is not enabled in the
 * Developer Portal) are all permanent misconfigurations. Retrying them burns
 * the 1000-IDENTIFY-per-24h budget and ends in Discord resetting the token.
 */
bool is_fatal_close_code(uint16_t code)
{
	switch (code) {
	case 4004: /* authentication failed */
	case 4010: /* invalid shard */
	case 4011: /* sharding required */
	case 4012: /* invalid API version */
	case 4013: /* invalid intent(s) */
	case 4014: /* disallowed intent(s) */
		return true;
	default:
		return false;
	}
}

const char *log_level_name(LogLevel lvl)
{
	switch (lvl) {
	case LogLevel::Error:	return "error";
	case LogLevel::Warn:	return "warn";
	case LogLevel::Info:	return "info";
	case LogLevel::Debug:	return "debug";
	}
	return "?";
}

namespace {

/* What one connection attempt decided the run loop should do next. */
enum class Next {
	Retry,	/* transient: back off and reconnect */
	Stop,	/* terminal */
};

struct Outcome {
	Next		next = Next::Retry;
	StopReason	reason = StopReason::Requested;
	bool		made_progress = false; /* reached READY/RESUMED */
};

} /* anonymous namespace */

struct Gateway::Impl {
	GatewayConfig	cfg;
	Transport	tp;
	LogSink		log_sink;

	std::function<void(const Ready &)>		cb_ready;
	std::function<void(const Message &)>		cb_msg_create;
	std::function<void(const Message &)>		cb_msg_update;
	std::function<void(const MessageDelete &)>	cb_msg_delete;

	/* Run-loop control. */
	std::atomic<bool>	stopping{false};
	std::mutex		ws_mtx;
	std::shared_ptr<WebSocket> ws;	/* current connection, for stop() */
	std::mutex		sleep_mtx;
	std::condition_variable	sleep_cv;

	/* Session state that survives a reconnect. */
	std::string	session_id;
	std::string	resume_url;

	/* Per-connection heartbeat state. */
	std::thread		hb_thr;
	std::atomic<bool>	hb_stop{false};
	std::atomic<int64_t>	seq{-1};
	std::atomic<bool>	ack_pending{false};
	std::mutex		hb_mtx;
	std::condition_variable	hb_cv;

	std::mt19937	rng{std::random_device{}()};

	void log(LogLevel lvl, const std::string &msg) const
	{
		if (log_sink)
			log_sink(lvl, msg);
	}

	double jitter01(void)
	{
		return std::uniform_real_distribution<double>(0.0, 1.0)(rng);
	}

	/* Interruptible sleep; false if we were asked to stop. */
	bool nap(int ms)
	{
		std::unique_lock<std::mutex> lk(sleep_mtx);
		sleep_cv.wait_for(lk, std::chrono::milliseconds(ms),
				  [this] { return stopping.load(); });
		return !stopping.load();
	}

	bool hb_nap(int64_t ms)
	{
		std::unique_lock<std::mutex> lk(hb_mtx);
		hb_cv.wait_for(lk, std::chrono::milliseconds(ms),
			       [this] { return hb_stop.load(); });
		return !hb_stop.load();
	}

	std::string heartbeat_frame(void)
	{
		Json::Value h;
		h["op"] = OP_HEARTBEAT;
		if (seq.load() < 0)
			h["d"] = Json::Value(Json::nullValue);
		else
			h["d"] = (Json::Int64)seq.load();
		return dump_json(h);
	}

	void start_heartbeat(std::shared_ptr<WebSocket> sock, int64_t interval_ms)
	{
		hb_stop = false;
		ack_pending = false;
		hb_thr = std::thread([this, sock, interval_ms] {
			/*
			 * The docs require the FIRST heartbeat to be delayed by
			 * interval * random(0,1), so a fleet of bots
			 * reconnecting together does not thunder.
			 */
			if (!hb_nap((int64_t)(interval_ms * jitter01())))
				return;

			while (!hb_stop.load()) {
				if (ack_pending.load()) {
					/*
					 * No ACK since the previous beat: the
					 * connection is a zombie. Close with a
					 * NON-1000 code so the session stays
					 * resumable, then let run_once() see
					 * the drop and resume.
					 */
					log(LogLevel::Warn,
					    "heartbeat not ACKed; closing to force a resume");
					sock->close(4000, "heartbeat ack timeout");
					sock->disconnect();
					return;
				}

				std::string err;
				ack_pending = true;
				if (!sock->write(heartbeat_frame(), &err)) {
					log(LogLevel::Warn,
					    "heartbeat write failed: " + err);
					sock->disconnect();
					return;
				}
				if (!hb_nap(interval_ms))
					return;
			}
		});
	}

	void stop_heartbeat(void)
	{
		{
			std::lock_guard<std::mutex> lk(hb_mtx);
			hb_stop = true;
		}
		hb_cv.notify_all();
		if (hb_thr.joinable())
			hb_thr.join();
	}

	std::string identify_frame(void) const
	{
		Json::Value v;
		v["op"] = OP_IDENTIFY;
		v["d"]["token"] = cfg.token;
		v["d"]["intents"] = (Json::Int64)cfg.intents;
		v["d"]["properties"]["os"] = "linux";
		v["d"]["properties"]["browser"] = "gwdiscord";
		v["d"]["properties"]["device"] = "gwdiscord";
		return dump_json(v);
	}

	std::string resume_frame(void) const
	{
		Json::Value v;
		v["op"] = OP_RESUME;
		v["d"]["token"] = cfg.token;
		v["d"]["session_id"] = session_id;
		/* NOTE: RESUME's field is "seq", not "s". */
		v["d"]["seq"] = (Json::Int64)(seq.load() < 0 ? 0 : seq.load());
		return dump_json(v);
	}

	/* Resolve the host to connect to for this attempt. */
	std::string pick_host(bool resuming)
	{
		if (resuming && !resume_url.empty())
			return host_from_ws_url(resume_url);

		if (cfg.use_rest_discovery && tp.http) {
			auto http = tp.http();
			GatewayInfo info;
			std::string err;
			if (http && fetch_gateway_info(*http, cfg.token, info,
						       &err)) {
				if (info.session_remaining >= 0) {
					log(LogLevel::Info,
					    "gateway: " + info.url + " (" +
					    std::to_string(info.session_remaining) +
					    "/" + std::to_string(info.session_total) +
					    " session starts left)");
				}
				return host_from_ws_url(info.url);
			}
			log(LogLevel::Warn,
			    "gateway discovery failed, using default host: " + err);
		}
		return DEFAULT_GATEWAY_HOST;
	}

	Outcome run_once(void);
	void dispatch(const std::string &t, const Json::Value &d, Outcome &oc);
};

void Gateway::Impl::dispatch(const std::string &t, const Json::Value &d,
			     Outcome &oc)
{
	if (t == "READY") {
		Ready r = parse_ready(d);
		session_id = r.session_id;
		resume_url = r.resume_gateway_url;
		oc.made_progress = true;
		log(LogLevel::Info, "READY as " + r.user.username +
				    " (session " + r.session_id + ")");
		if (cb_ready)
			cb_ready(r);
	} else if (t == "RESUMED") {
		oc.made_progress = true;
		log(LogLevel::Info, "RESUMED");
	} else if (t == "MESSAGE_CREATE") {
		if (cb_msg_create)
			cb_msg_create(parse_message(d));
	} else if (t == "MESSAGE_UPDATE") {
		if (cb_msg_update)
			cb_msg_update(parse_message(d));
	} else if (t == "MESSAGE_DELETE") {
		if (cb_msg_delete)
			cb_msg_delete(parse_message_delete(d));
	}
	/* Everything else (GUILD_CREATE, TYPING_START, ...) is ignored. */
}

Outcome Gateway::Impl::run_once(void)
{
	Outcome oc;
	const bool resuming = !session_id.empty();
	const std::string host = pick_host(resuming);

	if (stopping.load()) {
		oc.next = Next::Stop;
		oc.reason = StopReason::Requested;
		return oc;
	}

	std::shared_ptr<WebSocket> sock(tp.ws().release());
	if (!sock) {
		log(LogLevel::Error, "transport produced no WebSocket");
		return oc;
	}
	{
		std::lock_guard<std::mutex> lk(ws_mtx);
		ws = sock;
	}

	std::string err;
	log(LogLevel::Info, std::string(resuming ? "resuming" : "connecting") +
			    " to wss://" + host + GATEWAY_TARGET);
	if (!sock->connect(host, GATEWAY_TARGET, &err)) {
		log(LogLevel::Warn, "connect failed: " + err);
		return oc;
	}

	bool identified = false;
	for (;;) {
		std::string raw;
		ReadStatus st = sock->read(raw, &err);

		if (st == ReadStatus::Closed) {
			CloseInfo ci = sock->peer_close();
			stop_heartbeat();

			if (stopping.load()) {
				oc.next = Next::Stop;
				oc.reason = StopReason::Requested;
				return oc;
			}
			if (ci.code && is_fatal_close_code(ci.code)) {
				log(LogLevel::Error,
				    "FATAL close code " + std::to_string(ci.code) +
				    " (" + ci.reason + ") -- not reconnecting");
				if (ci.code == 4014) {
					log(LogLevel::Error,
					    "4014 means a privileged intent is not enabled "
					    "for this application; enable MESSAGE_CONTENT "
					    "in the Developer Portal");
				}
				oc.next = Next::Stop;
				oc.reason = (ci.code == 4004)
						    ? StopReason::AuthFailed
						    : StopReason::FatalClose;
				return oc;
			}
			/*
			 * 4007/4009 (bad seq / session timed out) mean the
			 * session cannot be resumed; drop it so the next
			 * attempt does a clean IDENTIFY.
			 */
			if (ci.code == 4007 || ci.code == 4009)
				session_id.clear();

			log(LogLevel::Warn,
			    "connection closed (code " + std::to_string(ci.code) +
			    ") -- will reconnect");
			return oc;
		}

		if (st == ReadStatus::Error) {
			stop_heartbeat();
			if (stopping.load()) {
				oc.next = Next::Stop;
				oc.reason = StopReason::Requested;
				return oc;
			}
			log(LogLevel::Warn, "read error: " + err);
			return oc;
		}

		Json::Value v;
		if (!parse_json(raw, v, &err)) {
			log(LogLevel::Warn, "unparsable frame: " + err);
			continue;
		}

		const int op = v["op"].isIntegral() ? v["op"].asInt() : -1;
		if (v.isMember("s") && v["s"].isIntegral())
			seq = v["s"].asInt64();

		switch (op) {
		case OP_HELLO: {
			const int64_t iv =
				v["d"]["heartbeat_interval"].asInt64();
			log(LogLevel::Info, "HELLO heartbeat_interval=" +
					    std::to_string(iv) + "ms");
			start_heartbeat(sock, iv);

			const std::string frame =
				resuming ? resume_frame() : identify_frame();
			if (!sock->write(frame, &err)) {
				log(LogLevel::Warn,
				    "identify/resume write failed: " + err);
				stop_heartbeat();
				return oc;
			}
			identified = true;
			log(LogLevel::Debug,
			    resuming ? "-> RESUME" : "-> IDENTIFY");
			break;
		}
		case OP_HEARTBEAT:
			/* Server asked for an immediate beat. */
			ack_pending = true;
			if (!sock->write(heartbeat_frame(), &err))
				log(LogLevel::Warn,
				    "requested heartbeat failed: " + err);
			break;
		case OP_HEARTBEAT_ACK:
			ack_pending = false;
			break;
		case OP_RECONNECT:
			log(LogLevel::Info,
			    "server asked us to reconnect; resuming");
			stop_heartbeat();
			sock->close(4000, "reconnect requested");
			sock->disconnect();
			return oc;
		case OP_INVALID_SESSION: {
			const bool resumable =
				v["d"].isBool() && v["d"].asBool();
			log(LogLevel::Info,
			    std::string("invalid session (resumable=") +
			    (resumable ? "true" : "false") + ")");
			if (!resumable) {
				session_id.clear();
				resume_url.clear();
			}
			stop_heartbeat();
			sock->close(4000, "invalid session");
			sock->disconnect();
			/*
			 * The docs ask for a short random wait before the
			 * follow-up IDENTIFY.
			 */
			nap(1000 + (int)(4000 * jitter01()));
			return oc;
		}
		case OP_DISPATCH:
			dispatch(v["t"].isString() ? v["t"].asString()
						   : std::string(),
				 v["d"], oc);
			break;
		default:
			break;
		}
		(void)identified;
	}
}

Gateway::Gateway(GatewayConfig cfg, Transport tp, LogSink log)
	: impl_(new Impl())
{
	impl_->cfg = std::move(cfg);
	impl_->tp = std::move(tp);
	impl_->log_sink = std::move(log);
}

Gateway::~Gateway(void)
{
	stop();
	impl_->stop_heartbeat();
}

void Gateway::on_ready(std::function<void(const Ready &)> cb)
{
	impl_->cb_ready = std::move(cb);
}

void Gateway::on_message_create(std::function<void(const Message &)> cb)
{
	impl_->cb_msg_create = std::move(cb);
}

void Gateway::on_message_update(std::function<void(const Message &)> cb)
{
	impl_->cb_msg_update = std::move(cb);
}

void Gateway::on_message_delete(std::function<void(const MessageDelete &)> cb)
{
	impl_->cb_msg_delete = std::move(cb);
}

void Gateway::stop(void)
{
	impl_->stopping = true;
	{
		std::lock_guard<std::mutex> lk(impl_->hb_mtx);
		impl_->hb_stop = true;
	}
	impl_->hb_cv.notify_all();
	impl_->sleep_cv.notify_all();

	std::shared_ptr<WebSocket> sock;
	{
		std::lock_guard<std::mutex> lk(impl_->ws_mtx);
		sock = impl_->ws;
	}
	if (sock)
		sock->disconnect(); /* unblocks a read() in run_once() */
}

StopReason Gateway::run(void)
{
	Impl &m = *impl_;

	if (!m.tp.valid()) {
		m.log(LogLevel::Error, "no transport configured");
		return StopReason::Exhausted;
	}
	if (m.cfg.token.empty()) {
		m.log(LogLevel::Error, "no bot token configured");
		return StopReason::AuthFailed;
	}

	int attempt = 0;
	int backoff = m.cfg.backoff_min;

	for (;;) {
		if (m.stopping.load())
			return StopReason::Requested;

		Outcome oc = m.run_once();
		m.stop_heartbeat();

		if (oc.next == Next::Stop)
			return oc.reason;
		if (m.stopping.load())
			return StopReason::Requested;

		if (oc.made_progress) {
			/* A connection that worked resets the penalty. */
			attempt = 0;
			backoff = m.cfg.backoff_min;
		} else {
			attempt++;
			if (m.cfg.max_attempts &&
			    attempt >= m.cfg.max_attempts) {
				m.log(LogLevel::Error,
				      "giving up after " +
				      std::to_string(attempt) + " attempts");
				return StopReason::Exhausted;
			}
		}

		/* Full jitter: sleep in [0, backoff). */
		const int wait_ms = (int)(backoff * 1000 * m.jitter01());
		m.log(LogLevel::Info, "reconnecting in " +
				      std::to_string(wait_ms) + "ms");
		if (!m.nap(wait_ms))
			return StopReason::Requested;

		backoff *= 2;
		if (backoff > m.cfg.backoff_max)
			backoff = m.cfg.backoff_max;
	}
}

} /* namespace gwdiscord */
