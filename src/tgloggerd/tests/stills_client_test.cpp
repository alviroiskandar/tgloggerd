// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
/*
 * stills_client_test -- the daemon's side of the stills service's
 * protocol (StillsClient): the request line, the header parsing, and
 * whole exchanges with a stand-in service on a Unix socket in a
 * temporary directory: a still, a refusal, an answer cut short, one with
 * bytes to spare, garbage, no service at all, a service that never
 * answers, and a wait cut short from another thread.
 *
 *   cmake --build build --target stills_client_test && build/stills_client_test
 */
#include <tgloggerd/StillsClient.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace tgloggerd;
using Kind = stills::Answer::Kind;

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

/*
 * A stand-in service for one connection: it reads the request line into
 * got, then hands the connection to @reply. got is to be read after
 * done().
 */
struct Service {
	std::string	path;
	int		fd = -1;
	std::thread	thr;
	std::string	got;

	void done(void)
	{
		shutdown(fd, SHUT_RDWR);
		if (thr.joinable())
			thr.join();
	}

	Service(const std::string &p, std::function<void(int)> reply) : path(p)
	{
		unlink(path.c_str());
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		sockaddr_un a{};
		a.sun_family = AF_UNIX;
		std::strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
		if (bind(fd, (sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 1) != 0) {
			perror("bind");
			std::exit(1);
		}
		thr = std::thread([this, reply] {
			int c = accept(fd, nullptr, nullptr);
			if (c < 0)
				return;
			char ch;
			while (read(c, &ch, 1) == 1 && ch != '\n')
				got += ch;
			reply(c);
			close(c);
		});
	}
	~Service()
	{
		done();
		close(fd);
		unlink(path.c_str());
	}
};

static void put(int fd, const std::string &s)
{
	(void)!write(fd, s.data(), s.size());
}

int main()
{
	const char *tmp = getenv("TMPDIR");
	std::string buf = std::string(tmp && *tmp ? tmp : "/tmp") +
			  "/stills_client_test.XXXXXX";
	char *dir = mkdtemp(buf.data());
	if (!dir) {
		perror("mkdtemp");
		return 1;
	}
	const std::string sock = std::string(dir) + "/stills.sock";
	const auto t5 = std::chrono::seconds(5);

	stills::Request req;
	req.path = "0a/53/3d/97/ed/0a533d.mp4";
	req.kind = "video";
	req.duration = 12.5;

	printf("request line:\n");
	CHECK(stills::requestLine(req) ==
	      R"({"duration":12.5,"kind":"video","path":"0a/53/3d/97/ed/0a533d.mp4"})"
	      "\n",
	      "path, kind and duration as one JSON line");
	{
		stills::Request r2 = req;
		r2.duration = -3;
		CHECK(stills::requestLine(r2).find("\"duration\":0.0") !=
		      std::string::npos, "no negative duration");
	}

	printf("header:\n");
	{
		auto h = stills::parseHeader(R"({"ok":true,"format":"jpg","size":1234})");
		CHECK(h && h->ok && h->format == "jpg" && h->size == 1234,
		      "a JPEG still");
		h = stills::parseHeader(R"({"ok":true,"format":"png","size":204800})");
		CHECK(h && h->format == "png" && h->size == 204800,
		      "a PNG still at the cap");
		h = stills::parseHeader(R"({"ok":false,"error":"no_video","message":"m"})");
		CHECK(h && !h->ok && h->error == "no_video" && h->message == "m",
		      "a refusal");
		CHECK(!stills::parseHeader(R"({"ok":true,"format":"jpg","size":204801})") &&
		      !stills::parseHeader(R"({"ok":true,"format":"jpg","size":0})") &&
		      !stills::parseHeader(R"({"ok":true,"format":"gif","size":5})") &&
		      !stills::parseHeader(R"({"ok":true,"format":"jpg","size":-5})") &&
		      !stills::parseHeader(R"({"ok":true,"size":5})") &&
		      !stills::parseHeader(R"({"ok":false})") &&
		      !stills::parseHeader(R"({"ok":"yes"})") &&
		      !stills::parseHeader("[1]") && !stills::parseHeader("nope"),
		      "not over the cap, empty, another format, no format, a"
		      " refusal without a code, or not a header at all");
	}

	printf("exchanges:\n");
	{
		Service s(sock, [](int c) {
			put(c, "{\"ok\":true,\"format\":\"jpg\",\"size\":5}\nab");
			usleep(50000);
			put(c, "cde");
		});
		auto a = stills::ask(sock, req, t5);
		CHECK(a.kind == Kind::Still && a.format == "jpg" &&
		      a.bytes == "abcde", "a still, its bytes in two pieces");
		s.done();
		CHECK(s.got + "\n" == stills::requestLine(req),
		      "the service got the request line");
	}
	{
		Service s(sock, [](int c) {
			put(c, "{\"ok\":false,\"error\":\"too_large\","
			       "\"message\":\"the still is 300000 bytes\"}\n");
		});
		auto a = stills::ask(sock, req, t5);
		CHECK(a.kind == Kind::Refused && a.error == "too_large" &&
		      a.message == "the still is 300000 bytes",
		      "a refusal with its code and message");
	}
	{
		Service s(sock, [](int c) {
			put(c, "{\"ok\":true,\"format\":\"png\",\"size\":10}\nabc");
		});
		auto a = stills::ask(sock, req, t5);
		CHECK(a.kind == Kind::Failed, "an answer cut short");
	}
	{
		Service s(sock, [](int c) {
			put(c, "{\"ok\":true,\"format\":\"png\",\"size\":2}\nabc");
		});
		auto a = stills::ask(sock, req, t5);
		CHECK(a.kind == Kind::Failed, "more bytes than the header said");
	}
	{
		Service s(sock, [](int c) { put(c, "HTTP/1.1 200 OK\r\n\r\n"); });
		auto a = stills::ask(sock, req, t5);
		CHECK(a.kind == Kind::Failed, "garbage");
	}
	{
		Service s(sock, [](int c) { put(c, std::string(5000, 'x')); });
		auto a = stills::ask(sock, req, t5);
		CHECK(a.kind == Kind::Failed, "a header line that never ends");
	}
	{
		auto a = stills::ask(std::string(dir) + "/none.sock", req, t5);
		CHECK(a.kind == Kind::Unreachable, "no service at the socket");
	}
	{
		Service s(sock, [](int) { usleep(3000000); });
		auto t0 = std::chrono::steady_clock::now();
		auto a = stills::ask(sock, req, std::chrono::seconds(1));
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - t0).count();
		CHECK(a.kind == Kind::Failed && ms < 2500,
		      "a service that does not answer in time");
	}
	{
		Service s(sock, [](int) { usleep(3000000); });
		std::atomic<int> active{ -1 };
		std::thread cut([&] {
			for (int i = 0; i < 100 && active.load() < 0; i++)
				usleep(10000);
			usleep(100000);
			int fd = active.load();
			if (fd >= 0)
				shutdown(fd, SHUT_RDWR);
		});
		auto t0 = std::chrono::steady_clock::now();
		auto a = stills::ask(sock, req, std::chrono::seconds(30), &active);
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - t0).count();
		cut.join();
		CHECK(a.kind == Kind::Failed && ms < 2000 && active.load() == -1,
		      "a wait cut short by shutting the active socket down");
	}

	rmdir(dir);
	if (g_failures) {
		printf("%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
