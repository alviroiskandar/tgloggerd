// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "mcp/Filter.hpp"

#include <gwmcp/Errors.hpp>

#include <cstdlib>
#include <ctime>
#include <string>

namespace tgweb::mcp::filter {

namespace {

using gwmcp::ToolError;

[[noreturn]] void fail(const std::string &msg)
{
	throw ToolError(msg);
}

bool csvHas(std::string_view csv, const std::string &v)
{
	size_t p = 0;
	while (p <= csv.size()) {
		const size_t c = csv.find(',', p);
		const size_t e = (c == std::string_view::npos) ? csv.size() : c;
		if (csv.substr(p, e - p) == v)
			return true;
		if (c == std::string_view::npos)
			break;
		p = c + 1;
	}
	return false;
}

/* Escape the caller's own % and _ so a LIKE means what they wrote. */
std::string likeEscape(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s) {
		if (c == '%' || c == '_' || c == '\\')
			out += '\\';
		out += c;
	}
	return out;
}

/*
 * telegram_*_messages.date is a unix BIGINT, but a caller thinks in dates. So
 * accept both: a bare integer passes through, an ISO-8601-ish string is
 * converted. Anything else is an error rather than a silent 0, because 0 is a
 * legitimate stored value (some messages genuinely have date = 0) and would
 * quietly match them.
 */
std::string toUnixSeconds(const nlohmann::json &v)
{
	if (v.is_number_integer())
		return std::to_string(v.get<long long>());

	if (!v.is_string())
		fail("a date value must be a string like \"2026-01-31\" or "
		     "\"2026-01-31T12:00:00Z\", or an integer unix timestamp");

	std::string s = v.get<std::string>();
	if (s.size() > 32)
		fail("date value is too long");

	/* All digits: already a unix timestamp handed over as a string. */
	bool digits = !s.empty();
	for (char c : s) {
		if (c < '0' || c > '9') {
			digits = false;
			break;
		}
	}
	if (digits)
		return s;

	struct tm tm_v;
	memset(&tm_v, 0, sizeof(tm_v));
	const char *p = strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &tm_v);
	if (!p)
		p = strptime(s.c_str(), "%Y-%m-%d %H:%M:%S", &tm_v);
	if (!p)
		p = strptime(s.c_str(), "%Y-%m-%d", &tm_v);
	if (!p)
		fail("could not parse date \"" + s +
		     "\"; use YYYY-MM-DD or YYYY-MM-DDTHH:MM:SSZ");

	/* timegm, not mktime: stored timestamps are UTC. */
	const time_t t = timegm(&tm_v);
	return std::to_string((long long)t);
}

/* Turn one JSON value into the string that will be bound. */
std::string bindValue(const Field &f, const nlohmann::json &v)
{
	if (v.is_null())
		fail("field \"" + std::string(f.key) + "\" needs a value");
	if (v.is_object() || v.is_array())
		fail("field \"" + std::string(f.key) +
		     "\" needs a scalar value");

	switch (f.type) {
	case FType::DateTs:
		return toUnixSeconds(v);
	case FType::Int: {
		if (v.is_number_integer())
			return std::to_string(v.get<long long>());
		if (!v.is_string())
			fail("field \"" + std::string(f.key) +
			     "\" needs an integer");
		const std::string s = v.get<std::string>();
		if (s.empty() || s.size() > 24)
			fail("field \"" + std::string(f.key) +
			     "\" needs an integer");
		for (size_t i = 0; i < s.size(); i++) {
			if (i == 0 && (s[i] == '-' || s[i] == '+'))
				continue;
			if (s[i] < '0' || s[i] > '9')
				fail("field \"" + std::string(f.key) +
				     "\" needs an integer, got \"" + s + "\"");
		}
		return s;
	}
	case FType::Bool:
	case FType::Presence:
		if (v.is_boolean())
			return v.get<bool>() ? "1" : "0";
		if (v.is_number_integer())
			return v.get<long long>() ? "1" : "0";
		fail("field \"" + std::string(f.key) + "\" needs true or false");
	case FType::Enum: {
		if (!v.is_string())
			fail("field \"" + std::string(f.key) +
			     "\" needs one of: " + std::string(f.enumVals));
		const std::string s = v.get<std::string>();
		if (!csvHas(f.enumVals, s))
			fail("\"" + s + "\" is not valid for field \"" +
			     std::string(f.key) + "\"; expected one of: " +
			     std::string(f.enumVals));
		return s;
	}
	case FType::Text:
	case FType::FullText:
	default: {
		if (!v.is_string())
			fail("field \"" + std::string(f.key) +
			     "\" needs a string");
		const std::string s = v.get<std::string>();
		if (s.size() > MAX_VALUE_LEN)
			fail("value for \"" + std::string(f.key) +
			     "\" is too long (max " +
			     std::to_string(MAX_VALUE_LEN) + ")");
		return s;
	}
	}
}

