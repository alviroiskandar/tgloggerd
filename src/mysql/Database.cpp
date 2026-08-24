// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <mysql/Database.hpp>

#include <cppconn/connection.h>
#include <cppconn/statement.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <cppconn/resultset_metadata.h>
#include <cppconn/exception.h>

#include <stdexcept>

namespace mysql {

namespace {

/* Combine lambdas into a single overload set for std::visit. */
template <class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

void bind_param(sql::PreparedStatement *ps, unsigned idx, const Param &p)
{
	std::visit(overloaded{
		[&](std::monostate)        { ps->setNull(idx, 0); },
		[&](int64_t v)             { ps->setInt64(idx, v); },
		[&](uint64_t v)            { ps->setUInt64(idx, v); },
		[&](double v)              { ps->setDouble(idx, v); },
		[&](const std::string &v)  { ps->setString(idx, v); },
	}, p);
}

/*
 * Borrows a connection from the pool and returns it on scope exit.
 */
class ScopedConnection {
public:
	ScopedConnection(ConnectionPool &pool)
		: pool_(pool)
		, conn_(pool.acquire())
	{
	}

	~ScopedConnection(void)
	{
		if (conn_)
			pool_.release(std::move(conn_));
	}

	sql::Connection *get(void) const { return conn_.get(); }

