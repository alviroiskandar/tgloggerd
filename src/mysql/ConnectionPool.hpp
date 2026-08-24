// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#ifndef MYSQL__CONNECTION_POOL_HPP
#define MYSQL__CONNECTION_POOL_HPP

#include <deque>
#include <mutex>
#include <chrono>
#include <memory>
#include <string>
#include <cstdint>
#include <condition_variable>

namespace sql {
class Connection;
} /* namespace sql */

namespace mysql {

/*
 * Settings used to open connections to a MySQL server.
 */
struct Config {
	std::string	host = "127.0.0.1";
	uint16_t	port = 3306;
	std::string	user;
	std::string	password;
	std::string	database;
	size_t		pool_size = 4;
};

/*
 * A small thread-safe pool of MySQL (JDBC) connections.
 *
 * Connections are created lazily up to Config::pool_size. Callers borrow
 * a connection with acquire() and hand it back with release(); acquire()
 * blocks when the pool is exhausted until a connection is returned.
 *
 * The pool heals itself when the server drops a connection out from under it
 * (a restart, an idle timeout, a network blip). A connection that has sat idle
 * in the pool is pinged before it is handed back out, and a dead one is thrown
 * away and replaced rather than returned to a caller; a connection that dies
 * while checked out is handed to discard() instead of release(). Without this,
 * one dead connection would circulate forever, failing every query drawn from
 * it until the whole process was restarted.
 */
class ConnectionPool {
public:
	explicit ConnectionPool(const Config &cfg);
	~ConnectionPool(void);

	ConnectionPool(const ConnectionPool &) = delete;
	ConnectionPool &operator=(const ConnectionPool &) = delete;

	/*
	 * Borrow a connection, blocking until one is available. The returned
	 * connection has been verified live (see the class comment), so a
	 * caller never receives one the server has already closed.
	 */
	std::unique_ptr<sql::Connection> acquire(void);

	/* Return a healthy connection to the pool for reuse. */
	void release(std::unique_ptr<sql::Connection> conn);

	/*
	 * Drop a connection instead of returning it: for one that failed with a
	 * connection-loss error while checked out. It is closed rather than
	 * pooled, and the pool's count is decremented so acquire() opens a fresh
	 * one in its place.
	 */
	void discard(std::unique_ptr<sql::Connection> conn);

	const Config &config(void) const { return cfg_; }

private:
	std::unique_ptr<sql::Connection> create(void);

	/* A pooled connection plus the moment it was last returned idle. */
	struct Idle {
		std::unique_ptr<sql::Connection> conn;
		std::chrono::steady_clock::time_point since;
	};

	Config					cfg_;
	std::mutex				mtx_;
	std::condition_variable			cv_;
	std::deque<Idle>			idle_;
	size_t					created_ = 0;

	/*
	 * Serializes connection creation. The JDBC driver singleton
	 * (get_mysql_driver_instance) is not thread-safe, and create() runs
	 * outside mtx_ so several threads can create connections at once;
	 * this guards the driver interaction without blocking acquire/release
	 * of already-open connections.
	 */
	std::mutex				create_mtx_;
};

} /* namespace mysql */

#endif /* #ifndef MYSQL__CONNECTION_POOL_HPP */