struct Ctx {
	const Schema             &schema;
	std::vector<std::string> &binds;
	int                       nodes = 0;
	/* First fulltext condition seen; see Compiled::ftExpr. */
	std::string               ftExpr;
	std::string               ftValue;
};

std::string compileNode(Ctx &ctx, const nlohmann::json &node, int depth);

std::string compileGroup(Ctx &ctx, const nlohmann::json &arr, int depth,
			 const char *joiner)
{
	if (!arr.is_array())
		fail(std::string("\"") + joiner + "\" needs an array of filters");
	if (arr.empty())
		fail(std::string("\"") + joiner + "\" needs at least one filter");

	std::string out = "(";
	bool first = true;
	for (const auto &child : arr) {
		if (!first)
			out += std::string(" ") + joiner + " ";
		out += compileNode(ctx, child, depth + 1);
		first = false;
	}
	out += ")";
	return out;
}

std::string compileLeaf(Ctx &ctx, const nlohmann::json &node)
{
	if (!node.contains("field") || !node["field"].is_string())
		fail("a filter needs a string \"field\"");

	const std::string key = node["field"].get<std::string>();
	const Field *f = ctx.schema.find(key);
	if (!f)
		fail("unknown field \"" + key + "\"");

	const std::string op = node.contains("op") && node["op"].is_string()
				       ? node["op"].get<std::string>()
				       : std::string("=");

	const std::string expr(f->expr);
	const bool hasValue = node.contains("value") && !node["value"].is_null();

	/* Fulltext is its own world: MATCH cannot be combined with LIKE or
	 * comparison, and the column is not usefully comparable otherwise. */
	if (f->type == FType::FullText) {
		if (op != "match")
			fail("field \"" + key +
			     "\" is a full-text field; the only operator is "
			     "\"match\"");
		if (!hasValue)
			fail("\"match\" needs a value");
		const std::string v = bindValue(*f, node["value"]);
		ctx.binds.push_back(v);
		if (ctx.ftExpr.empty()) {
			ctx.ftExpr = expr;
			ctx.ftValue = v;
		}
		return "(MATCH(" + expr + ") AGAINST(? IN BOOLEAN MODE))";
	}
	if (op == "match")
		fail("field \"" + key + "\" does not support \"match\"");

	/* How this field spells a bound value; "?" unless it needs converting. */
	const std::string ph = f->valExpr.empty() ? std::string("?")
						  : std::string(f->valExpr);

	if (op == "is_null" || op == "is_not_null") {
		/*
		 * A 0-sentinel column has no NULL to test, so the same question
		 * -- did this ever happen? -- is asked of the value instead.
		 */
		if (f->type == FType::Presence)
			return op == "is_null" ? "(" + expr + " = 0)"
					       : "(" + expr + " <> 0)";
		return op == "is_null" ? "(" + expr + " IS NULL)"
				       : "(" + expr + " IS NOT NULL)";
	}

	if (!hasValue)
		fail("operator \"" + op + "\" on field \"" + key +
		     "\" needs a value");
	const nlohmann::json &v = node["value"];

	if (op == "in") {
		if (!v.is_array() || v.empty())
			fail("\"in\" needs a non-empty array of values");
		if (v.size() > MAX_IN_ITEMS)
			fail("\"in\" accepts at most " +
			     std::to_string(MAX_IN_ITEMS) + " values");
		std::string out = "(" + expr + " IN (";
		for (size_t i = 0; i < v.size(); i++) {
			if (i)
				out += ", ";
			out += ph;
			ctx.binds.push_back(bindValue(*f, v[i]));
		}
		out += "))";
		return out;
	}

	if (op == "between") {
		if (!v.is_array() || v.size() != 2)
			fail("\"between\" needs an array of exactly two values");
		ctx.binds.push_back(bindValue(*f, v[0]));
		ctx.binds.push_back(bindValue(*f, v[1]));
		return "(" + expr + " BETWEEN " + ph + " AND " + ph + ")";
	}

	static const struct {
		const char *op;
		const char *sql;
	} kCmp[] = {
		{ "=", "=" },   { "!=", "!=" }, { "<", "<" },
		{ ">", ">" },   { "<=", "<=" }, { ">=", ">=" },
	};
	for (const auto &c : kCmp) {
		if (op == c.op) {
			ctx.binds.push_back(bindValue(*f, v));
			return "(" + expr + " " + c.sql + " " + ph + ")";
		}
	}

	if (op == "contains" || op == "not_contains" || op == "starts_with") {
		if (f->type != FType::Text)
			fail("operator \"" + op + "\" needs a text field; \"" +
			     key + "\" is not one");
		const std::string raw = bindValue(*f, v);
		if (op == "starts_with")
			ctx.binds.push_back(likeEscape(raw) + "%");
		else
			ctx.binds.push_back("%" + likeEscape(raw) + "%");
		const char *sqlOp = (op == "not_contains") ? "NOT LIKE" : "LIKE";
		return "(" + expr + " " + sqlOp + " ? ESCAPE '\\\\')";
	}

	fail("unknown operator \"" + op + "\" on field \"" + key + "\"");
}

