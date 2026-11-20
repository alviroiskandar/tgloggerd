// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "StillsClient.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace tgloggerd {
namespace stills {

namespace {

/* The header line is short; anything longer is not one. */
constexpr size_t kMaxHeader = 4096;

Answer failed(const std::string &message)
{
	Answer a;
	a.kind = Answer::Kind::Failed;
	a.message = message;
	return a;
}

/* Closes the socket and clears @active on every way out. */
struct Conn {
	int			fd = -1;
	std::atomic<int>	*active = nullptr;
	~Conn()
	{
		if (active)
			active->store(-1);
		if (fd >= 0)
			::close(fd);
	}
};

bool send_all(int fd, const std::string &s)
{
	size_t off = 0;
	while (off < s.size()) {
		ssize_t n = ::send(fd, s.data() + off, s.size() - off,
				   MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return false;
		off += (size_t)n;
	}
	return true;
}

} /* namespace */

std::string requestLine(const Request &req)
{
	nlohmann::json j = {
		{ "path", req.path },
		{ "kind", req.kind },
		{ "duration", req.duration > 0 ? req.duration : 0.0 },
	};
	return j.dump() + "\n";
}

std::optional<Header> parseHeader(const std::string &line)
{
	nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
	if (!j.is_object() || !j.contains("ok") || !j["ok"].is_boolean())
		return std::nullopt;
	Header h;
	h.ok = j["ok"].get<bool>();
	if (h.ok) {
		if (!j.contains("format") || !j["format"].is_string() ||
		    !j.contains("size") || !j["size"].is_number_unsigned())
			return std::nullopt;
		h.format = j["format"].get<std::string>();
		uint64_t size = j["size"].get<uint64_t>();
		if ((h.format != "jpg" && h.format != "png") || size == 0 ||
		    size > kMaxStill)
			return std::nullopt;
		h.size = (size_t)size;
		return h;
	}
	if (!j.contains("error") || !j["error"].is_string() ||
	    j["error"].get<std::string>().empty())
		return std::nullopt;
	h.error = j["error"].get<std::string>();
	if (j.contains("message") && j["message"].is_string())
		h.message = j["message"].get<std::string>();
	return h;
}

Answer ask(const std::string &socket_path, const Request &req,
	   std::chrono::seconds timeout, std::atomic<int> *active)
{
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	if (socket_path.empty() || socket_path.size() >= sizeof(addr.sun_path))
		return failed("bad socket path");
	std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size());

	Conn c;
	c.fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (c.fd < 0)
		return failed(std::string("socket: ") + std::strerror(errno));
	c.active = active;
	if (active)
		active->store(c.fd);

	timeval tv{};
	tv.tv_sec = (time_t)timeout.count();
	setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(c.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	if (::connect(c.fd, (const sockaddr *)&addr, sizeof(addr)) != 0) {
		Answer a;
		a.kind = Answer::Kind::Unreachable;
		a.message = socket_path + ": " + std::strerror(errno);
		return a;
	}
	if (!send_all(c.fd, requestLine(req)))
		return failed(std::string("send: ") + std::strerror(errno));

	/* The header line, then exactly its size in bytes. */
	std::string buf;
	size_t nl = std::string::npos;
	size_t want = 0;
	std::optional<Header> h;
	char chunk[16384];
	for (;;) {
		if (!h) {
			nl = buf.find('\n');
			if (nl != std::string::npos) {
				h = parseHeader(buf.substr(0, nl));
				if (!h)
					return failed("not a header: " +
						      buf.substr(0, std::min<size_t>(nl, 200)));
				if (!h->ok) {
					Answer a;
					a.kind = Answer::Kind::Refused;
					a.error = h->error;
					a.message = h->message;
					return a;
				}
				buf.erase(0, nl + 1);
				want = h->size;
			} else if (buf.size() > kMaxHeader) {
				return failed("header line too long");
			}
		}
		if (h && buf.size() >= want)
			break;
		ssize_t n = ::recv(c.fd, chunk, sizeof(chunk), 0);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			return failed(std::string("recv: ") + std::strerror(errno));
		if (n == 0)
			return failed(h ? "the answer ended before its bytes"
					: "the answer ended before its header");
		buf.append(chunk, (size_t)n);
	}
	if (buf.size() > want)
		return failed("more bytes than the header said");

	Answer a;
	a.kind = Answer::Kind::Still;
	a.format = h->format;
	a.bytes = std::move(buf);
	return a;
}

} /* namespace stills */
} /* namespace tgloggerd */
