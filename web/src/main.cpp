// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include <drogon/drogon.h>

#include <cstdio>
#include <drogon/orm/DbConfig.h>

#include <termios.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include "Config.hpp"
#include "auth/Password.hpp"
#include "auth/Token.hpp"
#include "auth/FileToken.hpp"
#include "dao/Accounts.hpp"
#include "views/Render.hpp"

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

/* Read a line from stdin with terminal echo disabled (for passwords). */
std::string readSecret(const char *prompt)
{
	std::cerr << prompt;

	struct termios oldt;
	bool tty = tcgetattr(STDIN_FILENO, &oldt) == 0;
	if (tty) {
		struct termios noecho = oldt;
		noecho.c_lflag &= ~(tcflag_t)ECHO;
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &noecho);
	}

	std::string line;
	std::getline(std::cin, line);

	if (tty) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
		std::cerr << "\n";
	}
	return line;
}

/*
 * Seed (or reset) an admin account. Prompts for the password twice on stdin,
 * hashes it with argon2id, then upserts the row over the "app" DbClient. The
 * write is driven by Drogon's own event loop (registerBeginningAdvice +
 * async_run) rather than a standalone client, then the loop quits. Returns a
 * process exit code.
 */
int seedAdmin(const tgweb::Config &cfg, const std::string &username)
{
	if (!tgweb::auth::initCrypto()) {
		std::cerr << "error: failed to initialize libsodium\n";
		return 1;
	}

	std::string p1 = readSecret("New admin password: ");
	std::string p2 = readSecret("Confirm password: ");
	if (p1.empty()) {
		std::cerr << "error: password must not be empty\n";
		return 1;
	}
	if (p1 != p2) {
		std::cerr << "error: passwords do not match\n";
		return 1;
	}

	std::string hash = tgweb::auth::hashPassword(p1);
	if (hash.empty()) {
		std::cerr << "error: password hashing failed\n";
		return 1;
	}

	addMysqlClient(cfg.app);

	int rc = 0;
	drogon::app().registerBeginningAdvice([&]() {
		drogon::async_run([&]() -> drogon::Task<> {
			try {
				auto db = drogon::app().getDbClient("app");
				co_await tgweb::dao::accounts::seedAdmin(db,
						username, hash);
				std::cerr << "Seeded admin account '"
					  << username << "'.\n";
			} catch (const std::exception &e) {
				std::cerr << "error: " << e.what() << "\n";
				rc = 1;
			}
			drogon::app().quit();
			co_return;
		});
	});
	drogon::app().run();
	return rc;
}

} /* namespace */

