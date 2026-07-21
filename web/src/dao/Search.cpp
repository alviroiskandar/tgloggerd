// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "dao/Search.hpp"

#include "views/Render.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tgweb::dao::search {

namespace {

using tgweb::views::Render;

/* --- small escaping/formatting helpers (mirror dao::browse) --------------- */

std::string escCol(const drogon::orm::Row &r, const char *col)
{
	if (r[col].isNull())
		return std::string();
	return Render::esc(r[col].as<std::string>());
}

std::string displayName(const drogon::orm::Row &r)
{
	std::string first = r["first_name"].isNull()
				    ? "" : r["first_name"].as<std::string>();
	std::string last = r["last_name"].isNull()
				   ? "" : r["last_name"].as<std::string>();
	std::string name = first;
	if (!last.empty()) {
		if (!name.empty())
			name += " ";
		name += last;
	}
	if (name.empty())
		name = "(no name)";
	return Render::esc(name);
}

/* "%q%" with LIKE metacharacters escaped; value is bound, so match-only. */
std::string likePattern(const std::string &q)
{
	std::string e;
	for (char c : q) {
		if (c == '\\' || c == '%' || c == '_')
			e += '\\';
		e += c;
	}
	return "%" + e + "%";
}

bool rowBool(const drogon::orm::Row &r, const char *col)
{
	return !r[col].isNull() && r[col].as<int>() != 0;
}

/* --- operator table ------------------------------------------------------- */

struct OpTok {
	Op          op;
	const char *tok;    /* what the client sends as `o` */
	const char *sqlTok; /* SQL token for a Column comparison, or nullptr */
};

/* Order here is the order the UI shows operators in. */
const OpTok kOps[] = {
	{ OP_EQ,        "=",           "="        },
	{ OP_NE,        "!=",          "!="       },
	{ OP_LT,        "<",           "<"        },
	{ OP_GT,        ">",           ">"        },
	{ OP_LE,        "<=",          "<="       },
	{ OP_GE,        ">=",          ">="       },
	{ OP_LIKE,      "LIKE",        "LIKE"     },
	{ OP_NLIKE,     "NOT LIKE",    "NOT LIKE" },
	{ OP_ISNULL,    "IS NULL",     nullptr    },
	{ OP_ISNOTNULL, "IS NOT NULL", nullptr    },
};

const OpTok *opLookup(const std::string &tok)
{
	for (const auto &e : kOps)
		if (tok == e.tok)
			return &e;
	return nullptr;
}

const char *typeName(FType t)
{
	switch (t) {
	case FType::Text:     return "text";
	case FType::Int:      return "int";
	case FType::Bool:     return "bool";
	case FType::Datetime: return "datetime";
	case FType::Enum:     return "enum";
	}
	return "text";
}

/* --- value validation ----------------------------------------------------- */

bool isIntStr(const std::string &v)
{
	if (v.empty())
		return false;
	size_t i = (v[0] == '-') ? 1 : 0;
	if (i >= v.size())
		return false;
	for (; i < v.size(); i++)
		if (v[i] < '0' || v[i] > '9')
			return false;
	return true;
}

bool isDatetimeStr(const std::string &v)
{
	if (v.empty())
		return false;
	for (char c : v) {
		bool ok = (c >= '0' && c <= '9') || c == '-' || c == ':' ||
			  c == ' ' || c == '.' || c == 'T' || c == '/';
		if (!ok)
			return false;
	}
	return true;
}

bool csvHas(std::string_view csv, const std::string &v)
{
	size_t start = 0;
	while (start <= csv.size()) {
		size_t comma = csv.find(',', start);
		std::string_view tok = (comma == std::string_view::npos)
			? csv.substr(start)
			: csv.substr(start, comma - start);
		if (tok == v)
			return true;
		if (comma == std::string_view::npos)
			break;
		start = comma + 1;
	}
	return false;
}

bool validValue(const SearchField &f, const std::string &v, std::string &err)
{
	if (v.size() > (size_t)MAX_VLEN) {
		err = "value too long (max " + std::to_string(MAX_VLEN) + ")";
		return false;
	}
	switch (f.type) {
	case FType::Int:
	case FType::Bool:
		if (!isIntStr(v)) {
			err = "field '" + std::string(f.key) +
			      "' expects a numeric value";
			return false;
		}
		break;
	case FType::Datetime:
		if (!isDatetimeStr(v)) {
			err = "field '" + std::string(f.key) +
			      "' expects a date/time value";
			return false;
		}
		break;
	case FType::Enum:
		if (!csvHas(f.enumVals, v)) {
			err = "field '" + std::string(f.key) +
			      "' expects one of: " + std::string(f.enumVals);
			return false;
		}
		break;
	case FType::Text:
		break;
	}
	return true;
}

/* --- users registry ------------------------------------------------------- */

constexpr uint32_t INT_OPS  = OP_EQ | OP_NE | OP_LT | OP_GT | OP_LE | OP_GE;
constexpr uint32_t DT_OPS   = INT_OPS;
constexpr uint32_t TEXT_OPS = OP_EQ | OP_NE | OP_LIKE | OP_NLIKE;
constexpr uint32_t BOOL_OPS = OP_EQ | OP_NE;
constexpr uint32_t ENUM_OPS = OP_EQ | OP_NE;
constexpr uint32_t NULL_OPS = OP_ISNULL | OP_ISNOTNULL;
/* Exists: =/LIKE = "ever matched", !=/NOT LIKE = "never matched". */
constexpr uint32_t EXISTS_OPS = OP_EQ | OP_NE | OP_LIKE | OP_NLIKE;
constexpr uint32_t EXISTS_POS = OP_EQ | OP_LIKE;

const SearchField kUserFields[] = {
	/* key, label, type, kind, expr, exTable, exCol, exExtra, ops, sortable, display, enumVals */
	{ "id",         "User ID",   FType::Int,      FKind::Column, "u.id",        "", "", "", INT_OPS,  true,  true,  "" },
	{ "first_name", "First name", FType::Text,    FKind::Column, "u.first_name","", "", "", TEXT_OPS, true,  true,  "" },
	{ "last_name",  "Last name",  FType::Text,    FKind::Column, "u.last_name", "", "", "", TEXT_OPS, false, true,  "" },
	{ "username",   "Username (current)", FType::Text, FKind::Exists, "", "user_usernames x", "x.username", "AND x.kind='active'", EXISTS_OPS, false, true, "" },
	{ "type",       "Type",      FType::Enum,     FKind::Column, "u.type",      "", "", "", ENUM_OPS, true,  true,  "regular,deleted,bot,unknown" },
	{ "has_photo",  "Has photo", FType::Bool,     FKind::Column, "u.profile_photo_file_id", "", "", "", NULL_OPS, false, false, "" },
	{ "is_verified","Verified",  FType::Bool,     FKind::Column, "u.is_verified","", "", "", BOOL_OPS, false, true,  "" },
	{ "is_premium", "Premium",   FType::Bool,     FKind::Column, "u.is_premium", "", "", "", BOOL_OPS, false, true,  "" },
	{ "is_scam",    "Scam",      FType::Bool,     FKind::Column, "u.is_scam",   "", "", "", BOOL_OPS, false, true,  "" },
	{ "is_fake",    "Fake",      FType::Bool,     FKind::Column, "u.is_fake",   "", "", "", BOOL_OPS, false, true,  "" },
	{ "is_support", "Support",   FType::Bool,     FKind::Column, "u.is_support","", "", "", BOOL_OPS, false, false, "" },
	{ "created_at", "Created",   FType::Datetime, FKind::Column, "u.created_at","", "", "", DT_OPS,   true,  true,  "" },
	{ "updated_at", "Updated",   FType::Datetime, FKind::Column, "u.updated_at","", "", "", DT_OPS,   true,  true,  "" },
	{ "bio",        "Bio",       FType::Text,     FKind::Column, "COALESCE(e.bio,'')", "", "", "", TEXT_OPS, false, false, "" },
	{ "phone",      "Phone",     FType::Text,     FKind::Column, "COALESCE(e.phone_number,'')", "", "", "", TEXT_OPS, false, false, "" },
	{ "language",   "Language",  FType::Text,     FKind::Column, "COALESCE(e.language_code,'')", "", "", "", TEXT_OPS, false, false, "" },
	{ "restriction_reason", "Restriction", FType::Text, FKind::Column, "COALESCE(e.restriction_reason,'')", "", "", "", TEXT_OPS, false, false, "" },
	{ "has_sensitive", "Sensitive content", FType::Bool, FKind::Column, "COALESCE(e.has_sensitive_content,0)", "", "", "", BOOL_OPS, false, false, "" },
	{ "restricts_new_chats", "Restricts new chats", FType::Bool, FKind::Column, "COALESCE(e.restricts_new_chats,0)", "", "", "", BOOL_OPS, false, false, "" },
	{ "paid_star_count", "Paid message stars", FType::Int, FKind::Column, "COALESCE(e.paid_message_star_count,0)", "", "", "", INT_OPS, true, false, "" },
	{ "personal_chat_id", "Personal chat", FType::Int, FKind::Column, "e.personal_chat_id", "", "", "", OP_EQ | OP_NE | NULL_OPS, false, false, "" },
	{ "emoji_status", "Emoji status", FType::Int, FKind::Column, "e.emoji_status_custom_emoji_id", "", "", "", NULL_OPS, false, false, "" },
	{ "hist_username",   "Username (ever)",   FType::Text, FKind::Exists, "", "user_hist_usernames_events x", "x.username",     "", EXISTS_OPS, false, false, "" },
	{ "hist_first_name", "First name (ever)", FType::Text, FKind::Exists, "", "user_hist_name x",             "x.first_name",   "", EXISTS_POS, false, false, "" },
	{ "hist_last_name",  "Last name (ever)",  FType::Text, FKind::Exists, "", "user_hist_name x",             "x.last_name",    "", EXISTS_POS, false, false, "" },
	{ "hist_bio",        "Bio (ever)",        FType::Text, FKind::Exists, "", "user_hist_bio x",              "x.bio",          "", EXISTS_POS, false, false, "" },
	{ "hist_phone",      "Phone (ever)",      FType::Text, FKind::Exists, "", "user_hist_phone_num x",        "x.phone_number", "", EXISTS_POS, false, false, "" },
};

nlohmann::json mapRowUser(const drogon::orm::Row &r)
{
	nlohmann::json j;
	int64_t id = r["id"].as<int64_t>();
	j["id"]          = id;
	j["name"]        = displayName(r);
	j["first_name"]  = escCol(r, "first_name");
	j["last_name"]   = escCol(r, "last_name");
	j["type"]        = r["type"].as<std::string>();
	j["username"]    = escCol(r, "username");
	j["is_verified"] = rowBool(r, "is_verified");
	j["is_premium"]  = rowBool(r, "is_premium");
	j["is_scam"]     = rowBool(r, "is_scam");
	j["is_fake"]     = rowBool(r, "is_fake");
	j["is_support"]  = rowBool(r, "is_support");
	j["created_at"]  = r["created_at"].as<std::string>();
	j["updated_at"]  = r["updated_at"].as<std::string>();
	if (!r["profile_photo_file_id"].isNull())
		j["photo_file_id"] = r["profile_photo_file_id"].as<int64_t>();
	j["_href"] = "/users/" + std::to_string(id);
	return j;
}

const SearchSchema kUsersSchema = {
	/* fromJoin   */ "users u LEFT JOIN user_extra_info e ON e.user_id = u.id",
	/* selectCols */ "u.id, u.first_name, u.last_name, u.type, u.is_verified, "
			 "u.is_premium, u.is_scam, u.is_fake, u.is_support, "
			 "u.profile_photo_file_id, u.created_at, u.updated_at, "
			 "(SELECT un.username FROM user_usernames un "
			 "WHERE un.user_id = u.id AND un.kind='active' "
			 "ORDER BY un.position LIMIT 1) AS username",
	/* idCol      */ "u.id",
	/* exFk       */ "user_id",
	/* defaultSort*/ "u.id",
	/* defaultOrder*/ "DESC",
	/* fields     */ kUserFields,
	/* nFields    */ sizeof(kUserFields) / sizeof(kUserFields[0]),
	/* mapRow     */ &mapRowUser,
};

const SearchField *findField(const SearchSchema &s, const std::string &key)
{
	for (size_t i = 0; i < s.nFields; i++)
		if (key == s.fields[i].key)
			return &s.fields[i];
	return nullptr;
}

const char *canonConnector(const std::string &n)
{
	if (n == "AND")
		return "AND";
	if (n == "OR")
		return "OR";
	return nullptr;
}

/* --- registry JSON for the UI -------------------------------------------- */

nlohmann::json fieldsJson(const SearchSchema &s)
{
	nlohmann::json arr = nlohmann::json::array();
	for (size_t i = 0; i < s.nFields; i++) {
		const SearchField &f = s.fields[i];
		nlohmann::json o;
		o["key"]      = std::string(f.key);
		o["label"]    = std::string(f.label);
		o["type"]     = typeName(f.type);
		o["sortable"] = f.sortable;
		nlohmann::json ops = nlohmann::json::array();
		for (const auto &e : kOps)
			if (f.ops & e.op)
				ops.push_back(e.tok);
		o["operators"] = std::move(ops);
		if (f.type == FType::Enum)
			o["enum"] = std::string(f.enumVals);
		arr.push_back(std::move(o));
	}
	return arr;
}

nlohmann::json columnsJson(const SearchSchema &s)
{
	nlohmann::json arr = nlohmann::json::array();
	for (size_t i = 0; i < s.nFields; i++) {
		if (!s.fields[i].display)
			continue;
		nlohmann::json o;
		o["key"]   = std::string(s.fields[i].key);
		o["label"] = std::string(s.fields[i].label);
		arr.push_back(std::move(o));
	}
	return arr;
}

/*
 * Build the (injection-safe) count and page SQL plus the ordered bind values.
 * Returns false and sets err (400-worthy) on any invalid input. Only the bound
 * values come from the user; every emitted SQL token is a server constant.
 */
bool buildQuery(const SearchSchema &s, const Request &req,
		std::string &countSql, std::string &pageSql,
		std::vector<std::string> &binds, std::string &usedSort,
		std::string &usedOrder, int &limit, int &offset,
		std::string &err)
{
	if ((int)req.conds.size() > MAX_CONDS) {
		err = "too many conditions (max " + std::to_string(MAX_CONDS) + ")";
		return false;
	}

	std::string where;
	int existsCount = 0;

	for (size_t i = 0; i < req.conds.size(); i++) {
		const Condition &c = req.conds[i];
		const SearchField *f = findField(s, c.c);
		if (!f) {
			err = "unknown field: '" + c.c + "'";
			return false;
		}
		const OpTok *ot = opLookup(c.o);
		if (!ot || !(f->ops & ot->op)) {
			err = "operator '" + c.o + "' not allowed for field '" +
			      c.c + "'";
			return false;
		}

		if (i > 0) {
			const char *conn = canonConnector(req.conds[i - 1].n);
			if (!conn) {
				err = "invalid connector: '" +
				      req.conds[i - 1].n + "'";
				return false;
			}
			where += " ";
			where += conn;
			where += " ";
		}

		bool isNullOp = (ot->op == OP_ISNULL || ot->op == OP_ISNOTNULL);
		std::string frag = "(";

		if (isNullOp) {
			frag += f->expr;
			frag += (ot->op == OP_ISNULL) ? " IS NULL)" : " IS NOT NULL)";
		} else {
			if (!c.hasV) {
				err = "field '" + c.c + "' requires a value";
				return false;
			}
			if (!validValue(*f, c.v, err))
				return false;

			bool likeish = (ot->op == OP_LIKE || ot->op == OP_NLIKE);
			std::string val = likeish ? likePattern(c.v) : c.v;

			if (f->kind == FKind::Column) {
				frag += f->expr;
				frag += " ";
				frag += ot->sqlTok;
				frag += " ?)";
			} else { /* Exists */
				if (++existsCount > MAX_EXISTS) {
					err = "too many history conditions (max " +
					      std::to_string(MAX_EXISTS) + ")";
					return false;
				}
				const char *cmp = likeish ? "LIKE" : "=";
				const char *neg =
					(ot->op == OP_NE || ot->op == OP_NLIKE)
						? "NOT " : "";
				frag += neg;
				frag += "EXISTS (SELECT 1 FROM ";
				frag += f->exTable;
				frag += " WHERE x.";
				frag += s.exFk;
				frag += " = ";
				frag += s.idCol;
				frag += " ";
				frag += f->exExtra;
				frag += " AND ";
				frag += f->exCol;
				frag += " ";
				frag += cmp;
				frag += " ?))";
			}
			binds.push_back(std::move(val));
		}
		where += frag;
	}

	/* Sort column (registry-validated) + direction. */
	std::string sortExpr(s.defaultSort);
	usedSort.clear();
	if (!req.sort.empty()) {
		const SearchField *sf = findField(s, req.sort);
		if (sf && sf->sortable) {
			sortExpr = std::string(sf->expr);
			usedSort = std::string(sf->key);
		}
	}
	std::string ord;
	for (char ch : req.order)
		ord += (char)std::tolower((unsigned char)ch);
	if (ord == "asc")
		usedOrder = "ASC";
	else if (ord == "desc")
		usedOrder = "DESC";
	else
		usedOrder = std::string(s.defaultOrder);

	limit  = std::clamp(req.limit, 1, MAX_LIMIT);
	offset = std::clamp(req.offset, 0, MAX_OFFSET);

	/* Stable paging: break ties on the unique id, unless that IS the sort. */
	std::string tiebreak;
	if (sortExpr != s.idCol)
		tiebreak = ", " + std::string(s.idCol) + " DESC";

	std::string whereClause = where.empty() ? "" : (" WHERE " + where);
	countSql = "SELECT /*+ MAX_EXECUTION_TIME(3000) */ COUNT(*) AS n FROM " +
		   std::string(s.fromJoin) + whereClause;
	pageSql = "SELECT /*+ MAX_EXECUTION_TIME(3000) */ " +
		  std::string(s.selectCols) + " FROM " + std::string(s.fromJoin) +
		  whereClause + " ORDER BY " + sortExpr + " " + usedOrder +
		  tiebreak + " LIMIT " + std::to_string(limit) +
		  " OFFSET " + std::to_string(offset);

	/* Defensive: emitted placeholders must equal bound values. */
	size_t nph = 0;
	for (char ch : pageSql)
		if (ch == '?')
			nph++;
	if (nph != binds.size()) {
		err = "internal query build error";
		return false;
	}
	return true;
}

} /* namespace */

