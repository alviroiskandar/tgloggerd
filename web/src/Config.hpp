// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONFIG_HPP
#define TGLOGGERD_WEB_CONFIG_HPP

#include <cstdint>
#include <cstdlib>
#include <string>

namespace tgweb {

/* getenv with a default; an empty value counts as unset (mirrors the daemon). */
inline std::string env(const char *key, const char *def)
{
	const char *v = getenv(key);
	return (v && *v) ? std::string(v) : std::string(def);
}

/*
 * Settings for one Drogon async DbClient. The web app runs two of these:
 * a read-only client over the logger's tgloggerd schema, and a read-write
 * client over the web app's own tgloggerd_web schema (accounts, audit).
 */
struct DbConfig {
	std::string name;      /* Drogon client name, e.g. "ro" or "app".   */
	std::string host;
	uint16_t    port;
	std::string dbName;
	std::string user;
	std::string password;
	size_t      connNum;   /* Connections in this client's pool.         */
};

struct Config {
	std::string addr;
	uint16_t    port;
	int         threads;

	DbConfig    ro;        /* SELECT-only over tgloggerd.                */
	DbConfig    app;       /* RW over tgloggerd_web (accounts, audit).   */

	/*
	 * Build the configuration from the environment. WEB_DB_HOST/PORT set
	 * the server for both clients and fall back to the daemon's TG_DB_HOST/
	 * PORT so a single deployment can share one set of variables.
	 */
	static Config fromEnv(void)
	{
		Config c;

		c.addr    = env("WEB_LISTEN_ADDR", "127.0.0.1");
		c.port    = (uint16_t)atoi(env("WEB_LISTEN_PORT", "8080").c_str());
		c.threads = atoi(env("WEB_THREADS", "4").c_str());

		std::string host = env("WEB_DB_HOST", env("TG_DB_HOST", "127.0.0.1").c_str());
		uint16_t    port = (uint16_t)atoi(env("WEB_DB_PORT",
					env("TG_DB_PORT", "3306").c_str()).c_str());

		c.ro.name     = "ro";
		c.ro.host     = host;
		c.ro.port     = port;
		c.ro.dbName   = env("WEB_DB_RO_NAME", "tgloggerd");
		c.ro.user     = env("WEB_DB_RO_USER", "web_ro");
		c.ro.password = env("WEB_DB_RO_PASSWORD", "");
		c.ro.connNum  = (size_t)atoi(env("WEB_DB_RO_CONNS", "4").c_str());

		c.app.name     = "app";
		c.app.host     = host;
		c.app.port     = port;
		c.app.dbName   = env("WEB_DB_APP_NAME", "tgloggerd_web");
		c.app.user     = env("WEB_DB_APP_USER", "web_app");
		c.app.password = env("WEB_DB_APP_PASSWORD", "");
		c.app.connNum  = (size_t)atoi(env("WEB_DB_APP_CONNS", "2").c_str());

		return c;
	}
};

} /* namespace tgweb */

#endif /* TGLOGGERD_WEB_CONFIG_HPP */
