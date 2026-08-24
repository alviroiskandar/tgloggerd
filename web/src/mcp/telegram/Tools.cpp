// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "mcp/telegram/Tools.hpp"

#include "mcp/Filter.hpp"

#include <gwmcp/Errors.hpp>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>
#include <utility>

namespace tgweb::mcp::telegram {

namespace {

using gwmcp::Json;
using gwmcp::ToolError;
namespace flt = tgweb::mcp::filter;

constexpr int DEFAULT_LIMIT = 50;
constexpr int MAX_LIMIT = 200;

/*
 * THE EXPOSURE GATE.
 *
 * Every message query is ANDed with this. It is a single named constant used
 * everywhere rather than a phrase repeated per tool, so that "did we remember
 * the gate?" is answerable by grep rather than by reading each query.
 *
 * A semi-join against the allowlist rather than a JOIN: it cannot duplicate
 * rows, and it fails closed -- an empty allowlist matches nothing.
 */
constexpr const char *MSG_GATE =
	" AND m.chat_id IN (SELECT group_id FROM telegram_public_groups)";

/* The same gate expressed for a query whose alias for telegram_groups is g. */
constexpr const char *GROUP_GATE =
	" AND g.id IN (SELECT group_id FROM telegram_public_groups)";

/*
 * Blocking exec with a RUNTIME number of binds.
 *
 * DbClient::execSqlSync is variadic, so it cannot take a vector whose size is
 * only known at run time -- and unlike execSqlCoro, it has no vector overload.
 * This is that overload, built the way execSqlSync itself is: bind, switch the
 * binder to blocking mode, capture the result, exec (which throws on failure).
 */
drogon::orm::Result execSync(const drogon::orm::DbClientPtr &db,
			     const std::string &sql,
			     const std::vector<std::string> &binds)
{
	drogon::orm::Result r(nullptr);
	auto binder = *db << sql;
	for (const auto &b : binds)
		binder << b;
	binder << drogon::orm::Mode::Blocking;
	binder >> [&r](const drogon::orm::Result &res) { r = res; };
	binder.exec();
	return r;
}

std::string colStr(const drogon::orm::Row &r, const char *c)
{
	return r[c].isNull() ? std::string() : r[c].as<std::string>();
}

int64_t colI64(const drogon::orm::Row &r, const char *c)
{
	return r[c].isNull() ? 0 : r[c].as<int64_t>();
}

int clampLimit(const Json &args)
{
	int n = DEFAULT_LIMIT;
	if (args.contains("limit") && args["limit"].is_number_integer())
		n = args["limit"].get<int>();
	if (n < 1)
		n = 1;
	if (n > MAX_LIMIT)
		n = MAX_LIMIT;
	return n;
}

int clampOffset(const Json &args)
{
	int n = 0;
	if (args.contains("offset") && args["offset"].is_number_integer())
		n = args["offset"].get<int>();
	if (n < 0)
		n = 0;
	/* Deep offsets are O(offset) in MySQL; refuse rather than crawl. */
	if (n > 100000)
		throw ToolError("offset is too large (max 100000); narrow the "
				"filter instead of paging that deep");
	return n;
}

/* Message fields a caller may filter on. */
const flt::Field kMsgFields[] = {
	{ "text", "m.text", flt::FType::FullText, "",
	  "message body; word-based, so words shorter than 3 characters are "
	  "ignored by the index" },
	{ "group_id", "m.chat_id", flt::FType::Int, "",
	  "the group's id (negative)" },
	{ "sender_user_id", "m.sender_user_id", flt::FType::Int, "",
	  "author's Telegram user id" },
	{ "message_id", "m.message_id", flt::FType::Int, "",
	  "per-group message id" },
	{ "date", "m.date", flt::FType::DateTs, "",
	  "when it was sent; accepts YYYY-MM-DD or a unix timestamp" },
	{ "content_type", "m.content_type", flt::FType::Enum,
	  "text,photo,video,document,audio,voice,sticker,animation,service,unknown",
	  "kind of message" },
	{ "is_forwarded", "m.is_forwarded", flt::FType::Bool, "", "" },
	{ "is_channel_post", "m.is_channel_post", flt::FType::Bool, "", "" },
	{ "deleted", "m.deleted_at", flt::FType::Bool, "",
	  "use op is_null for live messages, is_not_null for deleted ones" },
};
const flt::Schema kMsgSchema{ kMsgFields,
			      sizeof(kMsgFields) / sizeof(kMsgFields[0]) };

/* User fields. */
const flt::Field kUserFields[] = {
	{ "user_id", "u.id", flt::FType::Int, "", "Telegram user id" },
	{ "username", "un.username", flt::FType::Text, "",
	  "current public username, without the @" },
	{ "first_name", "u.first_name", flt::FType::Text, "", "" },
	{ "last_name", "u.last_name", flt::FType::Text, "", "" },
	{ "phone_number", "COALESCE(e.phone_number,'')", flt::FType::Text, "",
	  "only set for contacts" },
	{ "bio", "COALESCE(e.bio,'')", flt::FType::Text, "", "" },
	{ "type", "u.type", flt::FType::Enum, "regular,deleted,bot,unknown", "" },
	{ "is_bot", "(u.type = 'bot')", flt::FType::Bool, "", "" },
	{ "msg_count", "u.msg_count", flt::FType::Int, "",
	  "messages seen from this user across the archive" },
};
const flt::Schema kUserSchema{ kUserFields,
			       sizeof(kUserFields) / sizeof(kUserFields[0]) };

Json messageRow(const drogon::orm::Row &r)
{
	Json j;
	j["message_id"] = colI64(r, "message_id");
	j["group_id"] = colI64(r, "chat_id");
	j["group_title"] = colStr(r, "group_title");
	j["sender_user_id"] = colI64(r, "sender_user_id");
	j["sender_name"] = colStr(r, "sender_name");
	j["sender_username"] = colStr(r, "sender_username");
	j["date"] = colI64(r, "date");
	j["sent_at"] = colStr(r, "sent_at");
	j["content_type"] = colStr(r, "content_type");
	/*
	 * RAW text, deliberately. dao::search HTML-escapes every cell because
	 * its output lands in a browser; this output lands in a model, where
	 * "it&#39;s &lt;b&gt;" is simply wrong and unescaping it later would be
	 * lossy.
	 */
	j["text"] = colStr(r, "text");
	j["is_forwarded"] = colI64(r, "is_forwarded") != 0;
	j["is_channel_post"] = colI64(r, "is_channel_post") != 0;
	if (!r["reply_to_msg_id"].isNull())
		j["reply_to_message_id"] = colI64(r, "reply_to_msg_id");
	if (!r["deleted_at"].isNull())
		j["deleted_at"] = colStr(r, "deleted_at");
	return j;
}

/*
 * Display columns, joined onto an ALREADY-CHOSEN set of message ids.
 *
 * The join is deliberately not part of choosing those ids -- see
 * runMessageQuery.
 */
constexpr const char *MSG_SELECT =
	"SELECT m.message_id, m.chat_id, m.sender_user_id, m.date, "
	"FROM_UNIXTIME(m.date) AS sent_at, m.content_type, m.text, "
	"m.is_forwarded, m.is_channel_post, m.reply_to_msg_id, m.deleted_at, "
	"g.title AS group_title, "
	"TRIM(CONCAT(COALESCE(su.first_name,''), ' ', "
	"            COALESCE(su.last_name,''))) AS sender_name, "
	"(SELECT x.username FROM telegram_user_usernames x "
	"  WHERE x.user_id = m.sender_user_id AND x.kind = 'active' "
	"  ORDER BY x.position LIMIT 1) AS sender_username ";

/*
 * Every message query goes through here, so the exposure gate cannot be
 * forgotten -- and so the plan below is used everywhere.
 *
 * TWO PHASES, and the reason matters. Choosing rows and decorating them for
 * display look like one query, but combining them is catastrophic for a
 * full-text search: with the joins present MySQL cannot use the fulltext index
 * order for ORDER BY, so it materialises every hit into a temporary table and
 * sorts that. Measured on this data, "kernel" walked 65,558 hits and took
 * 11.5 seconds -- past the statement timeout -- to return three rows.
 *
 * Selecting ids from the message table alone lets the fulltext index supply
 * rows in relevance order, so LIMIT stops after a handful: the same query
 * plans to 18 rows examined and 18 milliseconds. The joins then run against at
 * most `limit` ids, where they cost nothing.
 */
Json runMessageQuery(const drogon::orm::DbClientPtr &db,
		     const std::string &whereExtra,
		     const std::vector<std::string> &binds,
		     const std::string &orderBy, const std::string &orderBind,
		     int limit, int offset, bool includeTotal)
{
	/* Phase 1: single table, no joins, so the index can drive the order. */
	std::string inner =
		"SELECT /*+ MAX_EXECUTION_TIME(5000) */ m.id "
		"FROM telegram_group_messages m WHERE 1=1" +
		std::string(MSG_GATE);
	if (!whereExtra.empty())
		inner += " AND " + whereExtra;
	inner += " ORDER BY " + orderBy + " LIMIT " + std::to_string(limit) +
		 " OFFSET " + std::to_string(offset);

	/* Phase 2: decorate. Ordering again costs nothing on <= limit rows. */
	const std::string sql =
		std::string(MSG_SELECT) + "FROM (" + inner + ") sel " +
		"JOIN telegram_group_messages m ON m.id = sel.id " +
		"JOIN `telegram_groups` g ON g.id = m.chat_id " +
		"LEFT JOIN telegram_users su ON su.id = m.sender_user_id " +
		"ORDER BY " + orderBy;

	/*
	 * Placeholders are positional, and the inner query comes first in the
	 * SQL text: filter binds, then the inner ORDER BY's bind, then the
	 * outer one. A relevance ORDER BY restates MATCH(...) AGAINST(?), so it
	 * needs its own bind at each of the two places it appears.
	 */
	std::vector<std::string> pageBinds = binds;
	if (!orderBind.empty()) {
		pageBinds.push_back(orderBind);
		pageBinds.push_back(orderBind);
	}

	Json out;
	out["messages"] = Json::array();
	try {
		for (const auto &r : execSync(db, sql, pageBinds))
			out["messages"].push_back(messageRow(r));
	} catch (const std::exception &e) {
		throw ToolError(std::string("query failed: ") + e.what());
	}

	out["count"] = out["messages"].size();
	out["limit"] = limit;
	out["offset"] = offset;

	if (includeTotal) {
		/*
		 * Opt-in: counting a broad filter over 4.6M rows is the
		 * expensive half, and a caller reading the first page rarely
		 * needs it. No ORDER BY here, so no order bind.
		 */
		std::string csql =
			"SELECT /*+ MAX_EXECUTION_TIME(5000) */ COUNT(1) AS n "
			"FROM telegram_group_messages m WHERE 1=1" +
			std::string(MSG_GATE);
		if (!whereExtra.empty())
			csql += " AND " + whereExtra;
		try {
			auto cr = execSync(db, csql, binds);
			out["total"] = cr.empty() ? 0 : cr[0]["n"].as<int64_t>();
		} catch (const std::exception &e) {
			throw ToolError(std::string("count failed: ") + e.what());
		}
	}
	return out;
}

/* The exposed group ids. Cheap: a covering scan of a curated table. */
std::vector<int64_t> exposedGroups(const drogon::orm::DbClientPtr &db)
{
	std::vector<int64_t> out;
	for (const auto &r : execSync(
		     db, "SELECT group_id FROM telegram_public_groups", {}))
		out.push_back(r["group_id"].as<int64_t>());
	return out;
}

/* Join display columns onto an explicit, already-authorised set of ids. */
Json decorateIds(const drogon::orm::DbClientPtr &db,
		 const std::vector<int64_t> &ids)
{
	Json arr = Json::array();
	if (ids.empty())
		return arr;

	std::string in;
	std::vector<std::string> binds;
	for (size_t i = 0; i < ids.size(); i++) {
		in += i ? ",?" : "?";
		binds.push_back(std::to_string(ids[i]));
	}
	const std::string sql =
		std::string(MSG_SELECT) + "FROM telegram_group_messages m " +
		"JOIN `telegram_groups` g ON g.id = m.chat_id " +
		"LEFT JOIN telegram_users su ON su.id = m.sender_user_id " +
		"WHERE m.id IN (" + in + ") ORDER BY m.date DESC";
	for (const auto &r : execSync(db, sql, binds))
		arr.push_back(messageRow(r));
	return arr;
}

/*
 * "Recent messages", done per group and merged.
 *
 * Ordering by date across the whole gated set makes MySQL materialise every
 * message in every exposed group and sort it: measured at 2.6 seconds to return
 * two rows from one 236k-message group. Within a single group, message_id is
 * monotonic and covered by uq_group_messages_chat_msg, so a reverse index walk
 * stops after `limit` rows -- 2.6 MILLISECONDS for the same answer.
 *
 * There is no (chat_id, date) index that would let one query do this across
 * groups, so the fan-out is the query plan: one cheap indexed walk per exposed
 * group, merged here. The allowlist is curated and therefore small, which is
 * what makes that affordable.
 */
Json listRecent(const drogon::orm::DbClientPtr &db,
		const std::vector<int64_t> &groups, int limit, int offset)
{
	struct Hit {
		int64_t id;
		int64_t date;
	};
	std::vector<Hit> hits;
	const int perGroup = limit + offset;

	for (int64_t g : groups) {
		const std::string sql =
			"SELECT /*+ MAX_EXECUTION_TIME(5000) */ m.id, m.date "
			"FROM telegram_group_messages m WHERE m.chat_id = ? "
			"ORDER BY m.message_id DESC LIMIT " +
			std::to_string(perGroup);
		for (const auto &r :
		     execSync(db, sql, { std::to_string(g) }))
			hits.push_back({ r["id"].as<int64_t>(),
					 r["date"].as<int64_t>() });
	}

	std::sort(hits.begin(), hits.end(),
		  [](const Hit &a, const Hit &b) { return a.date > b.date; });

	std::vector<int64_t> ids;
	for (size_t i = (size_t)offset;
	     i < hits.size() && ids.size() < (size_t)limit; i++)
		ids.push_back(hits[i].id);

	Json out;
	out["messages"] = decorateIds(db, ids);
	out["count"] = out["messages"].size();
	out["limit"] = limit;
	out["offset"] = offset;
	return out;
}

Json inputSchemaForMessages(bool withFilter)
{
	Json props;
	if (withFilter) {
		props["filter"] = Json{
			{ "type", "object" },
			{ "description",
			  "A filter tree. A node is either a condition "
			  "{\"field\",\"op\",\"value\"} or a group "
			  "{\"and\":[...]}, {\"or\":[...]}, {\"not\":{...}}. "
			  "Groups nest, so (A OR B) AND NOT C is expressible." },
		};
	}
	props["group_id"] = Json{ { "type", "integer" },
				  { "description",
				    "Restrict to one group (negative id)." } };
	props["limit"] = Json{ { "type", "integer" },
			       { "description", "1-200, default 50." } };
	props["offset"] = Json{ { "type", "integer" },
				{ "description", "Rows to skip; default 0." } };
	props["include_total"] = Json{
		{ "type", "boolean" },
		{ "description",
		  "Also return the total match count. Off by default because "
		  "counting a broad filter is expensive." }
	};
	return Json{ { "type", "object" }, { "properties", props } };
}

} /* namespace */

void registerTools(gwmcp::ToolRegistry &registry, drogon::orm::DbClientPtr db)
{
	/* ---- telegram_list_groups ---- */
	{
		gwmcp::Tool t;
		t.name = "telegram_list_groups";
		t.title = "List available Telegram groups";
		t.description =
			"List the Telegram groups and channels this server may "
			"read. Start here: the other tools take a group_id, and "
			"only groups listed by this tool are queryable. Groups "
			"not listed -- including every private group and every "
			"direct message -- are invisible and cannot be reached "
			"by any filter.";
		t.inputSchema = Json{
			{ "type", "object" },
			{ "properties",
			  Json{ { "limit",
				  Json{ { "type", "integer" },
					{ "description", "1-200, default 50." } } } } }
		};
		t.handler = [db](const Json &args) {
			const int limit = clampLimit(args);
			std::string sql =
				"SELECT g.id, g.title, g.type, g.msg_count, "
				"(SELECT gu.username FROM telegram_group_usernames gu "
				"  WHERE gu.group_id = g.id AND gu.kind='active' "
				"  ORDER BY gu.position LIMIT 1) AS username "
				"FROM `telegram_groups` g WHERE 1=1" +
				std::string(GROUP_GATE) +
				" ORDER BY g.msg_count DESC LIMIT " +
				std::to_string(limit);
			Json out;
			out["groups"] = Json::array();
			try {
				for (const auto &r : execSync(db, sql, {})) {
					Json j;
					j["group_id"] = colI64(r, "id");
					j["title"] = colStr(r, "title");
					j["type"] = colStr(r, "type");
					j["username"] = colStr(r, "username");
					j["message_count"] =
						colI64(r, "msg_count");
					out["groups"].push_back(std::move(j));
				}
			} catch (const std::exception &e) {
				throw ToolError(std::string("query failed: ") +
						e.what());
			}
			out["count"] = out["groups"].size();
			return out;
		};
		registry.add(std::move(t));
	}

	/* ---- telegram_list_recent_messages ---- */
	{
		gwmcp::Tool t;
		t.name = "telegram_list_recent_messages";
		t.title = "Recent Telegram messages";
		t.description =
			"The most recent messages, newest first, across every "
			"readable group or within one group. Use this for "
			"\"what has been said lately\"; use "
			"telegram_search_messages when you need conditions.";
		t.inputSchema = inputSchemaForMessages(false);
		t.handler = [db](const Json &args) {
			std::vector<int64_t> groups;
			if (args.contains("group_id") &&
			    args["group_id"].is_number_integer()) {
				const int64_t g =
					args["group_id"].get<long long>();
				/* Honour the gate: an unexposed group simply
				 * has no rows to offer. */
				for (int64_t e : exposedGroups(db)) {
					if (e == g)
						groups.push_back(g);
				}
			} else {
				groups = exposedGroups(db);
			}
			try {
				return listRecent(db, groups, clampLimit(args),
						  clampOffset(args));
			} catch (const ToolError &) {
				throw;
			} catch (const std::exception &e) {
				throw ToolError(std::string("query failed: ") +
						e.what());
			}
		};
		registry.add(std::move(t));
	}

	/* ---- telegram_search_messages ---- */
	{
		gwmcp::Tool t;
		t.name = "telegram_search_messages";
		t.title = "Search Telegram messages";
		t.description =
			"Search messages with a filter tree supporting AND, OR "
			"and NOT.\n\nFields:\n" +
			flt::describeFields(kMsgSchema) +
			"\nOperators: =, !=, <, >, <=, >=, contains, "
			"not_contains, starts_with, in, between, is_null, "
			"is_not_null, match.\n\n"
			"Example -- messages mentioning \"kernel\" in either of "
			"two groups, excluding photos:\n"
			"{\"and\":[{\"field\":\"text\",\"op\":\"match\","
			"\"value\":\"kernel\"},"
			"{\"or\":[{\"field\":\"group_id\",\"op\":\"=\","
			"\"value\":-1001},{\"field\":\"group_id\",\"op\":\"=\","
			"\"value\":-1002}]},"
			"{\"not\":{\"field\":\"content_type\",\"op\":\"=\","
			"\"value\":\"photo\"}}]}\n\n"
			"Only readable groups are searched; see "
			"telegram_list_groups.";
		t.inputSchema = inputSchemaForMessages(true);
		t.handler = [db](const Json &args) {
			flt::Compiled c;
			if (args.contains("filter"))
				c = flt::compile(kMsgSchema, args["filter"]);

			std::string where = c.sql;
			std::vector<std::string> binds = c.binds;

			if (args.contains("group_id") &&
			    args["group_id"].is_number_integer()) {
				const std::string g = "m.chat_id = ?";
				where = where.empty() ? g : "(" + where +
								    " AND " + g +
								    ")";
				binds.push_back(std::to_string(
					args["group_id"].get<long long>()));
			}

			bool total = args.contains("include_total") &&
				     args["include_total"].is_boolean() &&
				     args["include_total"].get<bool>();

			/*
			 * Ordering a MATCH by date makes MySQL filesort every
			 * hit -- on 4.6M rows a broad term simply times out.
			 * When the filter has a full-text condition, order by
			 * relevance instead: that walks the fulltext index in
			 * order, so LIMIT stops early. Date order is still used
			 * when there is no MATCH to exploit.
			 */
			std::string order = "m.date DESC";
			std::string orderBind;
			if (c.hasFullText()) {
				order = "MATCH(" + c.ftExpr +
					") AGAINST(? IN BOOLEAN MODE) DESC";
				orderBind = c.ftValue;
			}

			return runMessageQuery(db, where, binds,
					       order, orderBind, clampLimit(args),
					       clampOffset(args), total);
		};
		registry.add(std::move(t));
	}

	/* ---- telegram_get_users ---- */
	{
		gwmcp::Tool t;
		t.name = "telegram_get_users";
		t.title = "Look up Telegram users";
		t.description =
			"Find users by id, username, phone number or name, "
			"using the same filter tree as message search.\n\n"
			"Fields:\n" +
			flt::describeFields(kUserSchema) +
			"\nNote names are not indexed, so a name search scans "
			"the user table -- prefer user_id or username when you "
			"have one.\n\n"
			"This tool is not restricted to readable groups: it "
			"describes accounts, not conversations.";
		t.inputSchema = Json{
			{ "type", "object" },
			{ "properties",
			  Json{ { "filter",
				  Json{ { "type", "object" },
					{ "description",
					  "Filter tree; same grammar as "
					  "telegram_search_messages." } } },
				{ "limit",
				  Json{ { "type", "integer" },
					{ "description", "1-200, default 50." } } },
				{ "offset", Json{ { "type", "integer" } } } } }
		};
		t.handler = [db](const Json &args) {
			flt::Compiled c;
			if (args.contains("filter"))
				c = flt::compile(kUserSchema, args["filter"]);
			if (c.sql.empty())
				throw ToolError(
					"a filter is required: give at least "
					"one of user_id, username, "
					"phone_number or a name");

			const std::string sql =
				"SELECT /*+ MAX_EXECUTION_TIME(5000) */ "
				"u.id, u.first_name, u.last_name, u.type, "
				"u.msg_count, u.is_verified, u.is_premium, "
				"u.is_scam, u.is_fake, u.created_at, "
				"COALESCE(e.bio,'') AS bio, "
				"COALESCE(e.phone_number,'') AS phone_number, "
				"COALESCE(e.language_code,'') AS language_code, "
				"un.username "
				"FROM telegram_users u "
				"LEFT JOIN telegram_user_extra_info e "
				"  ON e.user_id = u.id "
				"LEFT JOIN telegram_user_usernames un "
				"  ON un.user_id = u.id AND un.kind = 'active' "
				"  AND un.position = 0 "
				"WHERE " + c.sql +
				" ORDER BY u.msg_count DESC LIMIT " +
				std::to_string(clampLimit(args)) + " OFFSET " +
				std::to_string(clampOffset(args));

			Json out;
			out["users"] = Json::array();
			try {
				for (const auto &r : execSync(db, sql, c.binds)) {
					Json j;
					j["user_id"] = colI64(r, "id");
					j["first_name"] = colStr(r, "first_name");
					j["last_name"] = colStr(r, "last_name");
					j["username"] = colStr(r, "username");
					j["type"] = colStr(r, "type");
					j["bio"] = colStr(r, "bio");
					j["phone_number"] =
						colStr(r, "phone_number");
					j["language_code"] =
						colStr(r, "language_code");
					j["message_count"] = colI64(r, "msg_count");
					j["is_verified"] =
						colI64(r, "is_verified") != 0;
					j["is_premium"] =
						colI64(r, "is_premium") != 0;
					j["is_scam"] = colI64(r, "is_scam") != 0;
					j["is_fake"] = colI64(r, "is_fake") != 0;
					j["first_seen"] = colStr(r, "created_at");
					out["users"].push_back(std::move(j));
				}
			} catch (const std::exception &e) {
				throw ToolError(std::string("query failed: ") +
						e.what());
			}
			out["count"] = out["users"].size();
			return out;
		};
		registry.add(std::move(t));
	}
}

} /* namespace tgweb::mcp::telegram */