const SearchSchema &usersSchema(void)
{
	return kUsersSchema;
}

drogon::Task<nlohmann::json> run(drogon::orm::DbClientPtr db,
				 const SearchSchema &schema, Request req)
{
	std::string countSql, pageSql, usedSort, usedOrder, err;
	std::vector<std::string> binds;
	int limit = 0, offset = 0;

	if (!buildQuery(schema, req, countSql, pageSql, binds, usedSort,
			usedOrder, limit, offset, err))
		co_return nlohmann::json{ { "error", err } };

	nlohmann::json out;
	out["fields"]  = fieldsJson(schema);
	out["columns"] = columnsJson(schema);
	out["limit"]   = limit;
	out["offset"]  = offset;
	out["sort"]    = usedSort;
	out["order"]   = (usedOrder == "ASC") ? "asc" : "desc";

	/*
	 * std::as_const selects the execSqlCoro(sql, const std::vector<T>&)
	 * overload (dynamic bind list). A non-const vector would instead bind
	 * to the variadic overload as a single parameter.
	 */
	auto cres = co_await db->execSqlCoro(countSql, std::as_const(binds));
	out["total"] = cres.empty() ? 0 : cres[0]["n"].as<int64_t>();

	auto pres = co_await db->execSqlCoro(pageSql, std::as_const(binds));
	nlohmann::json rows = nlohmann::json::array();
	for (const auto &r : pres)
		rows.push_back(schema.mapRow(r));
	out["rows"] = std::move(rows);

	if (req.debug) {
		nlohmann::json dbg;
		dbg["sql"]       = pageSql;   /* no user data: values are ? */
		dbg["count_sql"] = countSql;
		nlohmann::json b = nlohmann::json::array();
		for (const auto &v : binds)
			b.push_back(Render::esc(v)); /* user data -> escape */
		dbg["bind"] = std::move(b);

		std::string explainSql = "EXPLAIN " + pageSql;
		auto eres = co_await db->execSqlCoro(explainSql,
						     std::as_const(binds));
		nlohmann::json ex = nlohmann::json::array();
		for (const auto &r : eres) {
			nlohmann::json row = nlohmann::json::object();
			for (drogon::orm::Result::SizeType ci = 0;
			     ci < eres.columns(); ci++) {
				const char *name = eres.columnName(ci);
				row[name] = r[name].isNull()
					? nlohmann::json(nullptr)
					: nlohmann::json(
						  Render::esc(r[name].as<std::string>()));
			}
			ex.push_back(std::move(row));
		}
		dbg["explain"] = std::move(ex);
		out["debug"] = std::move(dbg);
	}

	co_return out;
}

} /* namespace tgweb::dao::search */
