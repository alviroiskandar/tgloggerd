// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "mcp/telegram/Tools.hpp"

#include "mcp/FileUrl.hpp"
#include "mcp/NoiseWords.hpp"
#include "mcp/Stopwords.hpp"
#include "mcp/Filter.hpp"

#include <gwmcp/Errors.hpp>

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <ctime>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

/* An optional integer argument clamped to [lo, hi]; `def` when absent. */
int clampIntArg(const Json &args, const char *name, int def, int lo, int hi)
{
	int n = def;
	if (args.contains(name) && args[name].is_number_integer())
		n = args[name].get<int>();
	if (n < lo)
		n = lo;
	if (n > hi)
		n = hi;
	return n;
}

int clampLimit(const Json &args)
{
	return clampIntArg(args, "limit", DEFAULT_LIMIT, 1, MAX_LIMIT);
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

/* args[name] as an integer, or the uniform "required" error. */
int64_t requireInt(const Json &args, const char *name)
{
	if (!args.contains(name) || !args[name].is_number_integer())
		throw ToolError(std::string(name) +
				" is required and must be an integer");
	return args[name].get<long long>();
}

/*
 * execSync whose failure surfaces as a ToolError -- the channel a caller can
 * act on -- rather than escaping the handler. Every tool query goes through
 * this; the bare execSync above stays for internal lookups whose failure
 * should not abort the tool.
 */
drogon::orm::Result execTool(const drogon::orm::DbClientPtr &db,
			     const std::string &sql,
			     const std::vector<std::string> &binds,
			     const char *what = "query failed")
{
	try {
		return execSync(db, sql, binds);
	} catch (const std::exception &e) {
		throw ToolError(std::string(what) + ": " + e.what());
	}
}

/*
 * Correlated subquery for a user's current public username, parameterised on
 * the column that names the user. One definition so every tool resolves "the
 * username" identically; MSG_SELECT below inlines the same shape as a
 * compile-time constant.
 */
std::string usernameSubq(const char *userCol)
{
	return std::string("(SELECT x.username FROM telegram_user_usernames x "
			   "  WHERE x.user_id = ") + userCol +
	       " AND x.kind = 'active' "
	       "  ORDER BY x.position LIMIT 1)";
}

/* The schema properties nearly every tool repeats. */
Json propGroupId(void)
{
	return Json{ { "type", "integer" },
		     { "description", "The group's id (negative)." } };
}

Json propLimit(const char *desc = "1-200, default 50.")
{
	return Json{ { "type", "integer" }, { "description", desc } };
}

/*
 * An optional start/end date, as a SQL fragment plus binds.
 *
 * Absent bounds mean "all time" rather than a default window, except where a
 * tool documents otherwise. Both bounds are inclusive, which is what a caller
 * writing "2026-01-01 to 2026-01-31" means.
 */
struct DateRange {
	std::string sql;   /* "" when unbounded */
	std::vector<std::string> binds;
	bool hasStart = false, hasEnd = false;
	long long start = 0, end = 0;

	/*
	 * Echo the effective range into a result. "all time" is stated
	 * explicitly rather than implied by two absent fields, so a model
	 * never has to guess what an unbounded answer covered.
	 */
	void stamp(Json &out) const
	{
		if (hasStart)
			out["start_date"] = start;
		if (hasEnd)
			out["end_date"] = end;
		if (!hasStart && !hasEnd)
			out["range"] = "all time";
	}
};

DateRange dateRange(const Json &args, long long defaultStart = 0)
{
	DateRange dr;
	if (args.contains("start_date") && !args["start_date"].is_null()) {
		dr.start = flt::parseDate(args["start_date"]);
		dr.hasStart = true;
	} else if (defaultStart) {
		dr.start = defaultStart;
		dr.hasStart = true;
	}
	if (args.contains("end_date") && !args["end_date"].is_null()) {
		dr.end = flt::parseDate(args["end_date"]);
		dr.hasEnd = true;
	}
	if (dr.hasStart && dr.hasEnd && dr.start > dr.end)
		throw ToolError("start_date is after end_date");

	if (dr.hasStart) {
		dr.sql += " AND m.date >= ?";
		dr.binds.push_back(std::to_string(dr.start));
	}
	if (dr.hasEnd) {
		dr.sql += " AND m.date <= ?";
		dr.binds.push_back(std::to_string(dr.end));
	}
	return dr;
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

/*
 * The media object for a message row whose file_id is non-NULL: metadata
 * always, a URL only when the bytes are actually stored -- files at or above
 * TG_MAX_STORE_FILE_SIZE are recorded but not kept, and /files/<token>
 * answers 404 for those, so a link would promise something the archive cannot
 * deliver. withNames adds the original filename/extension when the query
 * selected those columns.
 */
Json mediaJson(const drogon::orm::Row &r, bool withNames)
{
	Json md;
	const int64_t fid = colI64(r, "file_id");
	md["file_id"] = fid;
	md["type"] = colStr(r, "file_type");
	md["size"] = colI64(r, "file_size");
	if (withNames) {
		const std::string name = colStr(r, "orig_file_name");
		if (!name.empty())
			md["filename"] = name;
		const std::string ext = colStr(r, "file_ext");
		if (!ext.empty())
			md["extension"] = ext;
	}
	const bool stored = colI64(r, "on_disk") != 0;
	md["stored"] = stored;
	if (stored) {
		const std::string u = fileurl::forFile((uint64_t)fid);
		if (!u.empty())
			md["url"] = u;
	}
	return md;
}

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
	/*
	 * Always present, never inferred from absence. A reader must be able to
	 * tell "not edited" from "the field was omitted", and a message whose
	 * text has since changed or vanished is a different thing from one that
	 * has not -- which matters most when the content is being quoted.
	 */
	const bool edited = colI64(r, "edit_date") != 0;
	const bool deleted = !r["deleted_at"].isNull();
	j["is_edited"] = edited;
	j["is_deleted"] = deleted;
	if (edited)
		j["edit_date"] = colI64(r, "edit_date");
	if (deleted)
		j["deleted_at"] = colStr(r, "deleted_at");
	if (edited || deleted) {
		/* Point at the tool that can say what changed, since the text
		 * above is only the latest version. */
		j["history_available_via"] = "telegram_get_message_history";
	}

	if (!r["file_id"].isNull())
		j["media"] = mediaJson(r, true);
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
	"m.edit_date, "
	"g.title AS group_title, "
	"TRIM(CONCAT(COALESCE(su.first_name,''), ' ', "
	"            COALESCE(su.last_name,''))) AS sender_name, "
	"(SELECT x.username FROM telegram_user_usernames x "
	"  WHERE x.user_id = m.sender_user_id AND x.kind = 'active' "
	"  ORDER BY x.position LIMIT 1) AS sender_username, "
	"f.id AS file_id, f.file_type, f.file_size, f.file_ext, "
	"f.orig_file_name, f.on_disk ";

/*
 * The display joins, shared by both places that decorate a chosen set of
 * messages. telegram_files is LEFT-joined because most messages have no
 * attachment; it costs nothing here because these joins only ever run over the
 * rows already selected, never over the table being searched.
 */
constexpr const char *MSG_JOINS =
	"JOIN `telegram_groups` g ON g.id = m.chat_id "
	"LEFT JOIN telegram_users su ON su.id = m.sender_user_id "
	"LEFT JOIN telegram_files f ON f.id = m.file_id ";

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
		MSG_JOINS + "ORDER BY " + orderBy;

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
	for (const auto &r : execTool(db, sql, pageBinds))
		out["messages"].push_back(messageRow(r));

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
		auto cr = execTool(db, csql, binds,
				     "count failed");
		out["total"] = cr.empty() ? 0 : cr[0]["n"].as<int64_t>();
	}
	return out;
}

