// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD__STILLS_CLIENT_HPP
#define TGLOGGERD__STILLS_CLIENT_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace tgloggerd {
namespace stills {

/*
 * The client side of the stills service's protocol (docker/stills/server.py):
 * one request per connection over a Unix socket, a JSON line
 * {"path", "kind", "duration"}; the answer a JSON header line, then the
 * image bytes.
 */

/* The most bytes a still may have; the service refuses bigger ones. */
constexpr size_t kMaxStill = 200 * 1024;

struct Request {
	std::string	path;		/* the blob, relative to the store */
	std::string	kind;		/* video, animation, video_note, sticker */
	double		duration = 0;	/* seconds; 0 = unknown */
};

/* The answer's header line, as parsed. */
struct Header {
	bool		ok = false;
	std::string	format;		/* ok: "jpg" or "png" */
	size_t		size = 0;	/* ok: bytes that follow */
	std::string	error;		/* not ok: the service's code */
	std::string	message;
};

struct Answer {
	enum class Kind {
		Still,		/* bytes holds the image */
		Refused,	/* the service refused this video (error, message) */
		Unreachable,	/* no service at the socket */
		Failed,		/* the exchange broke off or made no sense */
	};
	Kind		kind = Kind::Failed;
	std::string	format;
	std::string	bytes;
	std::string	error;
	std::string	message;
};

/* The request line, newline included. */
std::string requestLine(const Request &req);

/*
 * The header line (without its newline), or nullopt when it is not one:
 * not a JSON object, an ok answer without a known format or with a size
 * of 0 or over kMaxStill, a refusal without an error code.
 */
std::optional<Header> parseHeader(const std::string &line);

/*
 * The service's answer for @req at @socket_path. Every read and write is
 * bounded by @timeout. While it runs, @active holds the socket's file
 * descriptor (else -1), so another thread can shut it down to cut a
 * wait short.
 */
Answer ask(const std::string &socket_path, const Request &req,
	   std::chrono::seconds timeout, std::atomic<int> *active = nullptr);

} /* namespace stills */
} /* namespace tgloggerd */

#endif /* #ifndef TGLOGGERD__STILLS_CLIENT_HPP */