std::string compileNode(Ctx &ctx, const nlohmann::json &node, int depth)
{
	if (depth > MAX_DEPTH)
		fail("filter is nested too deeply (max " +
		     std::to_string(MAX_DEPTH) + ")");
	if (++ctx.nodes > MAX_NODES)
		fail("filter has too many conditions (max " +
		     std::to_string(MAX_NODES) + ")");
	if (!node.is_object())
		fail("each filter must be a JSON object");

	const bool hasAnd = node.contains("and");
	const bool hasOr = node.contains("or");
	const bool hasNot = node.contains("not");
	const bool hasField = node.contains("field");

	const int forms = (hasAnd ? 1 : 0) + (hasOr ? 1 : 0) + (hasNot ? 1 : 0) +
			  (hasField ? 1 : 0);
	if (forms == 0)
		fail("a filter must be a condition (\"field\") or a group "
		     "(\"and\", \"or\", \"not\")");
	if (forms > 1)
		fail("a filter must be exactly one of \"field\", \"and\", "
		     "\"or\" or \"not\" -- not several at once");

	if (hasAnd)
		return compileGroup(ctx, node["and"], depth, "AND");
	if (hasOr)
		return compileGroup(ctx, node["or"], depth, "OR");
	if (hasNot)
		return "(NOT " + compileNode(ctx, node["not"], depth + 1) + ")";
	return compileLeaf(ctx, node);
}

} /* namespace */

long long parseDate(const nlohmann::json &v)
{
	return strtoll(toUnixSeconds(v).c_str(), nullptr, 10);
}

const Field *Schema::find(const std::string &key) const
{
	for (size_t i = 0; i < nFields; i++) {
		if (fields[i].key == key)
			return &fields[i];
	}
	return nullptr;
}

Compiled compile(const Schema &schema, const nlohmann::json &node)
{
	Compiled out;
	if (node.is_null() || (node.is_object() && node.empty()))
		return out;

	Ctx ctx{ schema, out.binds, 0, {}, {} };
	out.sql = compileNode(ctx, node, 0);
	out.ftExpr = ctx.ftExpr;
	out.ftValue = ctx.ftValue;
	return out;
}

std::vector<std::string> operatorNames(void)
{
	return { "=",  "!=",       "<",            ">",
		 "<=", ">=",       "contains",     "not_contains",
		 "starts_with",     "in",           "between",
		 "is_null",         "is_not_null",  "match" };
}

std::string describeFields(const Schema &schema)
{
	std::string out;
	for (size_t i = 0; i < schema.nFields; i++) {
		const Field &f = schema.fields[i];
		out += "  ";
		out += f.key;
		out += " (";
		switch (f.type) {
		case FType::Int:      out += "integer"; break;
		case FType::Text:     out += "text"; break;
		case FType::FullText: out += "full-text, use op \"match\""; break;
		case FType::Enum:     out += "one of: ";
				      out += f.enumVals;
				      break;
		case FType::DateTs:   out += "date"; break;
		case FType::Bool:     out += "boolean"; break;
		case FType::Presence: out += "presence, use op is_null / "
					     "is_not_null";
				      break;
		}
		out += ")";
		if (!f.desc.empty()) {
			out += " -- ";
			out += f.desc;
		}
		out += "\n";
	}
	return out;
}

} /* namespace tgweb::mcp::filter */