/* The exposed group ids. Cheap: a covering scan of a curated table. */
std::vector<int64_t> exposedGroups(const drogon::orm::DbClientPtr &db)
{
	std::vector<int64_t> out;
	for (const auto &r : execTool(
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
		MSG_JOINS + "WHERE m.id IN (" + in + ") ORDER BY m.date DESC";
	for (const auto &r : execTool(db, sql, binds))
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
		     execTool(db, sql, { std::to_string(g) }))
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

/*
 * Word-frequency tokenisation for telegram_popular_words.
 *
 * A word character is an ASCII letter or digit, or any byte >= 0x80. Folding
 * only ASCII case is a deliberate limit: keeping every continuation byte as a
 * word character holds a UTF-8 word (Cyrillic, Arabic, CJK, an emoji) together
 * as one token instead of shredding it into bytes, but case-folding it would
 * need a Unicode table we have no business carrying here. So non-Latin words
 * are counted, just not case-merged -- and CJK, which does not delimit words
 * with spaces, comes out as whole runs rather than words.
 *
 * URLs and e-mail addresses are dropped WHOLE rather than split. Measured on
 * this archive it is not a marginal cleanup: a group that quotes mailing-list
 * mail ranked "com", "org", "vger" and "gmail" in its top six, which describes
 * the From: headers people paste and nothing about the conversation. An
 * address is a machine identifier, and the pieces it shreds into are not words
 * anyone used.
 *
 * @mentions are NOT dropped, unlike addresses: "@someone" is how people refer
 * to each other in chat, it does not shred, and who gets talked about is a
 * real answer to "what is this group discussing".
 *
 * Then four filters, cheapest first: too short, all digits (ids, times and
 * prices), a stopword (Stopwords.hpp, vendored) or corpus noise
 * (NoiseWords.hpp, hand-maintained slang and scaffolding).
 */
bool isWordByte(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c >= 0x80;
}

/* Characters that may sit INSIDE an address without ending the run. */
bool isAddrByte(unsigned char c)
{
	return isWordByte(c) || c == '.' || c == '-' || c == '_' ||
	       c == '+' || c == '@';
}

/*
 * "wkwkwkwkwk", "hahahaha", "awokawokawok" -- laughter and filler are an
 * unbounded family, so no word list can hold them and every length is a
 * distinct token that splits the count of a word that means nothing anyway.
 * The shape is what identifies them: a unit of one or two characters repeated
 * to fill the whole token.
 *
 * Two characters, not three, and at least two full repeats: "bandung" would go
 * under a looser rule. It still catches a few real words built the same way
 * ("kakak"), which is a cost worth paying and is why `include_stopwords`
 * exists.
 */
bool isRepetitive(std::string_view w)
{
	if (w.size() < 4)
		return false;

	for (size_t u = 1; u <= 2; u++) {
		if (w.size() % u || w.size() / u < 2)
			continue;
		bool same = true;
		for (size_t i = u; i < w.size() && same; i++)
			same = w[i] == w[i - u];
		if (same)
			return true;
	}
	return false;
}

/*
 * Collapse a run of three or more identical bytes to one: "hmmmm" -> "hm",
 * "yesss" -> "yes", "sooooo" -> "so". Elongation is emphasis, not a different
 * word, and left alone it scatters one word across a dozen spellings.
 *
 * Three, not two, so that "coffee" and "committee" survive -- no English or
 * Indonesian word triples a letter. Byte-wise is safe for UTF-8: a repeated
 * multi-byte codepoint never produces three identical bytes in a row.
 */
void collapseElongation(std::string &w)
{
	size_t out = 0;
	for (size_t i = 0; i < w.size(); i++) {
		if (out >= 2 && w[i] == w[out - 1] && w[i] == w[out - 2])
			continue;
		w[out++] = w[i];
	}
	w.resize(out);
}

struct WordFilter {
	size_t minLen = 3;
	bool useStopwords = true;
	std::unordered_set<std::string> extra;

	bool drop(const std::string &w) const
	{
		if (w.size() < minLen)
			return true;
		if (w.find_first_not_of("0123456789") == std::string::npos)
			return true;
		if (extra.count(w))
			return true;
		if (!useStopwords)
			return false;
		return stopwords::isStopword(w) ||
		       noisewords::isNoiseWord(w) || isRepetitive(w);
	}
};

void countWords(const std::string &text, const WordFilter &filter,
		std::unordered_map<std::string, int64_t> &freq, int64_t &total)
{
	const size_t n = text.size();
	std::string w;

	for (size_t i = 0; i < n;) {
		const unsigned char c = (unsigned char)text[i];

		if (!isWordByte(c)) {
			i++;
			continue;
		}

		/* A scheme means a URL: skip to the next whitespace. */
		if ((c == 'h' || c == 'H') &&
		    (!text.compare(i, 7, "http://") ||
		     !text.compare(i, 8, "https://"))) {
			while (i < n && !isspace((unsigned char)text[i]))
				i++;
			continue;
		}

		/*
		 * Take the whole run first, punctuation included, so that an
		 * address is recognisable as one thing before it is split.
		 */
		const size_t begin = i;
		bool isAddr = false;
		while (i < n && isAddrByte((unsigned char)text[i])) {
			if (text[i] == '@')
				isAddr = true;
			i++;
		}
		if (isAddr)
			continue;

		/* Not an address: split the run and count the pieces. */
		for (size_t j = begin; j < i;) {
			if (!isWordByte((unsigned char)text[j])) {
				j++;
				continue;
			}
			w.clear();
			while (j < i && isWordByte((unsigned char)text[j])) {
				unsigned char b = (unsigned char)text[j++];
				if (b >= 'A' && b <= 'Z')
					b += 'a' - 'A';
				w.push_back((char)b);
			}

			collapseElongation(w);
			if (filter.drop(w))
				continue;

			freq[w]++;
			total++;
		}
	}
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
	props["limit"] = propLimit();
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

/*
 * "kinds": [...] -- which of `names` the caller wants. All of them when the
 * argument is absent; only the named ones otherwise. An unknown name is an
 * error listing what would have been accepted, because a silently ignored
 * kind would read as an empty history.
 */
void parseKinds(const Json &args, const char *const *names, size_t n,
		bool *want)
{
	for (size_t i = 0; i < n; i++)
		want[i] = true;
	if (!args.contains("kinds"))
		return;
	if (!args["kinds"].is_array())
		throw ToolError("\"kinds\" must be an array of strings");
	for (size_t i = 0; i < n; i++)
		want[i] = false;
	for (const auto &k : args["kinds"]) {
		if (!k.is_string())
			throw ToolError("\"kinds\" must contain strings");
		const std::string v = k.get<std::string>();
		bool found = false;
		for (size_t i = 0; i < n; i++) {
			if (v == names[i]) {
				want[i] = true;
				found = true;
			}
		}
		if (!found) {
			std::string all;
			for (size_t i = 0; i < n; i++)
				all += std::string(i ? ", " : "") + names[i];
			throw ToolError("unknown kind \"" + v +
					"\"; expected any of: " + all);
		}
	}
}

/*
 * One simple history section: run `sql` (one ? bind), emit each row as an
 * object mapping the given json keys to string columns, plus the observed_at
 * timestamp every history table carries. The sections with per-row logic
 * (photos, admin events) stay hand-written.
 */
void histList(const drogon::orm::DbClientPtr &db, Json &out, const char *key,
	      const std::string &sql, const std::string &bind,
	      std::initializer_list<std::pair<const char *, const char *>> fields)
{
	out[key] = Json::array();
	for (const auto &r : execTool(db, sql, { bind })) {
		Json j;
		for (const auto &f : fields)
			j[f.first] = colStr(r, f.second);
		j["observed_at"] = colStr(r, "created_at");
		out[key].push_back(std::move(j));
	}
}

/* ---- telegram_list_groups ---- */
Json run_telegram_list_groups(const drogon::orm::DbClientPtr &db, const Json &args)
{
	const int limit = clampLimit(args);
	std::string sql =
		"SELECT g.id, g.title, g.type, g.msg_count, "
		"g.description, "
		"gf.id AS photo_file_id, gf.on_disk AS photo_on_disk, "
		"(SELECT gu.username FROM telegram_group_usernames gu "
		"  WHERE gu.group_id = g.id AND gu.kind='active' "
		"  ORDER BY gu.position LIMIT 1) AS username "
		"FROM `telegram_groups` g "
		"LEFT JOIN telegram_files gf "
		"  ON gf.id = g.photo_file_id WHERE 1=1" +
		std::string(GROUP_GATE) +
		" ORDER BY g.msg_count DESC LIMIT " +
		std::to_string(limit);
	Json out;
	out["groups"] = Json::array();
	for (const auto &r : execTool(db, sql, {})) {
		Json j;
		j["group_id"] = colI64(r, "id");
		j["title"] = colStr(r, "title");
		j["type"] = colStr(r, "type");
		j["username"] = colStr(r, "username");
		j["message_count"] =
			colI64(r, "msg_count");
		const std::string desc =
			colStr(r, "description");
		if (!desc.empty())
			j["description"] = desc;

		if (!r["photo_file_id"].isNull()) {
			const int64_t pid =
				colI64(r, "photo_file_id");
			j["photo_file_id"] = pid;
			if (colI64(r, "photo_on_disk")) {
				const std::string u =
					fileurl::forFile(
						(uint64_t)pid);
				if (!u.empty())
					j["photo_url"] = u;
			}
		}
		out["groups"].push_back(std::move(j));
	}
	out["count"] = out["groups"].size();
	return out;
}

void add_telegram_list_groups(gwmcp::ToolRegistry &registry,
                              const drogon::orm::DbClientPtr &db)
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
			  propLimit() } } }
	};
	t.handler = [db](const Json &args) {
		return run_telegram_list_groups(db, args);
	};
	registry.add(std::move(t));
}

