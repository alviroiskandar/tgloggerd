// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_DAO_SEARCH_HPP
#define TGLOGGERD_WEB_DAO_SEARCH_HPP

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/*
 * Advanced, entity-generic search over the tgloggerd schema.
 *
 * A search is a flat list of conditions [{c,o,v,n}] (column key, operator,
 * value, AND/OR connector to the NEXT condition), evaluated against a curated
 * per-entity SearchSchema. Only bound `?` VALUES are ever user data; every
 * other SQL token (column expressions, operators, connectors, table names,
 * sort column, order) is emitted from server-controlled allowlists, so the
 * generated SQL is injection-safe by construction. Like dao::browse, every
 * string placed on the returned JSON is already Render::esc()-escaped.
 */
namespace tgweb::dao::search {

/* Operator flags; a field advertises the subset it allows as a bitmask. */
enum Op : uint32_t {
	OP_EQ        = 1u << 0,
	OP_NE        = 1u << 1,
	OP_LT        = 1u << 2,
	OP_GT        = 1u << 3,
	OP_LE        = 1u << 4,
	OP_GE        = 1u << 5,
	OP_LIKE      = 1u << 6,  /* SQL LIKE; value bound verbatim (% is opt-in) */
	OP_NLIKE     = 1u << 7,
	OP_ISNULL    = 1u << 8,
	OP_ISNOTNULL = 1u << 9,
	OP_CLIKE     = 1u << 10, /* "%LIKE%": contains; value wrapped %..% server-side */
	OP_NCLIKE    = 1u << 11, /* "NOT %LIKE%": negation of contains */
	OP_MATCH     = 1u << 12, /* "matches": FullText MATCH..AGAINST (boolean mode) */
};

/*
 * FullText columns are matched with `MATCH(<expr>) AGAINST(? IN BOOLEAN MODE)`
 * (only OP_MATCH), so they need a FULLTEXT index and support Telegram-style
 * word queries (`+must -not "phrase"`); an ordinary Text column uses LIKE.
 */
enum class FType { Text, Int, Bool, Datetime, Enum, FullText };

/*
 * Column: the condition is `<expr> <op> ?` (or `<expr> IS [NOT] NULL`).
 * Exists : the condition is `[NOT] EXISTS (SELECT 1 FROM <exTable>
 *          WHERE x.<exFk> = <idCol> <exExtra> AND <exCol> {=|LIKE} ?)`.
 *          For Exists fields, `=`/LIKE mean "ever matched" and `!=`/`NOT LIKE`
 *          mean "never matched" (NOT EXISTS) -- never the misleading "ever had
 *          a value != x". Exists fields therefore never allow IS [NOT] NULL.
 */
enum class FKind { Column, Exists };

struct SearchField {
	std::string_view key;      /* the `c` value clients send */
	std::string_view label;    /* human label for the UI */
	FType            type;
	FKind            kind;
	std::string_view expr;     /* Column: the LHS SQL expression */
	std::string_view exTable;  /* Exists: "table x" (alias must be x) */
	std::string_view exCol;    /* Exists: "x.<col>" being compared */
	std::string_view exExtra;  /* Exists: extra predicate or "" */
	uint32_t         ops;      /* allowed-operator bitmask */
	bool             sortable;
	bool             display;  /* part of the default result columns */
	std::string_view enumVals; /* Enum: CSV of allowed values, else "" */
	/* Column only: SQL for the bound value on the RHS of the comparison; must
	 * contain exactly one `?`. Empty means a plain "?". Lets a field wrap its
	 * parameter, e.g. "UNHEX(?)" to match a BINARY column against a hex string
	 * while still using its index (a functional LHS like HEX(col) could not).
	 * Defaulted so existing field tables need not list it. */
	std::string_view bindTmpl = {};
};

/*
 * A displayed table column. `type` picks how the cell is rendered (photo, id,
 * name, username, bool, int, datetime, text, longtext); `sortKey` is the
 * registry field key to sort by when the header is clicked ("" = not sortable).
 * The DAO emits `cols` (this list) plus each row as a positional array aligned
 * to it -- like api2.php's keys/data -- so field keys are not repeated per row.
 */
struct DisplayCol {
	std::string_view key;
	std::string_view label;
	std::string_view type;
	std::string_view sortKey;
};

struct SearchSchema {
	std::string_view fromJoin;     /* "users u LEFT JOIN telegram_user_extra_info e ON ..." */
	std::string_view selectCols;   /* display projection for the page query */
	std::string_view idCol;        /* "u.id": tiebreaker + Exists join target */
	std::string_view exFk;         /* FK column in Exists tables ("user_id") */
	std::string_view defaultSort;  /* default ORDER BY expression */
	std::string_view defaultOrder; /* "ASC" | "DESC" */
	const SearchField *fields;     /* searchable field registry */
	size_t            nFields;
	const DisplayCol *cols;        /* displayed table columns, in order */
	size_t            nCols;
	/* Project one result row to a positional, escaped JSON array in `cols`
	 * order (photo cell = raw file id, later tokenised by the controller). */
	nlohmann::json (*mapRow)(const drogon::orm::Row &);
	/* FROM/JOIN for the COUNT(*) query, when it can be cheaper than `fromJoin`
	 * -- e.g. dropping display-only LEFT JOINs the WHERE never references (a
	 * count over millions of rows otherwise times out). Empty = use fromJoin.
	 * Must still satisfy every field's WHERE expression. */
	std::string_view countFrom = {};
	/* Cap COUNT(*) at this many rows for huge tables (0 = exact). A filter on a
	 * non-indexed column would otherwise scan millions of rows and time out;
	 * the cap bounds it (the total shows as this value once reached, which is
	 * plenty for paging). Fast/indexed counts still return their exact value if
	 * below the cap. */
	int countCap = 0;
};

/* One parsed condition from the search JSON. */
struct Condition {
	std::string c, o, v, n;
	bool hasV = false; /* whether "v" was present (IS NULL omits it) */
};

struct Request {
	std::vector<Condition> conds;
	std::string sort;   /* field key; empty -> schema default */
	std::string order;  /* "asc"|"desc"; empty -> schema default */
	int  limit  = 10;
	int  offset = 0;
	bool debug  = false; /* already AND-ed with is-admin by the caller */
};

/* Limits (documented in web/docs/search-api.md). */
constexpr int MAX_CONDS  = 16;
constexpr int MAX_EXISTS = 4;
constexpr int MAX_VLEN   = 512;
constexpr int MAX_LIMIT  = 1000;
constexpr int MAX_OFFSET = 500000;

/* The registered per-entity schemas. */
const SearchSchema &usersSchema(void);
const SearchSchema &groupsSchema(void);
const SearchSchema &filesSchema(void);
const SearchSchema &privateMessagesSchema(void);
const SearchSchema &groupMessagesSchema(void);

/* Look up a schema by entity name ("users", "groups"); nullptr if unknown. */
const SearchSchema *schemaByName(const std::string &entity);

/*
 * Parse the `search` query-param JSON ([{c,o,v,n}, ...]) into conditions.
 * An empty string is a valid "browse all" (out stays empty). Returns false and
 * sets err (400-worthy) on malformed input. Does not validate field/operator
 * names -- run() does that against the schema.
 */
bool parseConditions(const std::string &raw, std::vector<Condition> &out,
		     std::string &err);

/*
 * Run a search. On success returns {fields, columns, rows, total, limit,
 * offset, sort, order[, debug]}. On a validation error returns {"error": msg}
 * (the controller maps that to HTTP 400). Never throws for user input.
 */
drogon::Task<nlohmann::json> run(drogon::orm::DbClientPtr db,
				 const SearchSchema &schema, Request req);

} /* namespace tgweb::dao::search */

#endif /* TGLOGGERD_WEB_DAO_SEARCH_HPP */
