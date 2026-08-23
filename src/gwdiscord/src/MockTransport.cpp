// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include <gwdiscord/MockTransport.hpp>

#include <condition_variable>

namespace gwdiscord {
namespace {

class MockWebSocket final : public WebSocket {
public:
	MockWebSocket(std::shared_ptr<MockScript> s,
		      std::shared_ptr<MockRecord> r)
		: script_(std::move(s)), rec_(std::move(r))
	{
	}

	bool connect(const std::string &host, const std::string &target,
		     std::string *err) override
	{
		(void)target;
		{
			std::lock_guard<std::mutex> lk(rec_->mtx);
			rec_->connected_hosts.push_back(host);
			rec_->connect_attempts++;
		}
		if (script_->fail_connect) {
			if (err)
				*err = "mock: connect refused";
			return false;
		}
		return true;
	}

	ReadStatus read(std::string &out, std::string *err) override
	{
		(void)err;
		std::unique_lock<std::mutex> lk(mtx_);

		if (idx_ < script_->frames.size()) {
			out = script_->frames[idx_++];
			return ReadStatus::Message;
		}

		if (script_->close_when_done) {
			peer_.code = script_->close_code;
			peer_.reason = script_->close_reason;
			return ReadStatus::Closed;
		}

		/* Idle: hold until someone tears the connection down. */
		cv_.wait(lk, [this] { return down_; });
		peer_.code = 1006; /* abnormal: no close frame was seen */
		return ReadStatus::Closed;
	}

	bool write(const std::string &payload, std::string *err) override
	{
		(void)err;
		std::lock_guard<std::mutex> lk(rec_->mtx);
		rec_->sent.push_back(payload);
		return true;
	}

	void close(uint16_t code, const std::string &reason) override
	{
		(void)reason;
		std::lock_guard<std::mutex> lk(rec_->mtx);
		rec_->client_close_code = code;
	}

	CloseInfo peer_close(void) const override
	{
		return peer_;
	}

	void disconnect(void) override
	{
		{
			std::lock_guard<std::mutex> lk(mtx_);
			down_ = true;
		}
		cv_.notify_all();
	}

private:
	std::shared_ptr<MockScript>	script_;
	std::shared_ptr<MockRecord>	rec_;
	std::mutex			mtx_;
	std::condition_variable		cv_;
	size_t				idx_ = 0;
	bool				down_ = false;
	CloseInfo			peer_;
};

class MockHttpClient final : public HttpClient {
public:
	explicit MockHttpClient(std::string url) : url_(std::move(url)) {}

	HttpResponse get(const std::string &host, const std::string &target,
			 const HttpHeaders &headers) override
	{
		(void)host;
		(void)headers;
		HttpResponse res;
		if (target.find("/gateway/bot") != std::string::npos) {
			res.status = 200;
			res.body = "{\"url\":\"" + url_ +
				   "\",\"shards\":1,\"session_start_limit\":"
				   "{\"total\":1000,\"remaining\":999,"
				   "\"reset_after\":0,\"max_concurrency\":1}}";
			return res;
		}
		res.status = 404;
		res.body = "{}";
		return res;
	}

private:
	std::string url_;
};

} /* anonymous namespace */

Transport mock_transport(std::shared_ptr<MockScript> script,
			 std::shared_ptr<MockRecord> rec,
			 std::string gateway_url)
{
	Transport tp;
	tp.ws = [script, rec] {
		return std::unique_ptr<WebSocket>(
			new MockWebSocket(script, rec));
	};
	tp.http = [gateway_url] {
		return std::unique_ptr<HttpClient>(
			new MockHttpClient(gateway_url));
	};
	return tp;
}

} /* namespace gwdiscord */