/* ---- telegram_get_group ---- */
Json run_telegram_get_group(const drogon::orm::DbClientPtr &db, const Json &args)
{
	const int64_t gid = requireInt(args, "group_id");
	const std::string bind = std::to_string(gid);

	Json out;
	auto rows = execTool(
		db,
		"SELECT /*+ MAX_EXECUTION_TIME(5000) */ "
		"g.id, g.title, g.type, g.description, "
		"g.msg_count, g.created_at, g.updated_at, "
		"gf.id AS photo_file_id, "
		"gf.on_disk AS photo_on_disk "
		"FROM `telegram_groups` g "
		"LEFT JOIN telegram_files gf "
		"  ON gf.id = g.photo_file_id "
		"WHERE g.id = ? "
		"  AND g.id IN (SELECT group_id "
		"               FROM telegram_public_groups)",
		{ bind });
	if (rows.empty())
		throw ToolError(
			"no readable group with id " +
			bind + "; see "
			"telegram_list_groups for what "
			"is available");

	const auto &r = rows[0];
	out["group_id"] = colI64(r, "id");
	out["title"] = colStr(r, "title");
	out["type"] = colStr(r, "type");
	const std::string desc = colStr(r, "description");
	if (!desc.empty())
		out["description"] = desc;
	out["message_count"] = colI64(r, "msg_count");
	out["first_seen"] = colStr(r, "created_at");
	out["last_updated"] = colStr(r, "updated_at");

	if (!r["photo_file_id"].isNull()) {
		const int64_t pid =
			colI64(r, "photo_file_id");
		out["photo_file_id"] = pid;
		if (colI64(r, "photo_on_disk")) {
			const std::string u =
				fileurl::forFile(
					(uint64_t)pid);
			if (!u.empty())
				out["photo_url"] = u;
		}
	}

	out["usernames"] = Json::array();
	for (const auto &r : execTool(
		     db,
		     "SELECT username, is_collectible "
		     "FROM telegram_group_usernames "
		     "WHERE group_id = ? AND kind='active' "
		     "ORDER BY position",
		     { bind })) {
		Json j;
		j["username"] = colStr(r, "username");
		if (colI64(r, "is_collectible"))
			j["is_collectible"] = true;
		out["usernames"].push_back(std::move(j));
	}

	/*
	 * Cheap because of migration 000023's
	 * (chat_id, sender_user_id) index; without it
	 * this would scan the group's whole message
	 * history just to size the participant list.
	 */
	auto sr = execTool(
		db,
		"SELECT /*+ MAX_EXECUTION_TIME(5000) */ "
		"COUNT(DISTINCT m.sender_user_id) AS n "
		"FROM telegram_group_messages m "
		"WHERE m.chat_id = ? "
		"  AND m.sender_user_id IS NOT NULL",
		{ bind });
	if (!sr.empty())
		out["distinct_senders"] =
			sr[0]["n"].as<int64_t>();

	/*
	 * The message span, from the ENDS of
	 * (chat_id, message_id) rather than MIN/MAX
	 * over date.
	 *
	 * Aggregating date cannot use an index -- date
	 * is in neither key -- so it reads every row in
	 * the group and turned this tool into a
	 * 3-second call. message_id is monotonic within
	 * a chat, so the first and last messages are
	 * the two endpoints of that index, and reading
	                                 * their dates is two single-row lookups.
	 */
	for (int end = 0; end < 2; end++) {
		const char *dir = end ? "DESC" : "ASC";
		const char *key = end
					  ? "last_message_date"
					  : "first_message_date";
		auto dr = execTool(
			db,
			std::string(
				"SELECT m.date FROM "
				"telegram_group_messages m "
				"WHERE m.chat_id = ? "
				"ORDER BY m.message_id ") +
				dir + " LIMIT 1",
			{ bind });
		/* date 0 is a real stored value for some
		 * rows, so it is not a usable bound. */
		if (!dr.empty() && colI64(dr[0], "date"))
			out[key] = colI64(dr[0], "date");
	}

	auto ar = execTool(
		db,
		"SELECT COUNT(1) AS n FROM "
		"telegram_group_admins WHERE group_id = ?",
		{ bind });
	if (!ar.empty())
		out["admin_count"] =
			ar[0]["n"].as<int64_t>();
	return out;
}

void add_telegram_get_group(gwmcp::ToolRegistry &registry,
                            const drogon::orm::DbClientPtr &db)
{
	gwmcp::Tool t;
	t.name = "telegram_get_group";
	t.title = "Get one group's full details";
	t.description =
		"Everything the archive holds about one readable group: "
		"title, description, type, every active username, photo, "
		"message count, how many distinct people have posted, "
		"how many admins it has, and the span of messages "
		"recorded.\n\n"
		"Only groups returned by telegram_list_groups can be "
		"queried; anything else is reported as not found.";
	t.inputSchema = Json{
		{ "type", "object" },
		{ "properties",
		  Json{ { "group_id",
			  propGroupId() } } },
		{ "required", Json::array({ "group_id" }) },
	};
	t.handler = [db](const Json &args) {
		return run_telegram_get_group(db, args);
	};
	registry.add(std::move(t));
}

/* ---- telegram_get_group_history ---- */
/*
 * The per-kind sections of a group's history. Each takes the gate as already
 * passed: run_telegram_get_group_history checks the allowlist once before any
 * of these read.
 */
void groupHistPhotos(const drogon::orm::DbClientPtr &db, Json &out,
		     const std::string &bind, const std::string &lim)
{
	out["photos"] = Json::array();
	for (const auto &r : execTool(
		     db,
		     "SELECT h.file_id, h.created_at, f.on_disk "
		     "FROM telegram_group_hist_photo h "
		     "LEFT JOIN telegram_files f ON f.id = h.file_id "
		     "WHERE h.group_id = ? "
		     "ORDER BY h.id DESC LIMIT " + lim,
		     { bind })) {
		const int64_t fid = colI64(r, "file_id");
		Json j;
		j["file_id"] = fid;
		if (colI64(r, "on_disk")) {
			const std::string u = fileurl::forFile((uint64_t)fid);
			if (!u.empty())
				j["url"] = u;
		}
		j["observed_at"] = colStr(r, "created_at");
		out["photos"].push_back(std::move(j));
	}
}

void groupHistAdmins(const drogon::orm::DbClientPtr &db, Json &out,
		     const std::string &bind, const std::string &lim)
{
	/* Who, not just what: an admin event is unreadable without the
	 * person's name. */
	out["admins"] = Json::array();
	for (const auto &r : execTool(
		     db,
		     "SELECT h.user_id, h.action, h.status, h.custom_title, "
		     "h.is_anonymous, h.created_at, "
		     "u.first_name, u.last_name, " +
		     usernameSubq("h.user_id") + " AS username "
		     "FROM telegram_group_admin_hist h "
		     "LEFT JOIN telegram_users u ON u.id = h.user_id "
		     "WHERE h.group_id = ? "
		     "ORDER BY h.id DESC LIMIT " + lim,
		     { bind })) {
		Json j;
		j["user_id"] = colI64(r, "user_id");
		j["username"] = colStr(r, "username");
		j["first_name"] = colStr(r, "first_name");
		j["last_name"] = colStr(r, "last_name");
		j["action"] = colStr(r, "action");
		j["status"] = colStr(r, "status");
		const std::string ct = colStr(r, "custom_title");
		if (!ct.empty())
			j["custom_title"] = ct;
		if (colI64(r, "is_anonymous"))
			j["is_anonymous"] = true;
		j["observed_at"] = colStr(r, "created_at");
		out["admins"].push_back(std::move(j));
	}
}

Json run_telegram_get_group_history(const drogon::orm::DbClientPtr &db, const Json &args)
{
	const int64_t gid = requireInt(args, "group_id");
	const std::string bind = std::to_string(gid);
	const std::string lim = std::to_string(clampLimit(args));

	static const char *kNames[5] = { "titles", "descriptions", "usernames",
					 "photos", "admins" };
	bool want[5];
	parseKinds(args, kNames, 5, want);

	Json out;
	out["group_id"] = gid;
	/* The gate, once, before any history is read: a group's past is as
	 * private as its present. */
	auto ok = execTool(db,
			   "SELECT 1 FROM telegram_public_groups "
			   "WHERE group_id = ? LIMIT 1",
			   { bind });
	if (ok.empty())
		throw ToolError("no readable group with id " + bind +
				"; see telegram_list_groups for what "
				"is available");

	if (want[0])
		histList(db, out, "titles",
			 "SELECT title, created_at FROM "
			 "telegram_group_hist_title WHERE group_id = ? "
			 "ORDER BY id DESC LIMIT " + lim,
			 bind, { { "title", "title" } });
	if (want[1])
		histList(db, out, "descriptions",
			 "SELECT description, created_at "
			 "FROM telegram_group_hist_description "
			 "WHERE group_id = ? ORDER BY id DESC LIMIT " + lim,
			 bind, { { "description", "description" } });
	if (want[2])
		histList(db, out, "usernames",
			 "SELECT username, action, kind, created_at FROM "
			 "telegram_group_hist_usernames_events "
			 "WHERE group_id = ? ORDER BY id DESC LIMIT " + lim,
			 bind,
			 { { "username", "username" },
			   { "action", "action" },
			   { "kind", "kind" } });
	if (want[3])
		groupHistPhotos(db, out, bind, lim);
	if (want[4])
		groupHistAdmins(db, out, bind, lim);
	return out;
}

