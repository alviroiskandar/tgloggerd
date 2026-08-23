// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
/*
 * THE ONLY TRANSLATION UNIT IN gwdiscord THAT INCLUDES BOOST.
 *
 * Everything Boost.Beast-shaped is confined here, behind the gwdiscord::
 * WebSocket / HttpClient interfaces. No Boost type appears in any header, so
 * no other TU -- and no consumer of this library -- compiles against Boost at
 * all. To move to a different network stack, add a sibling .cpp implementing
 * the same two interfaces and swap one line in CMakeLists.txt; see README.md.
 */
#include <gwdiscord/Transport.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <atomic>
#include <memory>
#include <mutex>

namespace beast = boost::beast;
namespace http  = boost::beast::http;
namespace ws    = boost::beast::websocket;
namespace net   = boost::asio;
namespace ssl   = boost::asio::ssl;
using tcp       = boost::asio::ip::tcp;

namespace gwdiscord {
namespace {

/* Shared TLS setup: system trust store, peer + hostname verification. */
static void init_ssl_ctx(ssl::context &ctx, const std::string &host)
{
	ctx.set_default_verify_paths();
	ctx.set_verify_mode(ssl::verify_peer);
	ctx.set_verify_callback(ssl::host_name_verification(host));
}

class BeastWebSocket final : public WebSocket {
public:
	BeastWebSocket(void) : ctx_(ssl::context::tlsv12_client) {}

	~BeastWebSocket(void) override
	{
		disconnect();
	}

	bool connect(const std::string &host, const std::string &target,
		     std::string *err) override
	{
		try {
			init_ssl_ctx(ctx_, host);
			s_ = std::make_unique<Stream>(ioc_, ctx_);

			tcp::resolver resolver{ioc_};
			auto const results = resolver.resolve(host, "443");
			net::connect(s_->next_layer().next_layer(), results);

			/* SNI: Discord's edge requires it. */
			if (!SSL_set_tlsext_host_name(
				    s_->next_layer().native_handle(),
				    host.c_str())) {
				if (err)
					*err = "failed to set TLS SNI hostname";
				return false;
			}
			s_->next_layer().handshake(ssl::stream_base::client);

			/*
			 * Beast's default is a permessage-deflate-less, 16 MiB
			 * message limit; Discord frames stay far below it.
			 */
			s_->set_option(ws::stream_base::decorator(
				[](ws::request_type &req) {
					req.set(http::field::user_agent,
						"gwdiscord (https://git.gnuweeb.net/GNUWeeb/tgloggerd)");
				}));
			s_->handshake(host, target);
			disconnected_ = false;
			return true;
		} catch (const std::exception &e) {
			if (err)
				*err = e.what();
			s_.reset();
			return false;
		}
	}

	ReadStatus read(std::string &out, std::string *err) override
	{
		if (!s_) {
			if (err)
				*err = "read on a closed WebSocket";
			return ReadStatus::Error;
		}

		beast::flat_buffer buf;
		beast::error_code ec;
		s_->read(buf, ec);

		if (ec == ws::error::closed) {
			/*
			 * Beast surfaces the close code as a typed field. This
			 * is what lets the Gateway distinguish a fatal 4014
			 * from a recoverable drop -- see is_fatal_close_code().
			 */
			peer_close_.code =
				static_cast<uint16_t>(s_->reason().code);
			peer_close_.reason =
				std::string(s_->reason().reason.data(),
					    s_->reason().reason.size());
			return ReadStatus::Closed;
		}
		if (ec) {
			/* A disconnect() we asked for is not an error. */
			if (disconnected_.load())
				return ReadStatus::Closed;
			if (err)
				*err = ec.message();
			return ReadStatus::Error;
		}

		out = beast::buffers_to_string(buf.data());
		return ReadStatus::Message;
	}

	bool write(const std::string &payload, std::string *err) override
	{
		if (!s_) {
			if (err)
				*err = "write on a closed WebSocket";
			return false;
		}
		/*
		 * Beast permits one concurrent reader and one concurrent
		 * writer, but not two writers. The heartbeat thread and any
		 * protocol-level send both land here, so serialize them.
		 */
		std::lock_guard<std::mutex> lk(write_mtx_);
		beast::error_code ec;
		s_->text(true);
		s_->write(net::buffer(payload), ec);
		if (ec) {
			if (err)
				*err = ec.message();
			return false;
		}
		return true;
	}

	void close(uint16_t code, const std::string &reason) override
	{
		if (!s_)
			return;
		std::lock_guard<std::mutex> lk(write_mtx_);
		beast::error_code ec;
		/*
		 * Beast htons-casts whatever code it is handed, so Discord's
		 * 4000-4999 range passes through verbatim. Closing with 1000
		 * or 1001 would make the session unresumable.
		 */
		s_->close(ws::close_reason(static_cast<ws::close_code>(code),
					   reason),
			  ec);
		(void)ec; /* Best-effort: we are tearing down anyway. */
	}

	CloseInfo peer_close(void) const override
	{
		return peer_close_;
	}

	void disconnect(void) override
	{
		disconnected_ = true;
		if (!s_)
			return;
		beast::error_code ec;
		/*
		 * Shut the TCP socket down underneath the TLS and WebSocket
		 * layers: that is what makes a read() blocked on another
		 * thread return promptly.
		 */
		s_->next_layer().next_layer().shutdown(
			tcp::socket::shutdown_both, ec);
		s_->next_layer().next_layer().close(ec);
	}

private:
	using Stream = ws::stream<ssl::stream<tcp::socket>>;

	net::io_context		ioc_;
	ssl::context		ctx_;
	std::unique_ptr<Stream>	s_;
	std::mutex		write_mtx_;
	CloseInfo		peer_close_;
	std::atomic<bool>	disconnected_{false};
};

class BeastHttpClient final : public HttpClient {
public:
	HttpResponse get(const std::string &host, const std::string &target,
			 const HttpHeaders &headers) override
	{
		HttpResponse out;
		try {
			net::io_context ioc;
			ssl::context ctx{ssl::context::tlsv12_client};
			init_ssl_ctx(ctx, host);

			ssl::stream<tcp::socket> s{ioc, ctx};
			tcp::resolver resolver{ioc};
			net::connect(s.next_layer(), resolver.resolve(host, "443"));
			if (!SSL_set_tlsext_host_name(s.native_handle(),
						      host.c_str())) {
				out.error = "failed to set TLS SNI hostname";
				return out;
			}
			s.handshake(ssl::stream_base::client);

			http::request<http::empty_body> req{http::verb::get,
							    target, 11};
			req.set(http::field::host, host);
			for (const auto &h : headers)
				req.set(h.first, h.second);
			http::write(s, req);

			beast::flat_buffer buf;
			http::response<http::string_body> res;
			http::read(s, buf, res);

			out.status = static_cast<long>(res.result_int());
			out.body = res.body();

			beast::error_code ec;
			s.shutdown(ec); /* stream_truncated here is benign. */
			return out;
		} catch (const std::exception &e) {
			out.status = 0;
			out.error = e.what();
			return out;
		}
	}
};

} /* anonymous namespace */

Transport beast_transport(void)
{
	Transport tp;
	tp.ws = [] { return std::unique_ptr<WebSocket>(new BeastWebSocket()); };
	tp.http = [] {
		return std::unique_ptr<HttpClient>(new BeastHttpClient());
	};
	return tp;
}

} /* namespace gwdiscord */
