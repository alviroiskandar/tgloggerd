// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <mysql/ConnectionPool.hpp>

#include <mysql_driver.h>
#include <cppconn/connection.h>
#include <cppconn/statement.h>

namespace mysql {

namespace {

/*
 * A connection idle in the pool at least this long is pinged before being
 * handed out, so one that went stale while parked (server restart, wait_timeout)
 * is caught and replaced. Connections reused sooner skip the check, so a hot
 * path -- backfill, a busy request -- pays nothing for it. The window only has
 * to be short relative to how long an outage lasts; a second is comfortably
 * both.
 */
constexpr auto kValidateIdleFor = std::chrono::seconds(1);

/*
 * A real round-trip liveness probe. sql::Connection::isValid() pings the
 * server (without silently reconnecting), so this returns false precisely when
 * the server has gone away. Any exception is treated as "dead".
 */
bool connection_alive(sql::Connection *c)
{
	try {
		return c && !c->isClosed() && c->isValid();
	} catch (...) {
		return false;
	}
}

} /* namespace */

ConnectionPool::ConnectionPool(const Config &cfg)
	: cfg_(cfg)
{
	if (cfg_.pool_size == 0)
		cfg_.pool_size = 1;
}

ConnectionPool::~ConnectionPool(void) = default;

std::unique_ptr<sql::Connection> ConnectionPool::create(void)
{
	std::string url = "tcp://" + cfg_.host + ":" + std::to_string(cfg_.port);

	std::unique_ptr<sql::Connection> conn;
	{
		/*
		 * The driver singleton and its connect() are not thread-safe;
		 * serialize just this part. Everything below operates on the
		 * freshly created per-connection object and needs no lock.
		 */
		std::lock_guard<std::mutex> lock(create_mtx_);
		sql::mysql::MySQL_Driver *driver =
			sql::mysql::get_mysql_driver_instance();
		conn.reset(driver->connect(url, cfg_.user, cfg_.password));
	}
	conn->setSchema(cfg_.database);

	std::unique_ptr<sql::Statement> stmt(conn->createStatement());
	stmt->execute("SET NAMES utf8mb4");
	stmt->execute("SET time_zone = '+00:00'");
	return conn;
}

std::unique_ptr<sql::Connection> ConnectionPool::acquire(void)
{
	std::unique_lock<std::mutex> lock(mtx_);

	for (;;) {
		if (!idle_.empty()) {
			Idle slot = std::move(idle_.front());
			idle_.pop_front();

			const bool stale =
				(std::chrono::steady_clock::now() - slot.since)
				>= kValidateIdleFor;

			/*
			 * Ping outside the lock: it is a network round-trip and
			 * must not block other threads' acquire/release. A fresh
			 * enough connection is trusted without a ping.
			 */
			lock.unlock();
			if (!stale || connection_alive(slot.conn.get()))
				return std::move(slot.conn);

			/*
			 * Dead while parked. Close it and forget it existed, so
			 * the count below lets the pool open a replacement.
			 */
			slot.conn.reset();
			lock.lock();
			if (created_ > 0)
				--created_;
			cv_.notify_one();
			continue;
		}

		if (created_ < cfg_.pool_size) {
			++created_;
			lock.unlock();
			try {
				return create();
			} catch (...) {
				lock.lock();
				--created_;
				cv_.notify_one();
				throw;
			}
		}

		cv_.wait(lock);
	}
}

void ConnectionPool::release(std::unique_ptr<sql::Connection> conn)
{
	if (!conn)
		return;

	{
		std::lock_guard<std::mutex> lock(mtx_);
		idle_.push_back(Idle{ std::move(conn),
				      std::chrono::steady_clock::now() });
	}
	cv_.notify_one();
}

void ConnectionPool::discard(std::unique_ptr<sql::Connection> conn)
{
	if (!conn)
		return;

	/* Close it before touching the bookkeeping, and outside the lock. */
	conn.reset();

	std::lock_guard<std::mutex> lock(mtx_);
	if (created_ > 0)
		--created_;
	/* A waiter blocked on an exhausted pool can now create a replacement. */
	cv_.notify_one();
}

} /* namespace mysql */