void add_telegram_get_group_history(gwmcp::ToolRegistry &registry,
                                    const drogon::orm::DbClientPtr &db)
{
	gwmcp::Tool t;
	t.name = "telegram_get_group_history";
	t.title = "Get a group's change history";
	t.description =
		"How a readable group has changed over time: titles, "
		"descriptions, usernames, photos, and administrator "
		"promotions, demotions and privilege changes -- newest "
		"first.\n\n"
		"As with user history, timestamps are when a change was "
		"OBSERVED. Group metadata and the admin list are both "
		"polled rather than pushed, so a change made and "
		"reverted between polls leaves no trace, and the oldest "
		"entry of each kind is usually the value at first sight "
		"rather than a change.\n\n"
		"Pass `kinds` to fetch only some categories; the default "
		"is all of them.";
	t.inputSchema = Json{
		{ "type", "object" },
		{ "properties",
		  Json{ { "group_id",
			  propGroupId() },
			{ "kinds",
			  Json{ { "type", "array" },
				{ "items", Json{ { "type", "string" } } },
				{ "description",
				  "Any of: titles, descriptions, "
				  "usernames, photos, admins. "
				  "Default all." } } },
			{ "limit",
			  Json{ { "type", "integer" },
				{ "description",
				  "Entries per category, 1-200, "
				  "default 50." } } } } },
		{ "required", Json::array({ "group_id" }) },
	};
	t.handler = [db](const Json &args) {
		return run_telegram_get_group_history(db, args);
	};
	registry.add(std::move(t));
}

/* ---- telegram_list_group_admins ---- */
Json run_telegram_list_group_admins(const drogon::orm::DbClientPtr &db, const Json &args)
{

	const int64_t gid = requireInt(args, "group_id");

	/*
	 * The gate, as a WHERE clause rather than a check on
	 * the caller's argument: an unexposed group produces no
	 * rows, exactly as if it had no admins. Refusing it
	 * explicitly would confirm the group exists, which is
	 * itself something the allowlist is meant to withhold.
	 */
	const std::string sql =
		"SELECT /*+ MAX_EXECUTION_TIME(5000) */ "
		"a.user_id, a.status, a.custom_title, "
		"a.joined_date, a.is_anonymous, "
		"a.inviter_user_id, "
		"a.can_manage_chat, a.can_change_info, "
		"a.can_post_messages, a.can_edit_messages, "
		"a.can_delete_messages, a.can_invite_users, "
		"a.can_restrict_members, a.can_pin_messages, "
		"a.can_manage_topics, a.can_promote_members, "
		"a.can_manage_video_chats, a.can_post_stories, "
		"a.can_edit_stories, a.can_delete_stories, "
		"a.can_manage_direct_messages, a.can_manage_tags, "
		"u.first_name, u.last_name, u.type, "
		"u.is_verified, u.is_premium, " +
		usernameSubq("a.user_id") + " AS username "
		"FROM telegram_group_admins a "
		"LEFT JOIN telegram_users u ON u.id = a.user_id "
		"WHERE a.group_id = ? "
		"  AND a.group_id IN (SELECT group_id "
		"                     FROM telegram_public_groups) "
		/* Owner first, then longest-serving. */
		"ORDER BY (a.status = 'creator') DESC, "
		"         a.joined_date ASC "
		"LIMIT " + std::to_string(clampLimit(args));

	/* Only the rights actually held: 17 booleans, mostly
	 * false, are noise -- a list of what an admin CAN do
	 * reads better and is far smaller. */
	static const char *kRights[] = {
		"can_manage_chat", "can_change_info",
		"can_post_messages", "can_edit_messages",
		"can_delete_messages", "can_invite_users",
		"can_restrict_members", "can_pin_messages",
		"can_manage_topics", "can_promote_members",
		"can_manage_video_chats", "can_post_stories",
		"can_edit_stories", "can_delete_stories",
		"can_manage_direct_messages", "can_manage_tags",
	};

	Json out;
	out["group_id"] = gid;
	out["admins"] = Json::array();
	for (const auto &r : execTool(
		     db, sql, { std::to_string(gid) })) {
		Json j;
		j["user_id"] = colI64(r, "user_id");
		j["username"] = colStr(r, "username");
		j["first_name"] = colStr(r, "first_name");
		j["last_name"] = colStr(r, "last_name");
		j["status"] = colStr(r, "status");
		j["custom_title"] =
			colStr(r, "custom_title");
		j["is_anonymous"] =
			colI64(r, "is_anonymous") != 0;
		j["is_bot"] =
			colStr(r, "type") == "bot";
		j["is_verified"] =
			colI64(r, "is_verified") != 0;
		j["is_premium"] =
			colI64(r, "is_premium") != 0;
		if (colI64(r, "joined_date"))
			j["joined_date"] =
				colI64(r, "joined_date");
		if (colI64(r, "inviter_user_id"))
			j["promoted_by_user_id"] =
				colI64(r, "inviter_user_id");

		Json perms = Json::array();
		for (const char *k : kRights) {
			if (colI64(r, k))
				perms.push_back(k);
		}
		j["permissions"] = std::move(perms);
		out["admins"].push_back(std::move(j));
	}
	out["count"] = out["admins"].size();
	return out;
}

void add_telegram_list_group_admins(gwmcp::ToolRegistry &registry,
                                    const drogon::orm::DbClientPtr &db)
{
	gwmcp::Tool t;
	t.name = "telegram_list_group_admins";
	t.title = "List a group's administrators";
	t.description =
		"List the administrators of one readable group, with "
		"their privileges. The owner is listed first, then "
		"other admins oldest-promotion first.\n\n"
		"Only groups returned by telegram_list_groups can be "
		"queried; anything else yields an empty list rather "
		"than an error.\n\n"
		"Note this is a snapshot, not a live read. Telegram "
		"does not push admin changes to a regular account, so "
		"the list is refreshed by periodic polling and may lag "
		"a very recent promotion or demotion.";
	t.inputSchema = Json{
		{ "type", "object" },
		{ "properties",
		  Json{ { "group_id",
			  propGroupId() },
			{ "limit",
			  propLimit() } } },
		{ "required", Json::array({ "group_id" }) },
	};
	t.handler = [db](const Json &args) {
		return run_telegram_list_group_admins(db, args);
	};
	registry.add(std::move(t));
}

/* ---- telegram_list_recent_messages ---- */
Json run_telegram_list_recent_messages(const drogon::orm::DbClientPtr &db, const Json &args)
{
	std::vector<int64_t> groups;
	if (args.contains("group_id") &&
	    args["group_id"].is_number_integer()) {
		const int64_t g =
			requireInt(args, "group_id");
		/* Honour the gate: an unexposed group simply
		 * has no rows to offer. */
		for (int64_t e : exposedGroups(db)) {
			if (e == g)
				groups.push_back(g);
		}
	} else {
		groups = exposedGroups(db);
	}
	return listRecent(db, groups, clampLimit(args),
			  clampOffset(args));
}

void add_telegram_list_recent_messages(gwmcp::ToolRegistry &registry,
                                       const drogon::orm::DbClientPtr &db)
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
		return run_telegram_list_recent_messages(db, args);
	};
	registry.add(std::move(t));
}