	/*
	 * Give the connection up as dead instead of returning it to the pool.
	 * After this the destructor releases nothing.
	 */
	void discard(void)
	{
		if (conn_)
			pool_.discard(std::move(conn_));
	}

private:
	ConnectionPool			&pool_;
	std::unique_ptr<sql::Connection> conn_;
};

/*
 * A query failed because the connection to the server was lost, as opposed to
 * a SQL-level error (bad syntax, constraint violation). The two are handled
 * very differently: an ordinary error leaves a perfectly good connection that
 * goes back to the pool, while a lost connection must be discarded so a dead
 * one never circulates. Derives from runtime_error so callers that only catch
 * std::exception are unaffected.
 */
struct ConnectionLost : std::runtime_error {
	using std::runtime_error::runtime_error;
};

/*
 * True when a SQLException signals the connection dropped rather than the
 * statement being rejected. These client error codes mean the socket to the
 * server is gone; an autocommit statement that hits one never committed,
 * because the server rolls back the in-flight transaction when the client
 * disappears.
 */
bool is_connection_lost(const sql::SQLException &e)
{
	switch (e.getErrorCode()) {
	case 2006: /* CR_SERVER_GONE_ERROR  -- server closed the socket */
	case 2013: /* CR_SERVER_LOST        -- dropped in the middle of a query */
	case 2055: /* CR_SERVER_LOST_EXTENDED */
	case 4031: /* ER_CLIENT_INTERACTION_TIMEOUT -- server closed an idle conn */
		return true;
	default:
		break;
	}
	/* SQLSTATE class "08" is "connection exception"; backstops the codes. */
	const std::string &st = e.getSQLState();
	return st.size() >= 2 && st[0] == '0' && st[1] == '8';
}

/* Translate a JDBC SQLException into our exception hierarchy and rethrow. */
[[noreturn]] void rethrow_sql(const char *ctx, const sql::SQLException &e)
{
	std::string msg = std::string(ctx) + e.what();
	if (is_connection_lost(e))
		throw ConnectionLost(msg);
	throw std::runtime_error(msg);
}

uint64_t do_execute(sql::Connection *conn, const std::string &sql,
		    const std::vector<Param> &params)
{
	try {
		std::unique_ptr<sql::PreparedStatement> ps(
			conn->prepareStatement(sql));
		for (size_t i = 0; i < params.size(); i++)
			bind_param(ps.get(), (unsigned)(i + 1), params[i]);
		return (uint64_t)ps->executeUpdate();
	} catch (sql::SQLException &e) {
		rethrow_sql("MySQL execute failed: ", e);
	}
}

uint64_t do_insert(sql::Connection *conn, const std::string &sql,
		   const std::vector<Param> &params)
{
	try {
		std::unique_ptr<sql::PreparedStatement> ps(
			conn->prepareStatement(sql));
		for (size_t i = 0; i < params.size(); i++)
			bind_param(ps.get(), (unsigned)(i + 1), params[i]);
		ps->executeUpdate();

		std::unique_ptr<sql::Statement> st(conn->createStatement());
		std::unique_ptr<sql::ResultSet> rs(
			st->executeQuery("SELECT LAST_INSERT_ID()"));
		if (rs->next())
			return rs->getUInt64(1);
		return 0;
	} catch (sql::SQLException &e) {
		rethrow_sql("MySQL insert failed: ", e);
	}
}

std::vector<Row> do_query(sql::Connection *conn, const std::string &sql,
			  const std::vector<Param> &params)
{
	try {
		std::unique_ptr<sql::PreparedStatement> ps(
			conn->prepareStatement(sql));
		for (size_t i = 0; i < params.size(); i++)
			bind_param(ps.get(), (unsigned)(i + 1), params[i]);

		std::unique_ptr<sql::ResultSet> rs(ps->executeQuery());
		unsigned cols = rs->getMetaData()->getColumnCount();

		std::vector<Row> out;
		while (rs->next()) {
			Row row;
			row.reserve(cols);
			for (unsigned c = 1; c <= cols; c++) {
				if (rs->isNull(c))
					row.emplace_back(std::nullopt);
				else
					row.emplace_back(rs->getString(c));
			}
			out.push_back(std::move(row));
		}
		return out;
	} catch (sql::SQLException &e) {
		rethrow_sql("MySQL query failed: ", e);
	}
}

} /* namespace */

Database::Database(const Config &cfg)
	: pool_(cfg)
{
}

Database::~Database(void) = default;

uint64_t Database::execute(const std::string &sql,
			   const std::vector<Param> &params)
{
	/*
	 * A write is not retried automatically. A lost-connection error means
	 * this statement never committed, but retrying still risks a double
	 * apply in the rare case the commit landed and only its acknowledgement
	 * was lost. So the dead connection is discarded -- never handed back to
	 * the pool -- and the error surfaced; the caller's next write runs on a
	 * healthy connection acquire() has validated.
	 */
	ScopedConnection conn(pool_);
	try {
		return do_execute(conn.get(), sql, params);
	} catch (const ConnectionLost &) {
		conn.discard();
		throw;
	}
}

uint64_t Database::insert(const std::string &sql,
			  const std::vector<Param> &params)
{
	/* Same reasoning as execute(): discard on loss, do not re-run a write. */
	ScopedConnection conn(pool_);
	try {
		return do_insert(conn.get(), sql, params);
	} catch (const ConnectionLost &) {
		conn.discard();
		throw;
	}
}

std::vector<Row> Database::query(const std::string &sql,
				 const std::vector<Param> &params)
{
	/*
	 * A read is idempotent, so a lost connection is retried once on a fresh
	 * one. acquire() has already validated the connection it hands out, so a
	 * failure here means it died in the narrow window during the query
	 * itself; the retry gets a healthy connection, and a second loss is
	 * surfaced rather than looped on.
	 */
	for (int attempt = 0; ; attempt++) {
		ScopedConnection conn(pool_);
		try {
			return do_query(conn.get(), sql, params);
		} catch (const ConnectionLost &) {
			conn.discard();
			if (attempt >= 1)
				throw;
		}
	}
}

void Database::transaction(const std::function<void(Transaction &)> &fn)
{
	ScopedConnection conn(pool_);
	sql::Connection *c = conn.get();

	try {
		c->setAutoCommit(false);
		Transaction txn(c);
		fn(txn);
		c->commit();
		c->setAutoCommit(true);
	} catch (const ConnectionLost &) {
		/*
		 * The connection dropped mid-transaction; the server has already
		 * rolled the transaction back, and rollback() here would only
		 * fail again. Discard the dead connection rather than pool it,
		 * and do not retry -- replaying a multi-statement unit is the
		 * caller's decision, not ours.
		 */
		conn.discard();
		throw std::runtime_error(
			"MySQL transaction failed: connection lost");
	} catch (sql::SQLException &e) {
		if (is_connection_lost(e)) {
			/* Lost during commit/setAutoCommit: same handling. */
			conn.discard();
			throw std::runtime_error(
				std::string("MySQL transaction failed: ") +
				e.what());
		}
		try {
			c->rollback();
			c->setAutoCommit(true);
		} catch (...) {
			/* Best effort; report the original failure below. */
		}
		throw std::runtime_error(
			std::string("MySQL transaction failed: ") + e.what());
	} catch (...) {
		try {
			c->rollback();
			c->setAutoCommit(true);
		} catch (...) {
			/* Best effort; rethrow the original exception. */
		}
		throw;
	}
}

uint64_t Transaction::execute(const std::string &sql,
			      const std::vector<Param> &params)
{
	return do_execute(conn_, sql, params);
}

uint64_t Transaction::insert(const std::string &sql,
			     const std::vector<Param> &params)
{
	return do_insert(conn_, sql, params);
}

std::vector<Row> Transaction::query(const std::string &sql,
				    const std::vector<Param> &params)
{
	return do_query(conn_, sql, params);
}

} /* namespace mysql */
