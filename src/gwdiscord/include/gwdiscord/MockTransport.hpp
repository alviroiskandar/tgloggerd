// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef GWDISCORD__MOCK_TRANSPORT_HPP
#define GWDISCORD__MOCK_TRANSPORT_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Transport.hpp"

namespace gwdiscord {

/*
 * A second, non-network implementation of the transport interfaces.
 *
 * It exists for two reasons. Practically, it lets the Gateway state machine be
 * driven over canned frames -- including paths that are impossible or
 * dangerous to provoke against the live service, such as a fatal 4014 close
 * (deliberately triggering that against Discord burns the IDENTIFY budget and
 * can get the bot token reset). Structurally, it is the proof that the seam in
 * Transport.hpp is real: if this compiles and drives the protocol without a
 * single Boost symbol, then Boost is genuinely replaceable.
 */
struct MockScript {
	/* Frames the fake server delivers, in order, one per read(). */
	std::vector<std::string> frames;

	/*
	 * When the script runs out: if close_when_done is set, read() reports
	 * a close carrying close_code. Otherwise read() blocks -- modelling an
	 * idle-but-healthy connection -- until disconnect() is called.
	 */
	bool		close_when_done = false;
	uint16_t	close_code = 1000;
	std::string	close_reason;

	/* Fail connect() outright, to exercise the backoff path. */
	bool		fail_connect = false;
};

/* Everything the fake server observed, for assertions. */
struct MockRecord {
	std::mutex			mtx;
	std::vector<std::string>	sent;		/* client -> server */
	std::vector<std::string>	connected_hosts;
	uint16_t			client_close_code = 0;
	int				connect_attempts = 0;

	std::vector<std::string> sent_copy(void)
	{
		std::lock_guard<std::mutex> lk(mtx);
		return sent;
	}
};

/*
 * Build a Transport backed by `script`. Every observation lands in `rec`,
 * which the caller keeps a reference to. The HttpClient half answers
 * GET /gateway/bot with `gateway_url`.
 */
Transport mock_transport(std::shared_ptr<MockScript> script,
			 std::shared_ptr<MockRecord> rec,
			 std::string gateway_url = "wss://mock.invalid");

} /* namespace gwdiscord */

#endif /* #ifndef GWDISCORD__MOCK_TRANSPORT_HPP */