/* ---- telegram_list_group_senders ---- */
Json run_telegram_list_group_senders(const drogon::orm::DbClientPtr &db, const Json &args)
{

	const int64_t gid = requireInt(args, "group_id");
	const int limit = clampLimit(args);
	const int offset = clampOffset(args);
	const DateRange dr = dateRange(args);

	/*
	 * Aggregate first, decorate second -- the same split
	 * the message queries use, and for the same reason: the
	 * inner query is covered by
	 * idx_group_messages_chat_sender (added in migration
	 * 000023), and adding the user joins to it would break
	 * that. The joins then run over at most `limit` rows.
	 *
	 * sender_user_id IS NULL means a channel post or an
	 * anonymous admin -- sent by the chat, not a user -- so
	 * those are excluded rather than collapsed into a
	 * phantom participant.
	 */
	const std::string inner =
		"SELECT /*+ MAX_EXECUTION_TIME(5000) */ "
		"m.sender_user_id AS uid, COUNT(1) AS n "
		"FROM telegram_group_messages m "
		"WHERE m.chat_id = ? "
		"  AND m.sender_user_id IS NOT NULL "
		"  AND m.chat_id IN (SELECT group_id "
		"                    FROM telegram_public_groups)" +
		dr.sql +
		" GROUP BY m.sender_user_id "
		"ORDER BY n DESC LIMIT " + std::to_string(limit) +
		" OFFSET " + std::to_string(offset);

	const std::string sql =
		"SELECT s.uid, s.n, u.first_name, u.last_name, "
		"u.type, u.is_verified, u.is_premium, "
		"u.msg_count AS total_msgs, " +
		usernameSubq("s.uid") + " AS username "
		"FROM (" + inner + ") s "
		"LEFT JOIN telegram_users u ON u.id = s.uid "
		"ORDER BY s.n DESC";

	Json out;
	out["group_id"] = gid;
	out["senders"] = Json::array();
	std::vector<std::string> binds{
		std::to_string(gid)
	};
	binds.insert(binds.end(), dr.binds.begin(),
		     dr.binds.end());
	for (const auto &r : execTool(db, sql, binds)) {
		Json j;
		j["user_id"] = colI64(r, "uid");
		j["username"] = colStr(r, "username");
		j["first_name"] = colStr(r, "first_name");
		j["last_name"] = colStr(r, "last_name");
		j["is_bot"] = colStr(r, "type") == "bot";
		j["is_verified"] =
			colI64(r, "is_verified") != 0;
		j["is_premium"] =
			colI64(r, "is_premium") != 0;
		j["messages_in_group"] = colI64(r, "n");
		/* Across the whole archive, for
		 * contrast with the per-group count. */
		j["messages_total"] =
			colI64(r, "total_msgs");
		out["senders"].push_back(std::move(j));
	}

	out["count"] = out["senders"].size();
	out["limit"] = limit;
	out["offset"] = offset;
	dr.stamp(out);

	if (args.contains("include_total") &&
	    args["include_total"].is_boolean() &&
	    args["include_total"].get<bool>()) {
		const std::string csql =
			"SELECT /*+ MAX_EXECUTION_TIME(5000) */ "
			"COUNT(DISTINCT m.sender_user_id) AS n "
			"FROM telegram_group_messages m "
			"WHERE m.chat_id = ? "
			"  AND m.sender_user_id IS NOT NULL "
			"  AND m.chat_id IN (SELECT group_id "
			"                    FROM telegram_public_groups)" +
			dr.sql;
		std::vector<std::string> cb{
			std::to_string(gid)
		};
		cb.insert(cb.end(), dr.binds.begin(),
			  dr.binds.end());
		auto cr = execTool(db, csql, cb, "count failed");
		out["total_senders"] =
			cr.empty() ? 0
				   : cr[0]["n"].as<int64_t>();
	}
	return out;
}

void add_telegram_list_group_senders(gwmcp::ToolRegistry &registry,
                                     const drogon::orm::DbClientPtr &db)
{
	gwmcp::Tool t;
	t.name = "telegram_list_group_senders";
	t.title = "Message-count leaderboard for a group";
	t.description =
		"A leaderboard of who posts most in one readable group: "
		"every user who has sent a message, ordered by message "
		"count descending, with the count for each.\n\n"
		"Pass start_date and/or end_date to count only part of "
		"the history; with neither, it counts all time.\n\n"
		"This is participation, not membership: it can only "
		"see people who have posted, so lurkers and members who "
		"joined without speaking do not appear. Counts include "
		"messages later deleted, since the archive keeps them.\n\n"
		"Channel posts and messages from anonymous admins are "
		"excluded, as those are sent by the channel rather than "
		"by a user.\n\n"
		"Only groups returned by telegram_list_groups can be "
		"queried.";
	t.inputSchema = Json{
		{ "type", "object" },
		{ "properties",
		  Json{ { "group_id",
			  propGroupId() },
			{ "limit",
			  propLimit() },
			{ "offset", Json{ { "type", "integer" } } },
			{ "start_date",
			  Json{ { "description",
				  "Only count messages on or after this "
				  "date. YYYY-MM-DD or a unix "
				  "timestamp. Omit for all time." } } },
			{ "end_date",
			  Json{ { "description",
				  "Only count messages on or before "
				  "this date. Omit for all time." } } },
			{ "include_total",
			  Json{ { "type", "boolean" },
				{ "description",
				  "Also return how many distinct users "
				  "have posted. Costs a second pass." } } } } },
		{ "required", Json::array({ "group_id" }) },
	};
	t.handler = [db](const Json &args) {
		return run_telegram_list_group_senders(db, args);
	};
	registry.add(std::move(t));
}

/* ---- telegram_count_user_messages ---- */
Json run_telegram_count_user_messages(const drogon::orm::DbClientPtr &db, const Json &args)
{

	const int64_t gid = requireInt(args, "group_id");
	const int64_t uid = requireInt(args, "user_id");
	const DateRange dr = dateRange(args);

	/*
	 * (chat_id, sender_user_id) from migration 000023 makes
	 * the unbounded form a pure index range count, no rows
	 * touched. With a date bound MySQL still walks that
	 * range but has to visit each row for m.date, which is
	 * the price of the bound; it stays proportional to this
	 * user's messages in this group, not to the group.
	 */
	const std::string sql =
		"SELECT /*+ MAX_EXECUTION_TIME(5000) */ "
		"COUNT(1) AS n, MIN(m.date) AS first_date, "
		"MAX(m.date) AS last_date "
		"FROM telegram_group_messages m "
		"WHERE m.chat_id = ? AND m.sender_user_id = ? "
		"  AND m.chat_id IN (SELECT group_id "
		"                    FROM telegram_public_groups)" +
		dr.sql;

	std::vector<std::string> binds{ std::to_string(gid),
					std::to_string(uid) };
	binds.insert(binds.end(), dr.binds.begin(),
		     dr.binds.end());

	Json out;
	out["group_id"] = gid;
	out["user_id"] = uid;
	const auto rows = execTool(db, sql, binds);
	const int64_t n =
		rows.empty() ? 0 : colI64(rows[0], "n");
	out["message_count"] = n;
	if (n) {
		out["first_message_date"] =
			colI64(rows[0], "first_date");
		out["last_message_date"] =
			colI64(rows[0], "last_date");
	}

	dr.stamp(out);

	/*
	 * A zero is ambiguous on its own -- unknown user, wrong
	 * group, or a group nobody exposed -- so say which.
	 */
	if (out["message_count"].get<int64_t>() == 0) {
		const auto g = execTool(
			db,
			"SELECT 1 AS x FROM "
			"telegram_public_groups WHERE "
			"group_id = ?",
			{ std::to_string(gid) });
		if (g.empty())
			out["note"] =
				"That group is not exposed to "
				"MCP, so the count is zero "
				"regardless of what the user "
				"posted. See "
				"telegram_list_groups.";
	}

	/* Cheap PK lookup, so the answer names the person. */
	try {
		const auto u = execSync(
			db,
			"SELECT u.first_name, u.last_name, "
			"(SELECT x.username FROM "
			"  telegram_user_usernames x "
			"  WHERE x.user_id = u.id AND "
			"        x.kind = 'active' "
			"  ORDER BY x.position LIMIT 1) AS username "
			"FROM telegram_users u WHERE u.id = ?",
			{ std::to_string(uid) });
		if (!u.empty()) {
			out["username"] = colStr(u[0], "username");
			out["first_name"] =
				colStr(u[0], "first_name");
			out["last_name"] =
				colStr(u[0], "last_name");
		}
	} catch (const std::exception &) {
		/* Decoration only; never fail the count. */
	}
	return out;
}

void add_telegram_count_user_messages(gwmcp::ToolRegistry &registry,
                                      const drogon::orm::DbClientPtr &db)
{
	gwmcp::Tool t;
	t.name = "telegram_count_user_messages";
	t.title = "Count one user's messages in one group";
	t.description =
		"How many messages a single user has sent to a single "
		"readable group.\n\n"
		"Use this when the question is about one person -- "
		"\"how much has @alice posted in GNU/Weeb?\" -- rather "
		"than about the group as a whole; for a ranking of "
		"everyone use telegram_list_group_senders.\n\n"
		"Pass start_date and/or end_date to count only part of "
		"the history; with neither, it counts all time.\n\n"
		"Counts include messages later deleted, since the "
		"archive keeps them. Messages the user sent "
		"anonymously (as the group) are not attributed to "
		"them and so are not counted.\n\n"
		"Only groups returned by telegram_list_groups can be "
		"queried.";
	t.inputSchema = Json{
		{ "type", "object" },
		{ "properties",
		  Json{ { "group_id",
			  propGroupId() },
			{ "user_id",
			  Json{ { "type", "integer" },
				{ "description",
				  "The user's id. Resolve a username "
				  "with telegram_get_users first." } } },
			{ "start_date",
			  Json{ { "description",
				  "Only count messages on or after this "
				  "date. YYYY-MM-DD or a unix "
				  "timestamp. Omit for all time." } } },
			{ "end_date",
			  Json{ { "description",
				  "Only count messages on or before "
				  "this date. Omit for all time." } } } } },
		{ "required", Json::array({ "group_id", "user_id" }) },
	};
	t.handler = [db](const Json &args) {
		return run_telegram_count_user_messages(db, args);
	};
	registry.add(std::move(t));
}

