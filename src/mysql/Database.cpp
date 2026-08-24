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
		pool_.release(std::move(conn_));
	}

	sql::Connection *get(void) const { return conn_.get(); }

private:
	ConnectionPool			&pool_;
	std::unique_ptr<sql::Connection> conn_;
};

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
		throw std::runtime_error(std::string("MySQL execute failed: ") +
					 e.what());
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
		throw std::runtime_error(std::string("MySQL insert failed: ") +
					 e.what());
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
		throw std::runtime_error(std::string("MySQL query failed: ") +
					 e.what());
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
	ScopedConnection conn(pool_);
	return do_execute(conn.get(), sql, params);
}

uint64_t Database::insert(const std::string &sql,
			  const std::vector<Param> &params)
{
	ScopedConnection conn(pool_);
	return do_insert(conn.get(), sql, params);
}

std::vector<Row> Database::query(const std::string &sql,
				 const std::vector<Param> &params)
{
	ScopedConnection conn(pool_);
	return do_query(conn.get(), sql, params);
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
	} catch (sql::SQLException &e) {
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
