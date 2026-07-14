// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include <drogon/drogon.h>
#include <drogon/orm/DbConfig.h>

#include <cstdint>
#include <string>

#include "Config.hpp"

namespace {

/*
 * Register one Drogon async MySQL client from a DbConfig. Drogon's MySQL
 * backend is the non-blocking MariaDB Connector/C API, so these clients are
 * safe to co_await from event-loop handlers (unlike the daemon's blocking
 * JDBC layer, which the web app deliberately does not use).
 */
void addMysqlClient(const tgweb::DbConfig &db)
{
	drogon::orm::MysqlConfig cfg;

	cfg.host             = db.host;
	cfg.port             = db.port;
	cfg.databaseName     = db.dbName;
	cfg.username         = db.user;
	cfg.password         = db.password;
	cfg.connectionNumber = db.connNum;
	cfg.name             = db.name;
	cfg.isFast           = false;
	cfg.characterSet     = "utf8mb4";
	cfg.timeout          = -1.0;

	drogon::app().addDbClient(cfg);
}

} /* namespace */

int main(void)
{
	tgweb::Config cfg = tgweb::Config::fromEnv();

	addMysqlClient(cfg.ro);
	addMysqlClient(cfg.app);

	/* Minimal liveness endpoint; real controllers are added incrementally. */
	drogon::app().registerHandler("/healthz",
		[](const drogon::HttpRequestPtr &,
		   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
			auto resp = drogon::HttpResponse::newHttpResponse();
			resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
			resp->setBody("ok\n");
			cb(resp);
		});

	LOG_INFO << "tgloggerd_web listening on " << cfg.addr << ":" << cfg.port;
	drogon::app()
		.addListener(cfg.addr, cfg.port)
		.setThreadNum((size_t)(cfg.threads > 0 ? cfg.threads : 1))
		.run();
	return 0;
}