/* ---- telegram_popular_words ---- */
/* The word-filtering arguments of telegram_popular_words. */
WordFilter wordFilterFromArgs(const Json &args)
{
	WordFilter wf;
	wf.minLen = (size_t)clampIntArg(args, "min_length", 3, 1, 32);
	if (args.contains("include_stopwords") &&
	    args["include_stopwords"].is_boolean())
		wf.useStopwords = !args["include_stopwords"].get<bool>();
	if (args.contains("exclude")) {
		if (!args["exclude"].is_array())
			throw ToolError("exclude must be an array of strings");
		if (args["exclude"].size() > 200)
			throw ToolError("exclude holds at most 200 words");
		for (const auto &e : args["exclude"]) {
			if (!e.is_string())
				throw ToolError("exclude must be an array of "
						"strings");
			std::string v = e.get<std::string>();
			for (char &ch : v)
				if (ch >= 'A' && ch <= 'Z')
					ch += 'a' - 'A';
			wf.extra.insert(std::move(v));
		}
	}
	return wf;
}

Json run_telegram_popular_words(const drogon::orm::DbClientPtr &db, const Json &args)
{
	const int64_t gid = requireInt(args, "group_id");
	const int limit = clampLimit(args);

	/*
	 * The one tool with a default window rather than all time: "popular
	 * words, ever" over a decade-old group is both far more expensive and
	 * much less useful than "popular words lately".
	 */
	const long long defStart = (long long)time(nullptr) - 30LL * 86400LL;
	const DateRange dr = dateRange(args, defStart);
	const WordFilter wf = wordFilterFromArgs(args);
	const int scanCap = clampIntArg(args, "max_messages", 50000,
					1, 200000);

	/*
	 * Service messages ("X joined the group") are excluded: their text is
	 * generated by Telegram, so counting it measures membership churn,
	 * not conversation.
	 */
	const std::string sql =
		"SELECT /*+ MAX_EXECUTION_TIME(5000) */ m.text "
		"FROM telegram_group_messages m "
		"WHERE m.chat_id = ? AND m.text IS NOT NULL "
		"  AND m.content_type <> 'service'" +
		std::string(MSG_GATE) + dr.sql +
		/*
		 * By date, not message_id: the range is on date, so
		 * idx_group_messages_chat_date (migration 000024) supplies
		 * this ordering for free. Ordering by message_id instead
		 * ranges on one index and sorts by another, which is a
		 * filesort over the whole window.
		 */
		" ORDER BY m.date DESC LIMIT " + std::to_string(scanCap);

	std::vector<std::string> binds{ std::to_string(gid) };
	binds.insert(binds.end(), dr.binds.begin(), dr.binds.end());

	std::unordered_map<std::string, int64_t> freq;
	int64_t scanned = 0, words = 0;
	for (const auto &r : execTool(db, sql, binds)) {
		scanned++;
		countWords(colStr(r, "text"), wf, freq, words);
	}

	/*
	 * Partial sort: the tail is never looked at, and the map can hold a
	 * hundred thousand distinct words.
	 */
	std::vector<std::pair<std::string, int64_t>> top(freq.begin(),
							 freq.end());
	const size_t keep = std::min((size_t)limit, top.size());
	std::partial_sort(top.begin(), top.begin() + keep, top.end(),
			  [](const auto &a, const auto &b) {
				  if (a.second != b.second)
					  return a.second > b.second;
				  return a.first < b.first;
			  });

	Json out;
	out["group_id"] = gid;
	out["start_date"] = dr.start;
	out["end_date"] = dr.hasEnd ? Json(dr.end) : Json(nullptr);
	out["words"] = Json::array();
	for (size_t i = 0; i < keep; i++) {
		Json j;
		j["word"] = top[i].first;
		j["count"] = top[i].second;
		/* Share of all counted words, not of messages. */
		j["percent"] = words ? (double)top[i].second * 100.0 /
					       (double)words
				     : 0.0;
		out["words"].push_back(std::move(j));
	}
	out["count"] = out["words"].size();
	out["messages_scanned"] = scanned;
	out["words_counted"] = words;
	out["distinct_words"] = (int64_t)freq.size();
	out["truncated"] = scanned >= scanCap;
	out["stopwords_filtered"] = wf.useStopwords;

	if (!scanned)
		out["note"] =
			"No messages with text in that window. "
			"Check the group is exposed (see "
			"telegram_list_groups) and widen the "
			"date range.";
	return out;
}

void add_telegram_popular_words(gwmcp::ToolRegistry &registry,
                                const drogon::orm::DbClientPtr &db)
{
	gwmcp::Tool t;
	t.name = "telegram_popular_words";
	t.title = "Most-used words in a group";
	t.description =
		"What a readable group actually talks about: the words "
		"used most often in its messages, commonest first.\n\n"
		"Noise is filtered so the result is topical rather than "
		"grammatical. Excluded: Indonesian and English "
		"stopwords (\"yang\", \"the\", \"untuk\", \"and\"); chat "
		"slang and shorthand in both languages (\"gak\", "
		"\"aja\", \"wkwk\", \"lol\", \"btw\"); laughter and filler "
		"of any length (\"wkwkwkwk\", \"hahahaha\"); quoted-mail "
		"and calendar scaffolding (\"subject\", \"wrote\", "
		"\"jul\"); URLs, e-mail addresses and pure numbers. "
		"Elongation is collapsed, so \"yesss\" counts as "
		"\"yes\".\n\n"
		"Pass include_stopwords: true to switch all of that off "
		"and get raw frequencies, or exclude: [...] to drop "
		"further words specific to your question.\n\n"
		"With no dates this covers the last 30 days, not all "
		"time -- word frequency is a snapshot of what is being "
		"discussed now. Pass start_date and/or end_date for any "
		"other window.\n\n"
		"Counting reads message text, so it scans at most "
		"max_messages (newest first) and reports "
		"messages_scanned and truncated; when truncated is "
		"true the counts describe that newest slice, not the "
		"whole window.\n\n"
		"Only groups returned by telegram_list_groups can be "
		"queried.";
	t.inputSchema = Json{
		{ "type", "object" },
		{ "properties",
		  Json{ { "group_id",
			  propGroupId() },
			{ "limit",
			  propLimit("How many words to return. 1-200, default 50.") },
			{ "start_date",
			  Json{ { "description",
				  "Window start. YYYY-MM-DD or a unix "
				  "timestamp. Default: 30 days ago." } } },
			{ "end_date",
			  Json{ { "description",
				  "Window end. Default: now." } } },
			{ "min_length",
			  Json{ { "type", "integer" },
				{ "description",
				  "Ignore words shorter than this. "
				  "1-32, default 3." } } },
			{ "max_messages",
			  Json{ { "type", "integer" },
				{ "description",
				  "Cap on messages scanned, newest "
				  "first. 1-200000, default 50000." } } },
			{ "exclude",
			  Json{ { "type", "array" },
				{ "items", Json{ { "type", "string" } } },
				{ "description",
				  "Further words to drop, on top of "
				  "the built-in lists. Up to 200." } } },
			{ "include_stopwords",
			  Json{ { "type", "boolean" },
				{ "description",
				  "Count every word, filtering nothing "
				  "but `exclude`, min_length and pure "
				  "numbers. Default false." } } } } },
		{ "required", Json::array({ "group_id" }) },
	};
	t.handler = [db](const Json &args) {
		return run_telegram_popular_words(db, args);
	};
	registry.add(std::move(t));
}

/* ---- telegram_search_messages ---- */
Json run_telegram_search_messages(const drogon::orm::DbClientPtr &db, const Json &args)
{
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
			requireInt(args, "group_id")));
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
}

void add_telegram_search_messages(gwmcp::ToolRegistry &registry,
                                  const drogon::orm::DbClientPtr &db)
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
		return run_telegram_search_messages(db, args);
	};
	registry.add(std::move(t));
}

/* ---- telegram_get_message_history ---- */
/*
 * The pre-edit snapshots of one message, oldest first, so the list reads as
 * the progression and ends at `current`.
 */