int main(int argc, char **argv)
{
	tgweb::Config cfg = tgweb::Config::fromEnv();

	/* CLI: --seed-admin <username> creates/resets an admin, then exits. */
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--seed-admin")) {
			if (i + 1 >= argc) {
				std::cerr << "usage: tgloggerd_web --seed-admin <username>\n";
				return 1;
			}
			return seedAdmin(cfg, argv[i + 1]);
		}
		std::cerr << "error: unknown argument '" << argv[i] << "'\n";
		return 1;
	}

	if (!tgweb::auth::initCrypto()) {
		std::cerr << "error: failed to initialize libsodium\n";
		return 1;
	}

	/*
	 * Load the signing key for the stateless auth/CSRF cookies. There is no
	 * server-side session store: identity lives in a signed cookie, so a
	 * missing or bad key must stop startup rather than silently accept forged
	 * cookies.
	 */
	if (!tgweb::auth::token::init()) {
		std::cerr << "error: WEB_APP_KEY is unset or not valid base64 of "
			     "at least 16 bytes\n";
		return 1;
	}

	/* Same key, distinct subkeys: the reversible tokens for the public
	 * /files/<token> URLs. token::init() already validated WEB_APP_KEY. */
	if (!tgweb::auth::filetoken::init()) {
		std::cerr << "error: failed to derive file-token key from "
			     "WEB_APP_KEY\n";
		return 1;
	}

	/* Template rendering and static assets. */
	tgweb::views::Render::init(tgweb::env("WEB_TEMPLATE_DIR", "views/templates"),
				   "tgloggerd");

	/*
	 * Keep Drogon's upload cache out of the (bind-mounted) repository root.
	 * On startup Drogon creates <uploadPath>/tmp/00..FF shard directories for
	 * buffering multipart uploads; the app accepts none, so they stay empty,
	 * but we still relocate them under data/storage/web. A relative upload
	 * path is resolved against the document root, so this MUST come before
	 * setDocumentRoot() while the root is still the working directory (./).
	 */
	drogon::app().setUploadPath("data/storage/web/uploads");
	drogon::app().setDocumentRoot(tgweb::env("WEB_STATIC_DIR", "static"));

	addMysqlClient(cfg.ro);
	addMysqlClient(cfg.app);

	/*
	 * Request tracing, off unless MCP_LOG_REQUESTS=1.
	 *
	 * Exists because an MCP client's connection attempt is otherwise a
	 * black box: when Claude Desktop failed to connect, the only way to
	 * tell whether it was sending the query-string key, probing OAuth
	 * discovery paths, or something else entirely was to watch what
	 * actually arrived.
	 *
	 * It logs the SHAPE of a request, never its content: whether a
	 * credential was present, not what it was. A token in a log is exactly
	 * the exposure the query-string parameter is already criticised for,
	 * and this must not add to it.
	 */
	if (tgweb::env("MCP_LOG_REQUESTS", "0") == "1") {
		drogon::app().registerPreRoutingAdvice(
			[](const drogon::HttpRequestPtr &req) {
				const std::string path(req->path());
				/* Only the paths an MCP client touches. */
				if (path.rfind("/mcp", 0) != 0 &&
				    path.rfind("/.well-known", 0) != 0 &&
				    path != "/register")
					return;

				const bool hasAuth =
					!req->getHeader("authorization").empty();
				const bool hasKey =
					!req->getParameter("key").empty();
				/*
				 * stderr, not LOG_INFO: drogon's logger is not
				 * wired to the container's stdout here, so the
				 * framework's own startup lines never appear
				 * either. A diagnostic that cannot be seen is
				 * not a diagnostic.
				 */
				/*
				 * Origin is logged because it is a silent way
				 * to fail: an unlisted origin is refused with
				 * 403 before authentication is even looked at,
				 * so a client sending one looks to its user
				 * like a credential problem.
				 */
				fprintf(stderr,
					"mcp-trace %s %s auth_header=%s key_param=%s "
					"origin=\"%s\" mcpver=\"%s\" ua=\"%s\"\n",
					req->methodString(), path.c_str(),
					hasAuth ? "yes" : "no",
					hasKey ? "yes" : "no",
					req->getHeader("origin").c_str(),
					req->getHeader("mcp-protocol-version").c_str(),
					req->getHeader("user-agent").c_str());
				fflush(stderr);
			});
		/* The response side: a status code is usually the thing that
		 * explains a client's behaviour, and the request alone never
		 * shows it. */
		drogon::app().registerPostHandlingAdvice(
			[](const drogon::HttpRequestPtr &req,
			   const drogon::HttpResponsePtr &resp) {
				const std::string path(req->path());
				if (path.rfind("/mcp", 0) != 0 &&
				    path.rfind("/.well-known", 0) != 0 &&
				    path != "/register")
					return;
				fprintf(stderr, "mcp-trace   -> %d %s %s\n",
					(int)resp->getStatusCode(),
					req->methodString(), path.c_str());
				fflush(stderr);
			});

		fprintf(stderr, "MCP request tracing enabled (MCP_LOG_REQUESTS=1)\n");
		fflush(stderr);
	}

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

	/*
	 * run() returns only after a shutdown signal has stopped the event
	 * loops and joined the worker threads: all connections are closed and
	 * requests are drained, so nothing of ours remains to flush. Exit
	 * immediately with _exit() to skip C++ static-destructor teardown, whose
	 * ordering across the framework's global singletons is not guaranteed
	 * and otherwise faults during process exit.
	 */
	fflush(nullptr);
	_exit(0);
}
