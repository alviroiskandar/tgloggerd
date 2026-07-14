// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include <drogon/drogon.h>

#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

/* getenv with a default; empty value counts as unset (mirrors the daemon). */
std::string env(const char *key, const char *def)
{
	const char *v = getenv(key);
	return (v && *v) ? std::string(v) : std::string(def);
}

} /* namespace */

int main(void)
{
	std::string addr = env("WEB_LISTEN_ADDR", "127.0.0.1");
	uint16_t port = (uint16_t)atoi(env("WEB_LISTEN_PORT", "8080").c_str());
	int threads = atoi(env("WEB_THREADS", "4").c_str());

	/* Minimal liveness endpoint; real controllers are added incrementally. */
	drogon::app().registerHandler("/healthz",
		[](const drogon::HttpRequestPtr &,
		   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
			resp->setBody("ok\n");
			cb(resp);
		});

	LOG_INFO << "tgloggerd_web listening on " << addr << ":" << port;
	drogon::app()
		.addListener(addr, port)
		.setThreadNum((size_t)(threads > 0 ? threads : 1))
		.run();
	return 0;
}