void msgHistVersions(const drogon::orm::DbClientPtr &db, Json &out,
		     int64_t rowId)
{
	out["previous_versions"] = Json::array();
	for (const auto &e : execTool(
		     db,
		     "SELECT e.content_type, e.text, e.edit_date, "
		     "e.created_at, "
		     "f.id AS file_id, f.file_type, f.file_size, f.on_disk "
		     "FROM telegram_group_message_edits e "
		     "LEFT JOIN telegram_files f ON f.id = e.file_id "
		     "WHERE e.group_message_id = ? ORDER BY e.id ASC",
		     { std::to_string(rowId) })) {
		Json v;
		v["content_type"] = colStr(e, "content_type");
		v["text"] = colStr(e, "text");
		if (colI64(e, "edit_date"))
			v["edit_date"] = colI64(e, "edit_date");
		v["observed_at"] = colStr(e, "created_at");
		if (!e["file_id"].isNull())
			v["media"] = mediaJson(e, false);
		out["previous_versions"].push_back(std::move(v));
	}
}

Json run_telegram_get_message_history(const drogon::orm::DbClientPtr &db, const Json &args)
{
	const int64_t gid = requireInt(args, "group_id");
	const int64_t mid = requireInt(args, "message_id");

	/*
	 * Resolve the caller's (group, message) pair to the internal row id
	 * the edit snapshots reference, and apply the gate in the same
	 * statement. The pair is covered by uq_group_messages_chat_msg, so
	 * this is a single index lookup.
	 */
	auto rows = execTool(
		db,
		"SELECT /*+ MAX_EXECUTION_TIME(5000) */ "
		"m.id, m.content_type, m.text, m.date, "
		"m.edit_date, m.deleted_at, m.sender_user_id, "
		"FROM_UNIXTIME(m.date) AS sent_at, "
		"f.id AS file_id, f.file_type, f.file_size, f.on_disk "
		"FROM telegram_group_messages m "
		"LEFT JOIN telegram_files f ON f.id = m.file_id "
		"WHERE m.chat_id = ? AND m.message_id = ?" +
		std::string(MSG_GATE),
		{ std::to_string(gid), std::to_string(mid) });
	if (rows.empty())
		throw ToolError("no readable message " + std::to_string(mid) +
				" in group " + std::to_string(gid));

	const auto &r = rows[0];
	Json cur;
	cur["content_type"] = colStr(r, "content_type");
	cur["text"] = colStr(r, "text");
	cur["sent_at"] = colStr(r, "sent_at");
	cur["date"] = colI64(r, "date");
	if (!r["file_id"].isNull())
		cur["media"] = mediaJson(r, false);

	Json out;
	out["group_id"] = gid;
	out["message_id"] = mid;
	out["sender_user_id"] = colI64(r, "sender_user_id");

	const bool edited = colI64(r, "edit_date") != 0;
	const bool deleted = !r["deleted_at"].isNull();
	out["is_edited"] = edited;
	out["is_deleted"] = deleted;
	if (edited)
		out["last_edit_date"] = colI64(r, "edit_date");
	if (deleted) {
		/* When the deletion was NOTICED. The content is still here;
		 * deleting on Telegram does not unsay it. */
		out["deleted_observed_at"] = colStr(r, "deleted_at");
	}
	out["current"] = std::move(cur);

	msgHistVersions(db, out, colI64(r, "id"));

	const size_t n = out["previous_versions"].size();
	out["previous_version_count"] = n;

	/*
	 * Say plainly when the archive knows a message changed but cannot
	 * show how. Silence here would read as "nothing was edited", which
	 * is the opposite of the truth.
	 */
	if (edited && n == 0) {
		out["note"] =
			"This message is marked edited, "
			"but no earlier version was "
			"captured -- the edit happened "
			"before the message was archived "
			"or while the logger was not "
			"running.";
	}
	return out;
}

void add_telegram_get_message_history(gwmcp::ToolRegistry &registry,
                                      const drogon::orm::DbClientPtr &db)
{
	gwmcp::Tool t;
	t.name = "telegram_get_message_history";
	t.title = "Get one message's edit and deletion history";
	t.description =
		"What a single message used to say, and whether it was "
		"deleted. Returns the current version, every earlier "
		"version the archive captured (oldest first), and the "
		"deletion record if there is one.\n\n"
		"IMPORTANT -- edited and recoverable are not the same. "
		"Telegram marks a message as edited, but an earlier "
		"version exists only if the daemon was running and saw "
		"the edit happen. A message edited before it was first "
		"archived, or while the daemon was down, is flagged "
		"is_edited with no previous version to show. The "
		"response says so explicitly rather than looking "
		"like nothing changed: check `previous_version_count` "
		"against `is_edited`.\n\n"
		"Deleted messages keep their content here: a deletion "
		"records WHEN it was observed, it does not erase what "
		"was said.\n\n"
		"Identify the message by group_id plus message_id, as "
		"returned by the message tools. Only readable groups.";
	t.inputSchema = Json{
		{ "type", "object" },
		{ "properties",
		  Json{ { "group_id",
			  propGroupId() },
			{ "message_id",
			  Json{ { "type", "integer" },
				{ "description",
				  "The message's id within that "
				  "group." } } } } },
		{ "required",
		  Json::array({ "group_id", "message_id" }) },
	};
	t.handler = [db](const Json &args) {
		return run_telegram_get_message_history(db, args);
	};
	registry.add(std::move(t));
}

/* ---- telegram_get_user ---- */
Json run_telegram_get_user(const drogon::orm::DbClientPtr &db, const Json &args)
{
	const int64_t uid = requireInt(args, "user_id");
	const std::string bind = std::to_string(uid);

	const std::string sql =
		"SELECT /*+ MAX_EXECUTION_TIME(5000) */ "
		"u.id, u.first_name, u.last_name, u.type, "
		"u.profile_photo_file_id, u.accent_color_id, "
		"u.is_verified, u.is_scam, u.is_fake, "
		"u.is_premium, u.is_support, "
		"u.birthday_day, u.birthday_month, "
		"u.birthday_year, u.msg_count, "
		"u.created_at, u.updated_at, "
		"e.bio, e.phone_number, e.language_code, "
		"e.restriction_reason, e.has_sensitive_content, "
		"e.restricts_new_chats, e.paid_message_star_count, "
		"e.personal_chat_id, e.emoji_status_custom_emoji_id, "
		"e.profile_accent_color_id "
		"FROM telegram_users u "
		"LEFT JOIN telegram_user_extra_info e "
		"  ON e.user_id = u.id "
		"WHERE u.id = ?";

	Json out;
	auto rows = execTool(db, sql, { bind });
	if (rows.empty())
		throw ToolError(
			"no user with id " + bind +
			" in the archive");

	const auto &r = rows[0];
	out["user_id"] = colI64(r, "id");
	out["first_name"] = colStr(r, "first_name");
	out["last_name"] = colStr(r, "last_name");
	out["type"] = colStr(r, "type");
	out["is_bot"] = colStr(r, "type") == "bot";
	out["is_deleted"] = colStr(r, "type") == "deleted";

	/*
	 * Flags are always present, so they are always
	 * returned: an absent flag would be ambiguous
	 * between false and unknown.
	 */
	out["is_verified"] = colI64(r, "is_verified") != 0;
	out["is_premium"] = colI64(r, "is_premium") != 0;
	out["is_scam"] = colI64(r, "is_scam") != 0;
	out["is_fake"] = colI64(r, "is_fake") != 0;
	out["is_support"] = colI64(r, "is_support") != 0;

	out["message_count"] = colI64(r, "msg_count");
	out["first_seen"] = colStr(r, "created_at");
	out["last_updated"] = colStr(r, "updated_at");

	/*
	 * Everything below is optional. Omitting what
	 * was never seen -- rather than returning ""
	 * or 0 -- is the difference between "this user
	 * has no bio" and "we never learned one", and
	 * telegram_user_extra_info is sparse enough
	 * (56k rows for 298k users) that the
	 * distinction matters constantly.
	 */
	const std::string bio = colStr(r, "bio");
	if (!bio.empty())
		out["bio"] = bio;
	const std::string phone = colStr(r, "phone_number");
	if (!phone.empty())
		out["phone_number"] = phone;
	const std::string lang = colStr(r, "language_code");
	if (!lang.empty())
		out["language_code"] = lang;
	const std::string restr =
		colStr(r, "restriction_reason");
	if (!restr.empty())
		out["restriction_reason"] = restr;
	if (colI64(r, "has_sensitive_content"))
		out["has_sensitive_content"] = true;
	if (colI64(r, "restricts_new_chats"))
		out["restricts_new_chats"] = true;
	if (colI64(r, "paid_message_star_count"))
		out["paid_message_star_count"] =
			colI64(r, "paid_message_star_count");
	if (colI64(r, "personal_chat_id"))
		out["personal_chat_id"] =
			colI64(r, "personal_chat_id");
	if (colI64(r, "emoji_status_custom_emoji_id"))
		out["has_emoji_status"] = true;

	/* Telegram lets a user publish a birthday with
	 * no year, so the parts are reported as given
	 * rather than assembled into a false date. */
	const int64_t bd = colI64(r, "birthday_day");
	const int64_t bm = colI64(r, "birthday_month");
	const int64_t by = colI64(r, "birthday_year");
	if (bd || bm || by) {
		Json b;
		if (bd) b["day"] = bd;
		if (bm) b["month"] = bm;
		if (by) b["year"] = by;
		out["birthday"] = std::move(b);
	}

	const int64_t photo =
		colI64(r, "profile_photo_file_id");
	if (photo) {
		out["profile_photo_file_id"] = photo;
		/* The fetchable link. Omitted rather
		 * than emitted relative when no public
		 * base URL is configured, since a
		 * relative path is useless to a client
		 * that is not a browser on this site. */
		const std::string u =
			fileurl::forFile((uint64_t)photo);
		if (!u.empty())
			out["profile_photo_url"] = u;
	}

	/* Every active username, not just the first: a user may
	 * hold several, and which is "primary" is only the
	 * lowest position. */
	out["usernames"] = Json::array();
	for (const auto &r : execTool(
		     db,
		     "SELECT username, is_collectible "
		     "FROM telegram_user_usernames "
		     "WHERE user_id = ? AND kind = 'active' "
		     "ORDER BY position",
		     { bind }, "username lookup failed")) {
		Json j;
		j["username"] = colStr(r, "username");
		if (colI64(r, "is_collectible"))
			j["is_collectible"] = true;
		out["usernames"].push_back(std::move(j));
	}
	return out;
}

