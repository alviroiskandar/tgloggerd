// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_MCP_FILTER_HPP
#define TGLOGGERD_WEB_MCP_FILTER_HPP

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

/*
 * A recursive filter grammar, compiled to parameterised SQL.
 *
 * The web UI's /v1/search takes a FLAT list of conditions joined by AND/OR with
 * MySQL precedence, which can express only DNF -- (a AND b) OR (c AND d) -- and
 * has no NOT at all. That is not enough for a caller writing arbitrary queries,
 * so this is a tree:
 *
 *   leaf    { "field": "text", "op": "match", "value": "kernel panic" }
 *   and     { "and": [ node, node, ... ] }
 *   or      { "or":  [ node, node, ... ] }
 *   not     { "not": node }
 *
 * Compilation emits fully parenthesised SQL, so precedence is explicit rather
 * than inherited from MySQL.
 *
 * Injection posture, identical to the existing search layer: `field` and `op`
 * are looked up in an allowlist and emitted as server constants; only `value`
 * reaches SQL, always as a bound parameter. An unknown field is an error that
 * names the key -- it is never concatenated.
 */
namespace tgweb::mcp::filter {

enum class FType {
	Int,      /* bound as-is, must parse as an integer */
	Text,     /* LIKE-able string */
	FullText, /* MATCH ... AGAINST; only the `match` operator applies */
	Enum,     /* must be one of `enumVals` */
	DateTs,   /* ISO-8601 or unix seconds in, unix seconds out */
	Bool,     /* true/false */
};

struct Field {
	std::string_view key;       /* what the caller writes */
	std::string_view expr;      /* the SQL left-hand side */
	FType            type;
	std::string_view enumVals;  /* CSV, for FType::Enum */
	std::string_view desc;      /* shown in the tool's inputSchema */
};

struct Schema {
	const Field *fields;
	size_t       nFields;

	const Field *find(const std::string &key) const;
};

struct Compiled {
	/* A parenthesised boolean expression, or "" when no filter was given. */
	std::string              sql;
	std::vector<std::string> binds;

	/*
	 * The first full-text condition, when there is one.
	 *
	 * This exists because ordering a MATCH by date is a trap: MySQL cannot
	 * use the fulltext index for the sort, so it materialises and filesorts
	 * every hit -- on a 4.6M-row table a broad match times out. Ordering by
	 * relevance instead walks the index in order, so LIMIT stops early.
	 * The caller needs the expression and the search term to build that
	 * ORDER BY, and the term must be bound again there.
	 */
	std::string ftExpr;
	std::string ftValue;

	bool hasFullText(void) const { return !ftExpr.empty(); }
};

/*
 * Guards. A filter arrives from a language model, so it may be arbitrarily
 * shaped by accident; these stop a pathological tree becoming a denial of
 * service before it reaches the database.
 */
constexpr int    MAX_DEPTH = 8;
constexpr int    MAX_NODES = 64;
constexpr size_t MAX_VALUE_LEN = 512;
constexpr size_t MAX_IN_ITEMS = 100;

/*
 * Compile `node` against `schema`. Throws gwmcp::ToolError with a message
 * written for the caller to read -- these are user errors, not bugs, and the
 * caller is a model that can correct itself if told what was wrong.
 *
 * A null or absent node compiles to an empty result, meaning "no filter".
 */
Compiled compile(const Schema &schema, const nlohmann::json &node);

/*
 * Parse a date the way a filter value is parsed: an ISO-8601 date or datetime,
 * or an integer unix timestamp (as a number or a string). Throws ToolError with
 * a readable message on anything else, so tools accepting a date range behave
 * identically to the same field inside a filter.
 */
long long parseDate(const nlohmann::json &v);

/* The operator list, for documentation and inputSchema generation. */
std::vector<std::string> operatorNames(void);

/* A human-readable description of a schema's fields, for a tool description. */
std::string describeFields(const Schema &schema);

} /* namespace tgweb::mcp::filter */

#endif /* TGLOGGERD_WEB_MCP_FILTER_HPP */