void add_telegram_get_user(gwmcp::ToolRegistry &registry,
                           const drogon::orm::DbClientPtr &db)
{
	gwmcp::Tool t;
	t.name = "telegram_get_user";
	t.title = "Get one user's full profile";
	t.description =
		"Everything the archive holds about one user, by id: "
		"names, every active username, bio, phone number, "
		"language, birthday, account flags and counts.\n\n"
		"Use telegram_get_users to FIND a user by username, "
		"name or phone; use this once you have the id and want "
		"the whole record. Fields the archive never saw are "
		"omitted rather than returned empty, so what comes back "
		"is what is actually known.\n\n"
		"Not restricted to exposed groups: this describes an "
		"account, not a conversation.";
	t.inputSchema = Json{
		{ "type", "object" },
		{ "properties",
		  Json{ { "user_id",
			  Json{ { "type", "integer" },
				{ "description",
				  "Telegram user id (positive)." } } } } },
		{ "required", Json::array({ "user_id" }) },
	};
	t.handler = [db](const Json &args) {
		return run_telegram_get_user(db, args);
	};
	registry.add(std::move(t));
}

/* ---- telegram_get_user_history ---- */
/* The two user-history sections with per-row logic. */
void userHistUsernames(const drogon::orm::DbClientPtr &db, Json &out,
		       const std::string &bind, const std::string &lim)
{
	out["usernames"] = Json::array();
	for (const auto &r : execTool(
		     db,
		     "SELECT username, action, kind, is_collectible, "
		     "created_at FROM telegram_user_hist_usernames_events "
		     "WHERE user_id = ? ORDER BY id DESC LIMIT " + lim,
		     { bind })) {
		Json j;
		j["username"] = colStr(r, "username");
		/* added / removed / activated / deactivated -- the event,
		 * not just the value. */
		j["action"] = colStr(r, "action");
		j["kind"] = colStr(r, "kind");
		if (colI64(r, "is_collectible"))
			j["is_collectible"] = true;
		j["observed_at"] = colStr(r, "created_at");
		out["usernames"].push_back(std::move(j));
	}
}

void userHistPhotos(const drogon::orm::DbClientPtr &db, Json &out,
		    const std::string &bind, const std::string &lim)
{
	out["photos"] = Json::array();
	for (const auto &r : execTool(
		     db,
		     "SELECT file_id, created_at FROM "
		     "telegram_user_hist_profile_photo "
		     "WHERE user_id = ? ORDER BY id DESC LIMIT " + lim,
		     { bind })) {
		const int64_t fid = colI64(r, "file_id");
		Json j;
		j["file_id"] = fid;
		const std::string u = fileurl::forFile((uint64_t)fid);
		if (!u.empty())
			j["url"] = u;
		j["observed_at"] = colStr(r, "created_at");
		out["photos"].push_back(std::move(j));
	}
}

Json run_telegram_get_user_history(const drogon::orm::DbClientPtr &db, const Json &args)
{
	const int64_t uid = requireInt(args, "user_id");
	const std::string bind = std::to_string(uid);
	const std::string lim = std::to_string(clampLimit(args));

	static const char *kNames[5] = { "names", "usernames", "bios",
					 "phone_numbers", "photos" };
	bool want[5];
	parseKinds(args, kNames, 5, want);

	Json out;
	out["user_id"] = uid;
	if (want[0])
		histList(db, out, "names",
			 "SELECT first_name, last_name, created_at FROM "
			 "telegram_user_hist_name WHERE user_id = ? "
			 "ORDER BY id DESC LIMIT " + lim,
			 bind,
			 { { "first_name", "first_name" },
			   { "last_name", "last_name" } });
	if (want[1])
		userHistUsernames(db, out, bind, lim);
	if (want[2])
		histList(db, out, "bios",
			 "SELECT bio, created_at FROM telegram_user_hist_bio "
			 "WHERE user_id = ? ORDER BY id DESC LIMIT " + lim,
			 bind, { { "bio", "bio" } });
	if (want[3])
		histList(db, out, "phone_numbers",
			 "SELECT phone_number, created_at "
			 "FROM telegram_user_hist_phone_num "
			 "WHERE user_id = ? ORDER BY id DESC LIMIT " + lim,
			 bind, { { "phone_number", "phone_number" } });
	if (want[4])
		userHistPhotos(db, out, bind, lim);
	return out;
}

void add_telegram_get_user_history(gwmcp::ToolRegistry &registry,
                                   const drogon::orm::DbClientPtr &db)
{
	gwmcp::Tool t;
	t.name = "telegram_get_user_history";
	t.title = "Get a user's profile change history";
	t.description =
		"How a user's profile has changed over time: names, "
		"usernames, bios, phone numbers and profile photos, "
		"newest first.\n\n"
		"The archive records a row when a change is OBSERVED, "
		"so a timestamp is when the daemon noticed, not "
		"necessarily when the user made the change, and a "
		"change made and reverted between observations is "
		"invisible. The oldest entry of each kind is usually "
		"the value at first sight rather than a change.\n\n"
		"Pass `kinds` to fetch only some categories; the "
		"default is all of them.\n\n"
		"Not restricted to exposed groups: this describes an "
		"account, not a conversation.";
	t.inputSchema = Json{
		{ "type", "object" },
		{ "properties",
		  Json{ { "user_id",
			  Json{ { "type", "integer" },
				{ "description",
				  "Telegram user id (positive)." } } },
			{ "kinds",
			  Json{ { "type", "array" },
				{ "items", Json{ { "type", "string" } } },
				{ "description",
				  "Any of: names, usernames, bios, "
				  "phone_numbers, photos. Default all." } } },
			{ "limit",
			  Json{ { "type", "integer" },
				{ "description",
				  "Entries per category, 1-200, "
				  "default 50." } } } } },
		{ "required", Json::array({ "user_id" }) },
	};
	t.handler = [db](const Json &args) {
		return run_telegram_get_user_history(db, args);
	};
	registry.add(std::move(t));
}

/* ---- telegram_get_users ---- */
Json run_telegram_get_users(const drogon::orm::DbClientPtr &db, const Json &args)
{
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
	for (const auto &r : execTool(db, sql, c.binds)) {
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
	out["count"] = out["users"].size();
	return out;
}

void add_telegram_get_users(gwmcp::ToolRegistry &registry,
                            const drogon::orm::DbClientPtr &db)
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
			  propLimit() },
			{ "offset", Json{ { "type", "integer" } } } } }
	};
	t.handler = [db](const Json &args) {
		return run_telegram_get_users(db, args);
	};
	registry.add(std::move(t));
}

} /* namespace */

void registerTools(gwmcp::ToolRegistry &registry, drogon::orm::DbClientPtr db)
{
	add_telegram_list_groups(registry, db);
	add_telegram_get_group(registry, db);
	add_telegram_get_group_history(registry, db);
	add_telegram_list_group_admins(registry, db);
	add_telegram_list_recent_messages(registry, db);
	add_telegram_list_group_senders(registry, db);
	add_telegram_count_user_messages(registry, db);
	add_telegram_popular_words(registry, db);
	add_telegram_search_messages(registry, db);
	add_telegram_get_message_history(registry, db);
	add_telegram_get_user(registry, db);
	add_telegram_get_user_history(registry, db);
	add_telegram_get_users(registry, db);
}


} /* namespace tgweb::mcp::telegram */
