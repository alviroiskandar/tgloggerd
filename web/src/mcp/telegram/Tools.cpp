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

/* Ids per batch call. Matches the filter grammar's MAX_IN_ITEMS, so a
 * telegram_get_users {"op":"in"} batch and a telegram_get_messages batch have
 * the same ceiling and a caller only has to remember one number. */
constexpr size_t MAX_BATCH_IDS = 100;

/*
 * THE EXPOSURE GATE.
 *
 * Every message query is ANDed with this. It is produced in exactly one place
 * so that "did we remember the gate?" is answerable by grep rather than by
 * reading each query, and it fails closed: an empty allowlist compiles to
 * `1=0`, which matches nothing, rather than to an empty IN list, which is not
 * even valid SQL.
 *
 * It binds a LITERAL LIST of allowed ids rather than the semi-join it used to
 * be (`chat_id IN (SELECT group_id FROM telegram_public_groups)`). That is a
 * performance decision, and a large one. With the subquery MySQL drives the
 * join from the allowlist table and estimates ~90 rows per group against a
 * real 238k, so it never picks the (chat_id, <timestamp>) indexes: ordering by
 * edit_date measured 6.18 seconds -- past the statement timeout -- WITH the
 * index present. Bound constants let the optimiser turn the same query into a
 * reverse covering range scan: 0.012 s. Every other shape improved too
 * (date-ordered 0.138 s -> 0.0024 s, full-text 0.136 s -> 0.048 s), so there
 * is no case where the old form was better.
 *
 * The allowlist is curated and therefore small, which is what makes inlining
 * it affordable.
 */
struct Gate {
	std::string              sql;   /* " AND m.chat_id IN (?,?)" or " AND 1=0" */
	std::vector<std::string> binds;
	bool matchesNothing = false;    /* nothing readable; skip the query */
};

/* The exposed group ids. Cheap: a covering scan of a curated table. */
std::vector<int64_t> exposedGroups(const drogon::orm::DbClientPtr &db);

/*
 * Build the gate, optionally narrowed to one group.
 *
 * A group the caller named but that is not on the allowlist yields
 * matchesNothing rather than an error: refusing explicitly would confirm the
 * group exists, which is itself something the allowlist withholds.
 */
Gate msgGate(const drogon::orm::DbClientPtr &db, const Json &args,
	     const char *groupKey = "group_id")
{
	Gate g;
	std::vector<int64_t> groups = exposedGroups(db);

	if (args.contains(groupKey) && args[groupKey].is_number_integer()) {
		const int64_t want = args[groupKey].get<long long>();
		const bool ok = std::find(groups.begin(), groups.end(), want) !=
				groups.end();
		groups.clear();
		if (ok)
			groups.push_back(want);
	}

	if (groups.empty()) {
		g.sql = " AND 1=0";
		g.matchesNothing = true;
		return g;
	}

	g.sql = " AND m.chat_id IN (";
	for (size_t i = 0; i < groups.size(); i++) {
		g.sql += i ? ",?" : "?";
		g.binds.push_back(std::to_string(groups[i]));
	}
	g.sql += ")";
	return g;
}

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

/*
 * Message fields a caller may filter on.
 *
 * Three timestamps, and the difference between them is the whole point of
 * `edit_date` and `deleted_at` existing here: `date` is when a message was
 * SENT. A message sent in January and edited in August does not appear in any
 * window on `date`, so "what changed this week?" was previously not
 * expressible -- a caller had to page the week by send time and would silently
 * miss exactly the edits it was looking for.
 */
const flt::Field kMsgFields[] = {
	{ "text", "m.text", flt::FType::FullText, "",
	  "message body; word-based, so words shorter than 3 characters are "
	  "ignored by the index", "" },
	{ "group_id", "m.chat_id", flt::FType::Int, "",
	  "the group's id (negative)", "" },
	{ "sender_user_id", "m.sender_user_id", flt::FType::Int, "",
	  "author's Telegram user id", "" },
	{ "message_id", "m.message_id", flt::FType::Int, "",
	  "per-group message id", "" },
	{ "date", "m.date", flt::FType::DateTs, "",
	  "when it was SENT; accepts YYYY-MM-DD or a unix timestamp", "" },
	{ "content_type", "m.content_type", flt::FType::Enum,
	  "text,photo,video,document,audio,voice,sticker,animation,service,unknown",
	  "kind of message", "" },
	{ "is_forwarded", "m.is_forwarded", flt::FType::Bool, "", "", "" },
	{ "is_channel_post", "m.is_channel_post", flt::FType::Bool, "", "", "" },
	{ "deleted", "m.deleted_at", flt::FType::Bool, "",
	  "use op is_null for live messages, is_not_null for deleted ones", "" },
	{ "edited", "m.edit_date", flt::FType::Presence, "",
	  "use op is_not_null for messages that were edited, is_null for "
	  "untouched ones -- the mirror of `deleted`", "" },
	{ "edit_date", "m.edit_date", flt::FType::DateTs, "",
	  "when it was LAST EDITED. Use this, not `date`, to find edits in a "
	  "window; ordering by it is index-backed", "" },
	{ "deleted_at", "m.deleted_at", flt::FType::DateTs, "",
	  "when the deletion was OBSERVED. Use this, not `date`, to find "
	  "deletions in a window", "FROM_UNIXTIME(?)" },
	{ "sender_is_bot", "EXISTS(SELECT 1 FROM telegram_users bu "
			   "WHERE bu.id = m.sender_user_id AND bu.type = 'bot')",
	  flt::FType::Bool, "",
	  "true for messages sent by a bot -- excludes bot noise without "
	  "having to know any bot's id", "" },
	{ "text_length", "CHAR_LENGTH(m.text)", flt::FType::Int, "",
	  "characters of text; use < to skip megaposts while scanning", "" },
};
const flt::Schema kMsgSchema{ kMsgFields,
			      sizeof(kMsgFields) / sizeof(kMsgFields[0]) };

/* User fields. */
const flt::Field kUserFields[] = {
	{ "user_id", "u.id", flt::FType::Int, "",
	  "Telegram user id; with op \"in\" this is the batch path -- it is "
	  "the primary key, so a list of up to 100 ids is a single indexed "
	  "lookup", "" },
	{ "username", "un.username", flt::FType::Text, "",
	  "current public username, without the @", "" },
	{ "first_name", "u.first_name", flt::FType::Text, "", "", "" },
	{ "last_name", "u.last_name", flt::FType::Text, "", "", "" },
	{ "phone_number", "COALESCE(e.phone_number,'')", flt::FType::Text, "",
	  "only set for contacts", "" },
	{ "bio", "COALESCE(e.bio,'')", flt::FType::Text, "", "", "" },
	{ "type", "u.type", flt::FType::Enum, "regular,deleted,bot,unknown",
	  "", "" },
	{ "is_bot", "(u.type = 'bot')", flt::FType::Bool, "", "", "" },
	{ "msg_count", "u.msg_count", flt::FType::Int, "",
	  "messages seen from this user across the archive", "" },
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

/*
 * How much of a row to emit.
 *
 * A scan is usually looking for WHICH messages match, not for their contents,
 * and the contents are almost all of the bytes: one multi-kilobyte bot post
 * costs more than a hundred ids. These four controls exist so a caller can ask
 * the cheap question cheaply, and every one defaults to the old full-fat row so
 * an existing client sees no change.
 */
/* Every key messageRow can emit -- the allowlist for the `fields` projection. */
const char *const kRowFields[] = {
	"message_id", "group_id", "group_title", "sender_user_id",
	"sender_name", "sender_username", "date", "sent_at", "content_type",
	"text", "is_forwarded", "is_channel_post", "reply_to_message_id",
	"is_edited", "is_deleted", "edit_date", "deleted_at",
	"previous_version_count", "history_available_via", "media",
};

struct RowOpts {
	std::vector<std::string> fields;      /* empty = every key */
	size_t truncateText = 0;             /* 0 = never truncate */
	bool includeMedia = true;
	bool compact = false;                /* drop false/empty, hoist group */
	bool hoistGroup = false;             /* caller named one group */

	bool wants(const char *k) const
	{
		if (fields.empty())
			return true;
		for (const auto &f : fields)
			if (f == k)
				return true;
		return false;
	}
};

/*
 * The size controls, read from a tool's arguments.
 *
 * `compact` also implies hoisting the group columns, but only when the caller
 * named the group -- otherwise they differ per row and are not invariant.
 */
RowOpts rowOpts(const Json &args)
{
	RowOpts o;
	if (args.contains("fields")) {
		if (!args["fields"].is_array())
			throw ToolError("\"fields\" must be an array of strings");
		for (const auto &f : args["fields"]) {
			if (!f.is_string())
				throw ToolError("\"fields\" must contain strings");
			const std::string k = f.get<std::string>();
			/*
			 * A misspelled key would otherwise be silently dropped,
			 * and the caller would get rows missing the very field
			 * they asked for with no hint why. Naming the valid set
			 * lets a model correct itself instead of guessing.
			 */
			bool known = false;
			for (const char *v : kRowFields) {
				if (k == v) {
					known = true;
					break;
				}
			}
			if (!known) {
				std::string all;
				for (const char *v : kRowFields)
					all += std::string(all.empty() ? "" : ", ") + v;
				throw ToolError("unknown field \"" + k +
						"\" in \"fields\"; expected any of: " +
						all);
			}
			o.fields.push_back(k);
		}
	}
	if (args.contains("truncate_text") &&
	    args["truncate_text"].is_number_integer()) {
		const long long n = args["truncate_text"].get<long long>();
		o.truncateText = n <= 0 ? 0 : (size_t)n;
	}
	if (args.contains("include_media") && args["include_media"].is_boolean())
		o.includeMedia = args["include_media"].get<bool>();
	if (args.contains("compact") && args["compact"].is_boolean())
		o.compact = args["compact"].get<bool>();
	o.hoistGroup = o.compact && args.contains("group_id") &&
		       args["group_id"].is_number_integer();
	return o;
}

/*
 * Cut text to at most `max` BYTES without splitting a UTF-8 sequence, the same
 * rule the Discord forwarder uses. Returns true when anything was removed, so
 * the row can say so rather than leaving a caller to guess whether a message
 * really ended mid-word.
 */
bool truncateUtf8(std::string &t, size_t max)
{
	if (!max || t.size() <= max)
		return false;
	size_t cut = max;
	while (cut > 0 && ((unsigned char)t[cut] & 0xC0) == 0x80)
		cut--;
	t.resize(cut);
	return true;
}

/*
 * Opaque pagination cursor.
 *
 * OFFSET is wrong for a live archive: new messages arrive while a client is
 * paging, every later page shifts by however many landed, and rows are silently
 * seen twice or missed entirely. A cursor names the last row instead -- its
 * sort value and its row id -- so the next page resumes exactly where the
 * previous one stopped no matter what was inserted meanwhile.
 *
 * The sort key travels inside the cursor and is checked on the way back in: a
 * cursor minted under `order_by: edit_date` is meaningless to a `date` scan,
 * and silently applying it would return plausible nonsense.
 *
 * base64url of "key|value|id". Opaque by intent -- clients must not construct
 * or parse it -- but trivially decodable by us, which is what makes a bad one
 * diagnosable rather than mysterious.
 */
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
		    "0123456789-_";

std::string b64uEncode(const std::string &in)
{
	std::string out;
	size_t i = 0;
	for (; i + 3 <= in.size(); i += 3) {
		const uint32_t v = ((uint32_t)(unsigned char)in[i] << 16) |
				   ((uint32_t)(unsigned char)in[i + 1] << 8) |
				   (uint32_t)(unsigned char)in[i + 2];
		out += kB64[(v >> 18) & 63];
		out += kB64[(v >> 12) & 63];
		out += kB64[(v >> 6) & 63];
		out += kB64[v & 63];
	}
	if (i + 1 == in.size()) {
		const uint32_t v = (uint32_t)(unsigned char)in[i] << 16;
		out += kB64[(v >> 18) & 63];
		out += kB64[(v >> 12) & 63];
	} else if (i + 2 == in.size()) {
		const uint32_t v = ((uint32_t)(unsigned char)in[i] << 16) |
				   ((uint32_t)(unsigned char)in[i + 1] << 8);
		out += kB64[(v >> 18) & 63];
		out += kB64[(v >> 12) & 63];
		out += kB64[(v >> 6) & 63];
	}
	return out;
}

bool b64uDecode(const std::string &in, std::string &out)
{
	auto sextet = [](char c) -> int {
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '-') return 62;
		if (c == '_') return 63;
		return -1;
	};
	if (in.empty() || in.size() % 4 == 1 || in.size() > 512)
		return false;
	uint32_t acc = 0;
	int bits = 0;
	out.clear();
	for (char c : in) {
		const int d = sextet(c);
		if (d < 0)
			return false;
		acc = (acc << 6) | (uint32_t)d;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out += (char)((acc >> bits) & 0xFF);
		}
	}
	return true;
}

struct Cursor {
	std::string key;   /* the order column the cursor was minted under */
	std::string value; /* its value on the last row emitted */
	int64_t id = 0;    /* that row's internal id, to break ties */
	bool ok = false;
};

std::string cursorEncode(const std::string &key, const std::string &value,
			 int64_t id)
{
	return b64uEncode(key + "|" + value + "|" + std::to_string(id));
}

Cursor cursorDecode(const std::string &s)
{
	Cursor c;
	std::string raw;
	if (!b64uDecode(s, raw))
		return c;
	const size_t a = raw.find('|');
	if (a == std::string::npos)
		return c;
	const size_t b = raw.find('|', a + 1);
	if (b == std::string::npos)
		return c;
	c.key = raw.substr(0, a);
	c.value = raw.substr(a + 1, b - a - 1);
	c.id = strtoll(raw.c_str() + b + 1, nullptr, 10);
	/* Values are numeric in every ordering we mint; reject anything else
	 * rather than letting a hand-made cursor reach the SQL. */
	if (c.value.empty() ||
	    c.value.find_first_not_of("0123456789-") != std::string::npos)
		return c;
	c.ok = true;
	return c;
}

Json messageRow(const drogon::orm::Row &r, const RowOpts &o)
{
	Json j;
	/*
	 * message_id is emitted whatever the projection says. It is the handle
	 * every follow-up call needs (telegram_get_messages, history), so a row
	 * without one is not a smaller answer, it is an unusable one.
	 */
	j["message_id"] = colI64(r, "message_id");

	/* The group columns repeat identically on every row of a single-group
	 * scan; compact mode hoists them to the envelope instead. */
	if (!o.hoistGroup) {
		if (o.wants("group_id"))
			j["group_id"] = colI64(r, "chat_id");
		if (o.wants("group_title"))
			j["group_title"] = colStr(r, "group_title");
	}

	if (o.wants("sender_user_id"))
		j["sender_user_id"] = colI64(r, "sender_user_id");
	if (o.wants("sender_name")) {
		const std::string v = colStr(r, "sender_name");
		if (!o.compact || !v.empty())
			j["sender_name"] = v;
	}
	if (o.wants("sender_username")) {
		const std::string v = colStr(r, "sender_username");
		if (!o.compact || !v.empty())
			j["sender_username"] = v;
	}
	if (o.wants("date"))
		j["date"] = colI64(r, "date");
	if (o.wants("sent_at"))
		j["sent_at"] = colStr(r, "sent_at");
	if (o.wants("content_type"))
		j["content_type"] = colStr(r, "content_type");

	if (o.wants("text")) {
		/*
		 * RAW text, deliberately. dao::search HTML-escapes every cell
		 * because its output lands in a browser; this output lands in a
		 * model, where "it&#39;s &lt;b&gt;" is simply wrong and
		 * unescaping it later would be lossy.
		 */
		std::string text = colStr(r, "text");
		if (truncateUtf8(text, o.truncateText)) {
			/* Say so rather than let a caller mistake the cut for
			 * the end of the message. The full text is one
			 * telegram_get_messages call away. */
			j["text_truncated"] = true;
		}
		if (!o.compact || !text.empty())
			j["text"] = std::move(text);
	}

	const bool fwd = colI64(r, "is_forwarded") != 0;
	const bool chan = colI64(r, "is_channel_post") != 0;
	if (o.wants("is_forwarded") && (!o.compact || fwd))
		j["is_forwarded"] = fwd;
	if (o.wants("is_channel_post") && (!o.compact || chan))
		j["is_channel_post"] = chan;

	if (o.wants("reply_to_message_id") && !r["reply_to_msg_id"].isNull())
		j["reply_to_message_id"] = colI64(r, "reply_to_msg_id");

	/*
	 * Always present outside compact mode, never inferred from absence. A
	 * reader must be able to tell "not edited" from "the field was
	 * omitted", and a message whose text has since changed or vanished is a
	 * different thing from one that has not -- which matters most when the
	 * content is being quoted. Compact mode is the caller explicitly asking
	 * for the opposite trade, so there false means absent.
	 */
	const bool edited = colI64(r, "edit_date") != 0;
	const bool deleted = !r["deleted_at"].isNull();
	if (o.wants("is_edited") && (!o.compact || edited))
		j["is_edited"] = edited;
	if (o.wants("is_deleted") && (!o.compact || deleted))
		j["is_deleted"] = deleted;
	if (edited && o.wants("edit_date"))
		j["edit_date"] = colI64(r, "edit_date");
	if (deleted && o.wants("deleted_at"))
		j["deleted_at"] = colStr(r, "deleted_at");

	/*
	 * How many earlier versions the archive actually holds, carried on the
	 * row itself. Edited and RECOVERABLE are different things -- an edit
	 * seen before the daemon was watching leaves a flag and no snapshot --
	 * and without this a caller had to make one history call per edited
	 * message just to discover which ones had anything to show.
	 */
	if (edited && o.wants("previous_version_count") &&
	    !r["prev_versions"].isNull())
		j["previous_version_count"] = colI64(r, "prev_versions");

	if ((edited || deleted) && o.wants("history_available_via") &&
	    !o.compact) {
		/* Point at the tools that can say what changed, since the text
		 * above is only the latest version. */
		j["history_available_via"] =
			"telegram_get_messages (include_history) or "
			"telegram_get_message_history";
	}

	if (o.includeMedia && o.wants("media") && !r["file_id"].isNull())
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
	"f.orig_file_name, f.on_disk, m.id AS id_internal, "
	/*
	 * How many earlier versions exist for this message. A correlated
	 * count, but it runs only in phase 2 -- over at most `limit` already
	 * chosen rows -- against a 1.4k-row table keyed by exactly this
	 * column, so it is a handful of index lookups. It saves the caller one
	 * whole round trip per edited message.
	 */
	"(SELECT COUNT(1) FROM telegram_group_message_edits pe "
	"  WHERE pe.group_message_id = m.id) AS prev_versions ";

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
 * How a message scan is ordered, and how a cursor for it is spelled.
 *
 * Only these three columns can be ordered on, because only these three have a
 * (chat_id, <column>) index behind them -- ordering by anything else would
 * filesort the whole gated set. Relevance is a fourth ordering and is special:
 * it has no stable cursor, because a score is not a position.
 */
struct OrderSpec {
	std::string key;      /* date | edit_date | deleted_at | relevance */
	std::string col;      /* SQL column, for ORDER BY and cursor compare */
	std::string keyExpr;  /* same value as a NUMBER, for the cursor */
	std::string valExpr;  /* how a cursor value is spelled back in */
	std::string sql;      /* the full ORDER BY */
	bool asc = false;
	bool cursorable = true;
};

OrderSpec parseOrder(const Json &args, const std::string &ftExpr)
{
	OrderSpec o;
	if (args.contains("order") && args["order"].is_string()) {
		const std::string v = args["order"].get<std::string>();
		if (v != "asc" && v != "desc")
			throw ToolError("\"order\" must be \"asc\" or \"desc\"");
		o.asc = v == "asc";
	}

	o.key = "date";
	if (args.contains("order_by") && args["order_by"].is_string())
		o.key = args["order_by"].get<std::string>();

	if (o.key == "date") {
		o.col = "m.date";
		o.keyExpr = "m.date";
		o.valExpr = "?";
	} else if (o.key == "edit_date") {
		o.col = "m.edit_date";
		o.keyExpr = "m.edit_date";
		o.valExpr = "?";
	} else if (o.key == "deleted_at") {
		/*
		 * deleted_at is a DATETIME while a cursor carries unix seconds,
		 * so the two directions convert on opposite sides: out through
		 * UNIX_TIMESTAMP, back in through FROM_UNIXTIME. Converting on
		 * the way in keeps the column bare and the index usable.
		 */
		o.col = "m.deleted_at";
		o.keyExpr = "UNIX_TIMESTAMP(m.deleted_at)";
		o.valExpr = "FROM_UNIXTIME(?)";
	} else {
		throw ToolError("\"order_by\" must be one of: date, edit_date, "
				"deleted_at");
	}

	const char *dir = o.asc ? "ASC" : "DESC";
	/*
	 * The row id breaks ties. Without it two messages sharing a timestamp
	 * have no defined order between pages, so a cursor could repeat one and
	 * skip the other.
	 */
	o.sql = o.col + " " + std::string(dir) + ", m.id " + dir;

	/*
	 * A full-text search overrides all of it unless the caller asked for a
	 * specific ordering: ordering a MATCH by date makes MySQL filesort
	 * every hit, and on 4.6M rows a broad term simply times out. Relevance
	 * walks the fulltext index in order, so LIMIT stops early -- at the
	 * cost of a cursor, since relevance is not a column to resume from.
	 */
	if (!ftExpr.empty() && !args.contains("order_by")) {
		o.key = "relevance";
		o.sql = "MATCH(" + ftExpr + ") AGAINST(? IN BOOLEAN MODE) DESC";
		o.cursorable = false;
	}
	return o;
}

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
struct QueryReq {
	Gate gate;
	std::string whereExtra;
	std::vector<std::string> binds;   /* whereExtra's binds only */
	OrderSpec order;
	std::string orderBind;            /* relevance term, bound twice */
	Cursor cursor;
	int limit = DEFAULT_LIMIT;
	int offset = 0;
	bool includeTotal = false;
	RowOpts row;
	int64_t hoistedGroup = 0;
};

Json runMessageQuery(const drogon::orm::DbClientPtr &db, const QueryReq &q)
{
	Json out;
	out["messages"] = Json::array();

	/*
	 * Binds are positional and the inner query comes first in the SQL text,
	 * so they are appended in exactly the order the placeholders appear:
	 * gate, filter, cursor, inner relevance term, outer relevance term.
	 */
	std::string where = q.gate.sql;
	std::vector<std::string> binds = q.gate.binds;
	if (!q.whereExtra.empty()) {
		where += " AND " + q.whereExtra;
		binds.insert(binds.end(), q.binds.begin(), q.binds.end());
	}

	std::string pageWhere = where;
	std::vector<std::string> pageBinds = binds;
	if (q.cursor.ok) {
		/*
		 * Resume strictly after the last row emitted: past it on the
		 * sort column, or level with it and past it on the id.
		 */
		const std::string cmp = q.order.asc ? ">" : "<";
		pageWhere += " AND (" + q.order.col + " " + cmp + " " +
			     q.order.valExpr + " OR (" + q.order.col + " = " +
			     q.order.valExpr + " AND m.id " + cmp + " ?))";
		pageBinds.push_back(q.cursor.value);
		pageBinds.push_back(q.cursor.value);
		pageBinds.push_back(std::to_string(q.cursor.id));
	}

	/* limit 0 is a legitimate ask: "how many, without the rows". */
	if (q.limit > 0 && !q.gate.matchesNothing) {
		const std::string inner =
			"SELECT /*+ MAX_EXECUTION_TIME(5000) */ m.id, " +
			q.order.keyExpr + " AS ckey "
			"FROM telegram_group_messages m WHERE 1=1" + pageWhere +
			" ORDER BY " + q.order.sql +
			" LIMIT " + std::to_string(q.limit) +
			" OFFSET " + std::to_string(q.offset);

		const std::string sql =
			std::string(MSG_SELECT) + ", sel.ckey "
			"FROM (" + inner + ") sel " +
			"JOIN telegram_group_messages m ON m.id = sel.id " +
			MSG_JOINS + "ORDER BY " + q.order.sql;

		std::vector<std::string> allBinds = pageBinds;
		if (!q.orderBind.empty()) {
			allBinds.push_back(q.orderBind); /* inner ORDER BY */
			allBinds.push_back(q.orderBind); /* outer ORDER BY */
		}

		int64_t lastId = 0;
		std::string lastKey;
		for (const auto &r : execTool(db, sql, allBinds)) {
			out["messages"].push_back(messageRow(r, q.row));
			lastId = colI64(r, "id_internal");
			lastKey = colStr(r, "ckey");
		}

		/*
		 * A cursor only when a full page came back: a short page is the
		 * end, and handing one out there invites a pointless extra
		 * round trip that returns nothing.
		 */
		if (q.order.cursorable && lastId &&
		    (int)out["messages"].size() == q.limit) {
			out["next_cursor"] = cursorEncode(
				q.order.key, lastKey.empty() ? "0" : lastKey,
				lastId);
		}
	}

	out["count"] = out["messages"].size();
	out["limit"] = q.limit;
	if (!q.cursor.ok)
		out["offset"] = q.offset;
	out["order_by"] = q.order.key;
	out["order"] = q.order.asc ? "asc" : "desc";
	if (q.row.hoistGroup)
		out["group_id"] = q.hoistedGroup;

	if (q.includeTotal) {
		/*
		 * Opt-in: counting a broad filter over 4.6M rows is the
		 * expensive half, and a caller reading the first page rarely
		 * needs it. No ORDER BY here, so no order bind -- and no cursor
		 * clause either, since a total is of the whole match, not of
		 * what is left after the page already read.
		 */
		if (q.gate.matchesNothing) {
			out["total"] = 0;
		} else {
			const std::string csql =
				"SELECT /*+ MAX_EXECUTION_TIME(5000) */ "
				"COUNT(1) AS n FROM telegram_group_messages m "
				"WHERE 1=1" + where;
			/*
			 * A count that times out must not destroy the answer.
			 * Counting a broad full-text match is the case that
			 * does it: the fulltext index carries no chat_id, so a
			 * common term walks every match in the whole 5M-row
			 * table before the group filter can reject them --
			 * measured at 11 seconds for "kernel" against a
			 * 5-second limit, and just as slow before this tool
			 * ever offered a count. The rows are already in hand at
			 * this point, so return them and say why the total is
			 * missing, rather than throwing away a good page over
			 * an optional extra.
			 */
			try {
				auto cr = execTool(db, csql, binds,
						   "count failed");
				out["total"] = cr.empty()
						       ? 0
						       : cr[0]["n"].as<int64_t>();
			} catch (const ToolError &) {
				out["total_unavailable"] =
					"the total could not be counted within "
					"the time limit -- this happens with a "
					"broad text match, whose count scans "
					"every match in the archive before the "
					"group filter applies. Narrow the "
					"match, or drop include_total; the "
					"messages above are unaffected.";
			}
		}
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
		 const std::vector<int64_t> &ids, const RowOpts &o)
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
		arr.push_back(messageRow(r, o));
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
		const std::vector<int64_t> &groups, int limit, int offset,
		const RowOpts &o)
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
	out["messages"] = decorateIds(db, ids, o);
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

/*
 * The response-size controls, shared by every tool that returns message rows.
 *
 * A scan usually wants to know WHICH messages match, and the text is nearly all
 * of the bytes -- one multi-kilobyte post outweighs a hundred ids. Each of
 * these defaults to the old full row, so an existing client sees no change.
 */
void addRowShapeProps(Json &props)
{
	props["fields"] = Json{
		{ "type", "array" },
		{ "items", Json{ { "type", "string" } } },
		{ "description",
		  "Return only these keys per message, e.g. "
		  "[\"message_id\",\"sender_username\",\"date\",\"edit_date\"]. "
		  "Valid: message_id, group_id, group_title, sender_user_id, "
		  "sender_name, sender_username, date, sent_at, content_type, "
		  "text, is_forwarded, is_channel_post, reply_to_message_id, "
		  "is_edited, is_deleted, edit_date, deleted_at, "
		  "previous_version_count, history_available_via, media. "
		  "message_id is always included -- it is the handle every "
		  "follow-up call needs. Cuts response size several-fold on a "
		  "wide scan." }
	};
	props["truncate_text"] = Json{
		{ "type", "integer" },
		{ "description",
		  "Cut each message's text to this many bytes; rows that were "
		  "cut carry text_truncated: true. Retrieve the full text of "
		  "the ones that matter with telegram_get_messages." }
	};
	props["include_media"] = Json{
		{ "type", "boolean" },
		{ "description",
		  "Default true. Set false to drop the media object (url, "
		  "size, filename) -- the largest per-row cost, and rarely "
		  "needed while scanning." }
	};
	props["compact"] = Json{
		{ "type", "boolean" },
		{ "description",
		  "Default false. Omits false and empty fields instead of "
		  "serialising them, and when group_id was passed, hoists "
		  "group_id/group_title out of every row into the envelope." }
	};
}

/* Ordering and paging, shared by the tools that scan messages. */
void addPagingProps(Json &props)
{
	props["order_by"] = Json{
		{ "type", "string" },
		{ "enum", Json::array({ "date", "edit_date", "deleted_at" }) },
		{ "description",
		  "Which timestamp to sort on. Default date (when sent). Use "
		  "edit_date to list what changed, deleted_at to list what was "
		  "removed -- both are index-backed. With a text match and no "
		  "order_by, results come back by relevance instead." }
	};
	props["order"] = Json{
		{ "type", "string" },
		{ "enum", Json::array({ "desc", "asc" }) },
		{ "description", "Default desc (newest first)." }
	};
	props["cursor"] = Json{
		{ "type", "string" },
		{ "description",
		  "Pass back the next_cursor from the previous response to get "
		  "the next page. PREFER THIS OVER offset: the archive is live, "
		  "so offsets shift as new messages arrive and rows get "
		  "duplicated or skipped mid-scan. A cursor resumes exactly "
		  "where the last page stopped. It is opaque -- pass it "
		  "verbatim, do not build one." }
	};
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
	props["limit"] = propLimit(
		"0-200, default 50. Use limit: 0 with include_total to get "
		"just the count, so a job can be sized before paging it.");
	props["offset"] = Json{
		{ "type", "integer" },
		{ "description",
		  "Rows to skip; default 0. Prefer cursor for paging." }
	};
	props["include_total"] = Json{
		{ "type", "boolean" },
		{ "description",
		  "Also return the total match count. Off by default because "
		  "counting a broad filter is expensive. With a broad text "
		  "match the count can exceed the time limit; when it does the "
		  "messages are still returned and total_unavailable explains "
		  "why, so the call is never wasted." }
	};
	if (withFilter)
		addPagingProps(props);
	addRowShapeProps(props);
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
Json run_telegram_get_group_one(const drogon::orm::DbClientPtr &db,
				const Json &args, int64_t gidArg);

Json run_telegram_get_group(const drogon::orm::DbClientPtr &db, const Json &args)
{
	/*
	 * Batch form. Kept separate from the single form rather than folded
	 * into it because the single form's response is the flat object
	 * existing callers already parse; wrapping that in a list for everyone
	 * would break them for no gain.
	 */
	if (args.contains("group_ids")) {
		if (!args["group_ids"].is_array())
			throw ToolError("group_ids must be an array of "
					"integers");
		if (args["group_ids"].empty())
			throw ToolError("group_ids must not be empty");
		if (args["group_ids"].size() > MAX_BATCH_IDS)
			throw ToolError("group_ids accepts at most " +
					std::to_string(MAX_BATCH_IDS) +
					" ids per call");
		Json out;
		out["groups"] = Json::array();
		out["missing_ids"] = Json::array();
		for (const auto &v : args["group_ids"]) {
			if (!v.is_number_integer())
				throw ToolError("group_ids must contain "
						"integers");
			const int64_t g = v.get<long long>();
			/*
			 * One unreadable id must not sink the batch, for the
			 * same reason telegram_get_messages reports missing
			 * ids: the caller would otherwise have to bisect to
			 * find which id was the problem.
			 */
			try {
				out["groups"].push_back(
					run_telegram_get_group_one(db, args, g));
			} catch (const ToolError &) {
				out["missing_ids"].push_back(g);
			}
		}
		out["count"] = out["groups"].size();
		out["missing_count"] = out["missing_ids"].size();
		return out;
	}
	return run_telegram_get_group_one(db, args,
					  requireInt(args, "group_id"));
}

Json run_telegram_get_group_one(const drogon::orm::DbClientPtr &db,
				const Json &args, int64_t gidArg)
{
	(void)args; /* the single-group body takes no options today */
	const int64_t gid = gidArg;
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
		"queried; anything else is reported as not found.\n\n"
		"Pass group_ids: [...] instead of group_id to look up several "
		"groups in one call.";
	t.inputSchema = Json{
		{ "type", "object" },
		{ "properties",
		  Json{ { "group_id", propGroupId() },
			{ "group_ids",
			  Json{ { "type", "array" },
				{ "items", Json{ { "type", "integer" } } },
				{ "description",
				  "Look up several groups in one call (at "
				  "most " + std::to_string(MAX_BATCH_IDS) +
					  "). Returns {groups, missing_ids} "
				  "instead of a single group object; "
				  "unreadable ids are reported rather than "
				  "failing the batch." } } } } },
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
	if (args.contains("group_id") && args["group_id"].is_number_integer()) {
		const int64_t g = requireInt(args, "group_id");
		/* Honour the gate: an unexposed group simply has no rows to
		 * offer. */
		for (int64_t e : exposedGroups(db)) {
			if (e == g)
				groups.push_back(g);
		}
	} else {
		groups = exposedGroups(db);
	}

	RowOpts o = rowOpts(args);
	Json out = listRecent(db, groups, clampLimit(args), clampOffset(args), o);
	if (o.hoistGroup)
		out["group_id"] = args["group_id"].get<long long>();
	return out;
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
	const Gate gate = msgGate(db, args);
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
		gate.sql + dr.sql +
		/*
		 * By date, not message_id: the range is on date, so
		 * idx_group_messages_chat_date (migration 000024) supplies
		 * this ordering for free. Ordering by message_id instead
		 * ranges on one index and sorts by another, which is a
		 * filesort over the whole window.
		 */
		" ORDER BY m.date DESC LIMIT " + std::to_string(scanCap);

	std::vector<std::string> binds{ std::to_string(gid) };
	binds.insert(binds.end(), gate.binds.begin(), gate.binds.end());
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

	QueryReq q;
	/* group_id is folded into the gate rather than ANDed separately, so
	 * the optimiser sees one constant list instead of two conditions. */
	q.gate = msgGate(db, args);
	q.whereExtra = c.sql;
	q.binds = c.binds;
	q.order = parseOrder(args, c.hasFullText() ? c.ftExpr : std::string());
	if (q.order.key == "relevance")
		q.orderBind = c.ftValue;
	q.limit = clampIntArg(args, "limit", DEFAULT_LIMIT, 0, MAX_LIMIT);
	q.offset = clampOffset(args);
	q.includeTotal = args.contains("include_total") &&
			 args["include_total"].is_boolean() &&
			 args["include_total"].get<bool>();
	q.row = rowOpts(args);
	if (q.row.hoistGroup)
		q.hoistedGroup = args["group_id"].get<long long>();

	if (args.contains("cursor") && args["cursor"].is_string()) {
		q.cursor = cursorDecode(args["cursor"].get<std::string>());
		if (!q.cursor.ok)
			throw ToolError("cursor is not one this server issued; "
					"pass back next_cursor verbatim, or omit "
					"it to start over");
		if (q.cursor.key != q.order.key)
			throw ToolError(
				"this cursor was issued for order_by \"" +
				q.cursor.key + "\" but the call asks for \"" +
				q.order.key +
				"\"; a cursor is only valid for the ordering "
				"that produced it");
		if (!q.order.cursorable)
			throw ToolError("relevance ordering has no cursor; add "
					"an explicit order_by to page");
	}

	return runMessageQuery(db, q);
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
		"telegram_list_groups.\n\n"
		"WORKING WITH EDITS AND DELETIONS. `date` is when a message "
		"was SENT, so a message sent in January and edited in August "
		"appears in no window on `date` -- filter on `edit_date` or "
		"`deleted_at` instead, and order by them. Every edited row "
		"carries previous_version_count, so you can tell which edits "
		"actually have a recoverable revision WITHOUT a follow-up "
		"call, then fetch just those with telegram_get_messages "
		"(include_history: true).\n\n"
		"Example -- everything edited in one week, cheapest form:\n"
		"{\"filter\":{\"field\":\"edit_date\",\"op\":\"between\","
		"\"value\":[\"2026-01-01\",\"2026-01-08\"]},"
		"\"order_by\":\"edit_date\",\"limit\":200,"
		"\"fields\":[\"message_id\",\"sender_username\","
		"\"edit_date\",\"previous_version_count\"],"
		"\"include_media\":false,\"compact\":true}\n\n"
		"To exclude bot noise without knowing any bot's id, add "
		"{\"not\":{\"field\":\"sender_is_bot\",\"op\":\"=\","
		"\"value\":true}}.\n\n"
		"SIZE AND PAGING. Use fields/truncate_text/include_media/"
		"compact to keep a wide scan small, cursor (not offset) to "
		"page a live archive, and limit: 0 with include_total when you "
		"only need to know how big the job is.";
	t.inputSchema = inputSchemaForMessages(true);
	t.handler = [db](const Json &args) {
		return run_telegram_search_messages(db, args);
	};
	registry.add(std::move(t));
}

/*
 * A unified diff between two versions of a message.
 *
 * A one-word correction to a long post currently costs the whole post twice --
 * once as the old version, once as the new -- and the reader still has to spot
 * what moved. A diff is both smaller and the actual answer to "what changed".
 *
 * Line-based, with the usual three-part shape: trim the common prefix and
 * suffix, then run an LCS over what is left. The trim is what keeps this cheap
 * in the normal case, because the normal case is a large message with a small
 * change in it.
 *
 * BOUNDED ON PURPOSE. An LCS is O(n*m), and message text is caller-controlled,
 * so a pathological pair of long dissimilar messages could otherwise burn real
 * CPU inside a request. Past the cap the diff degrades to a summary line rather
 * than trying harder; the full text is still one format:"full" call away.
 */
constexpr size_t DIFF_MAX_LINES = 400;

std::vector<std::string> splitLines(const std::string &s)
{
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= s.size()) {
		const size_t nl = s.find('\n', start);
		if (nl == std::string::npos) {
			out.push_back(s.substr(start));
			break;
		}
		out.push_back(s.substr(start, nl - start));
		start = nl + 1;
	}
	return out;
}

std::string unifiedDiff(const std::string &oldText, const std::string &newText)
{
	if (oldText == newText)
		return "";

	std::vector<std::string> a = splitLines(oldText);
	std::vector<std::string> b = splitLines(newText);

	/* Common prefix / suffix: the parts a diff would only echo back. */
	size_t pre = 0;
	while (pre < a.size() && pre < b.size() && a[pre] == b[pre])
		pre++;
	size_t suf = 0;
	while (suf < a.size() - pre && suf < b.size() - pre &&
	       a[a.size() - 1 - suf] == b[b.size() - 1 - suf])
		suf++;

	const size_t an = a.size() - pre - suf;
	const size_t bn = b.size() - pre - suf;

	if (an > DIFF_MAX_LINES || bn > DIFF_MAX_LINES) {
		return "@@ too large to diff @@\n-" + std::to_string(an) +
		       " line(s) replaced by " + std::to_string(bn) +
		       " line(s); request format \"full\" for the text\n";
	}

	/* LCS over the differing middle only. */
	std::vector<std::vector<uint16_t>> dp(an + 1,
					      std::vector<uint16_t>(bn + 1, 0));
	for (size_t i = an; i-- > 0;) {
		for (size_t j = bn; j-- > 0;) {
			dp[i][j] = a[pre + i] == b[pre + j]
					   ? (uint16_t)(dp[i + 1][j + 1] + 1)
					   : std::max(dp[i + 1][j],
						      dp[i][j + 1]);
		}
	}

	std::string out = "@@ -" + std::to_string(pre + 1) + " +" +
			  std::to_string(pre + 1) + " @@\n";
	size_t i = 0, j = 0;
	while (i < an && j < bn) {
		if (a[pre + i] == b[pre + j]) {
			out += " " + a[pre + i] + "\n";
			i++;
			j++;
		} else if (dp[i + 1][j] >= dp[i][j + 1]) {
			out += "-" + a[pre + i] + "\n";
			i++;
		} else {
			out += "+" + b[pre + j] + "\n";
			j++;
		}
	}
	for (; i < an; i++)
		out += "-" + a[pre + i] + "\n";
	for (; j < bn; j++)
		out += "+" + b[pre + j] + "\n";
	return out;
}

/* ---- telegram_get_messages ---- */
/*
 * Every edit snapshot for a set of already-authorised message row ids, grouped
 * by message.
 *
 * One query for the whole batch rather than one per message: the edits table is
 * keyed by group_message_id, so an IN list of a hundred ids is a hundred index
 * lookups in a single round trip. This is the whole reason the tool exists --
 * the pattern it replaces was N separate telegram_get_message_history calls.
 */
std::unordered_map<int64_t, Json>
editsForRows(const drogon::orm::DbClientPtr &db,
	     const std::vector<int64_t> &rowIds)
{
	std::unordered_map<int64_t, Json> out;
	if (rowIds.empty())
		return out;

	std::string in;
	std::vector<std::string> binds;
	for (size_t i = 0; i < rowIds.size(); i++) {
		in += i ? ",?" : "?";
		binds.push_back(std::to_string(rowIds[i]));
	}
	const std::string sql =
		"SELECT /*+ MAX_EXECUTION_TIME(5000) */ "
		"e.group_message_id, e.content_type, e.text, e.edit_date, "
		"e.created_at, "
		"f.id AS file_id, f.file_type, f.file_size, f.on_disk "
		"FROM telegram_group_message_edits e "
		"LEFT JOIN telegram_files f ON f.id = e.file_id "
		"WHERE e.group_message_id IN (" + in + ") ORDER BY e.id ASC";

	for (const auto &e : execTool(db, sql, binds)) {
		const int64_t owner = colI64(e, "group_message_id");
		Json v;
		v["content_type"] = colStr(e, "content_type");
		v["text"] = colStr(e, "text");
		if (colI64(e, "edit_date"))
			v["edit_date"] = colI64(e, "edit_date");
		v["observed_at"] = colStr(e, "created_at");
		if (!e["file_id"].isNull())
			v["media"] = mediaJson(e, false);
		if (!out.count(owner))
			out[owner] = Json::array();
		out[owner].push_back(std::move(v));
	}
	return out;
}

Json run_telegram_get_messages(const drogon::orm::DbClientPtr &db,
			       const Json &args)
{
	const int64_t gid = requireInt(args, "group_id");

	if (!args.contains("message_ids") || !args["message_ids"].is_array())
		throw ToolError("message_ids is required and must be an array "
				"of integers");
	const Json &idsIn = args["message_ids"];
	if (idsIn.empty())
		throw ToolError("message_ids must not be empty");
	if (idsIn.size() > MAX_BATCH_IDS)
		throw ToolError("message_ids accepts at most " +
				std::to_string(MAX_BATCH_IDS) +
				" ids per call; split the batch");

	std::vector<int64_t> want;
	std::string in;
	std::vector<std::string> binds{ std::to_string(gid) };
	for (const auto &v : idsIn) {
		if (!v.is_number_integer())
			throw ToolError("message_ids must contain integers");
		const int64_t id = v.get<long long>();
		want.push_back(id);
		in += in.empty() ? "?" : ",?";
		binds.push_back(std::to_string(id));
	}

	const bool withHistory = args.contains("include_history") &&
				 args["include_history"].is_boolean() &&
				 args["include_history"].get<bool>();
	const Gate gate = msgGate(db, args);
	RowOpts o = rowOpts(args);
	/* The group is a call parameter here by definition, so compact always
	 * has an invariant to hoist. */
	o.hoistGroup = o.compact;

	Json out;
	out["group_id"] = gid;
	out["found"] = Json::array();
	out["missing_ids"] = Json::array();

	std::vector<int64_t> rowIds;
	std::unordered_map<int64_t, size_t> slotOf; /* row id -> index in found */
	std::unordered_set<int64_t> seen;

	if (!gate.matchesNothing) {
		std::vector<std::string> qb = binds;
		qb.insert(qb.end(), gate.binds.begin(), gate.binds.end());
		const std::string sql =
			std::string(MSG_SELECT) +
			"FROM telegram_group_messages m " + MSG_JOINS +
			"WHERE m.chat_id = ? AND m.message_id IN (" + in + ")" +
			gate.sql + " ORDER BY m.message_id ASC";

		for (const auto &r : execTool(db, sql, qb)) {
			const int64_t mid = colI64(r, "message_id");
			seen.insert(mid);
			Json row = messageRow(r, o);
			const int64_t rowId = colI64(r, "id_internal");
			if (withHistory) {
				rowIds.push_back(rowId);
				slotOf[rowId] = out["found"].size();
			}
			out["found"].push_back(std::move(row));
		}
	}

	/*
	 * An unknown id is reported, never fatal. A batch of a hundred ids
	 * where one has been purged should still return the ninety-nine --
	 * failing the call would make the caller bisect to find the bad id.
	 * A id in an unexposed group is "missing" for the same reason the
	 * other tools return nothing for one: saying otherwise would confirm
	 * it exists.
	 */
	for (int64_t id : want) {
		if (!seen.count(id))
			out["missing_ids"].push_back(id);
	}

	if (withHistory) {
		auto byOwner = editsForRows(db, rowIds);
		for (const auto &kv : slotOf) {
			Json &row = out["found"][kv.second];
			auto it = byOwner.find(kv.first);
			row["previous_versions"] =
				it == byOwner.end() ? Json::array() : it->second;
			row["previous_version_count"] =
				row["previous_versions"].size();
			/*
			 * Edited and RECOVERABLE are not the same: an edit seen
			 * before the daemon was watching leaves the flag and no
			 * snapshot. Say so on the row rather than let an empty
			 * list read as "nothing changed".
			 */
			const bool ed = row.contains("is_edited") &&
					row["is_edited"].get<bool>();
			if (ed && row["previous_versions"].empty())
				row["note"] =
					"marked edited, but no earlier version "
					"was captured -- the edit predates "
					"archiving or happened while the "
					"daemon was down";
		}
	}

	out["count"] = out["found"].size();
	out["missing_count"] = out["missing_ids"].size();
	return out;
}

void add_telegram_get_messages(gwmcp::ToolRegistry &registry,
			       const drogon::orm::DbClientPtr &db)
{
	gwmcp::Tool t;
	t.name = "telegram_get_messages";
	t.title = "Fetch many messages by id, optionally with their revisions";
	t.description =
		"Fetch up to " + std::to_string(MAX_BATCH_IDS) +
		" messages of one group by id, in a single call.\n\n"
		"USE THIS INSTEAD OF LOOPING. With include_history: true it "
		"also inlines every earlier version and the deletion record "
		"for each message, which is what telegram_get_message_history "
		"returns for ONE message -- so a set of fifty edited messages "
		"costs one call here rather than fifty there.\n\n"
		"Unknown ids never fail the batch: they come back in "
		"missing_ids while everything found comes back in found. An id "
		"in a group that is not readable is reported missing.\n\n"
		"Typical two-call workflow for \"what was edited this week, "
		"and what changed\":\n"
		"  1. telegram_search_messages with filter {\"field\":"
		"\"edit_date\",\"op\":\"between\",\"value\":[\"2026-01-01\","
		"\"2026-01-08\"]}, order_by edit_date, "
		"fields [\"message_id\",\"previous_version_count\"]\n"
		"  2. telegram_get_messages with those message_ids and "
		"include_history: true\n\n"
		"The size controls (fields, truncate_text, include_media, "
		"compact) work here too.";

	Json props;
	props["group_id"] = propGroupId();
	props["message_ids"] = Json{
		{ "type", "array" },
		{ "items", Json{ { "type", "integer" } } },
		{ "description",
		  "The message ids within that group; at most " +
			  std::to_string(MAX_BATCH_IDS) + "." }
	};
	props["include_history"] = Json{
		{ "type", "boolean" },
		{ "description",
		  "Default false. When true each message also carries "
		  "previous_versions (oldest first) and "
		  "previous_version_count -- collapsing N history calls into "
		  "this one." }
	};
	addRowShapeProps(props);
	t.inputSchema = Json{
		{ "type", "object" },
		{ "properties", props },
		{ "required", Json::array({ "group_id", "message_ids" }) },
	};
	t.handler = [db](const Json &args) {
		return run_telegram_get_messages(db, args);
	};
	registry.add(std::move(t));
}

/* ---- telegram_get_message_history ---- */
/*
 * The pre-edit snapshots of one message, oldest first, so the list reads as
 * the progression and ends at `current`.
 */
void msgHistVersions(const drogon::orm::DbClientPtr &db, Json &out,
		     int64_t rowId, bool asDiff, const std::string &currentText)
{
	struct Ver {
		Json meta;
		std::string text;
	};
	std::vector<Ver> vers;

	for (const auto &e : execTool(
		     db,
		     "SELECT e.content_type, e.text, e.edit_date, "
		     "e.created_at, "
		     "f.id AS file_id, f.file_type, f.file_size, f.on_disk "
		     "FROM telegram_group_message_edits e "
		     "LEFT JOIN telegram_files f ON f.id = e.file_id "
		     "WHERE e.group_message_id = ? ORDER BY e.id ASC",
		     { std::to_string(rowId) })) {
		Ver v;
		v.text = colStr(e, "text");
		v.meta["content_type"] = colStr(e, "content_type");
		if (colI64(e, "edit_date"))
			v.meta["edit_date"] = colI64(e, "edit_date");
		v.meta["observed_at"] = colStr(e, "created_at");
		if (!e["file_id"].isNull())
			v.meta["media"] = mediaJson(e, false);
		vers.push_back(std::move(v));
	}

	if (!asDiff) {
		out["previous_versions"] = Json::array();
		for (auto &v : vers) {
			Json j = std::move(v.meta);
			j["text"] = std::move(v.text);
			out["previous_versions"].push_back(std::move(j));
		}
		return;
	}

	/*
	 * Diff mode. One array, not two: each entry carries the change AND the
	 * metadata of the version it produced, so nothing is stated twice. A
	 * separate previous_versions list alongside the diffs would put the
	 * edit dates in the response twice and make the "smaller" format
	 * bigger than the full one for a short message.
	 */
	out["diffs"] = Json::array();
	for (size_t i = 0; i < vers.size(); i++) {
		const std::string &from = vers[i].text;
		const bool toSnapshot = i + 1 < vers.size();
		const std::string &to = toSnapshot ? vers[i + 1].text
						   : currentText;
		Json d;
		d["from_version"] = (int64_t)i;
		/* The last diff lands on the live message, not a snapshot. */
		d["to_version"] = toSnapshot ? Json((int64_t)(i + 1))
					     : Json("current");
		/* Metadata of the version this diff PRODUCED. */
		const Json &meta = toSnapshot ? vers[i + 1].meta : Json();
		if (toSnapshot) {
			if (meta.contains("edit_date"))
				d["edit_date"] = meta["edit_date"];
			d["observed_at"] = meta["observed_at"];
		}
		const std::string u = unifiedDiff(from, to);
		d["diff"] = u;
		if (u.empty())
			d["unchanged_text"] = true;
		out["diffs"].push_back(std::move(d));
	}
}

Json run_telegram_get_message_history(const drogon::orm::DbClientPtr &db, const Json &args)
{
	const int64_t gid = requireInt(args, "group_id");
	const int64_t mid = requireInt(args, "message_id");
	const Gate gate = msgGate(db, args);

	std::vector<std::string> binds{ std::to_string(gid),
					std::to_string(mid) };
	binds.insert(binds.end(), gate.binds.begin(), gate.binds.end());

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
		"WHERE m.chat_id = ? AND m.message_id = ?" + gate.sql,
		binds);
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

	std::string fmt = "full";
	if (args.contains("format") && args["format"].is_string()) {
		fmt = args["format"].get<std::string>();
		if (fmt != "full" && fmt != "diff")
			throw ToolError("\"format\" must be \"full\" or "
					"\"diff\"");
	}
	msgHistVersions(db, out, colI64(r, "id"), fmt == "diff",
			colStr(r, "text"));
	out["format"] = fmt;

	const size_t n = out.contains("previous_versions")
				 ? out["previous_versions"].size()
				 : (out.contains("diffs") ? out["diffs"].size()
							  : 0);
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
		"returned by the message tools. Only readable groups.\n\n"
		"FOR MORE THAN ONE MESSAGE, use telegram_get_messages with "
		"include_history: true -- it returns the same information for "
		"up to 100 messages in a single call. This tool is the "
		"single-message form.\n\n"
		"format: \"diff\" returns unified diffs between consecutive "
		"versions rather than the full text of each, which is much "
		"smaller when a long message got a small edit.";
	t.inputSchema = Json{
		{ "type", "object" },
		{ "properties",
		  Json{ { "group_id",
			  propGroupId() },
			{ "message_id",
			  Json{ { "type", "integer" },
				{ "description",
				  "The message's id within that "
				  "group." } } },
			{ "format",
			  Json{ { "type", "string" },
				{ "enum", Json::array({ "full", "diff" }) },
				{ "description",
				  "full (default) returns the complete text "
				  "of every version. diff returns a unified "
				  "diff between consecutive versions instead "
				  "-- far smaller when a long message was "
				  "edited slightly, and it shows what "
				  "actually changed." } } } } },
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
		"BATCH LOOKUP. To resolve many users at once, use the \"in\" "
		"operator on user_id -- it is the primary key, so up to 100 "
		"ids resolve in a single indexed call:\n"
		"{\"field\":\"user_id\",\"op\":\"in\",\"value\":"
		"[123,456,789]}\n"
		"Do not loop one call per user.\n\n"
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
	add_telegram_get_messages(registry, db);
	add_telegram_get_message_history(registry, db);
	add_telegram_get_user(registry, db);
	add_telegram_get_user_history(registry, db);
	add_telegram_get_users(registry, db);
}


} /* namespace tgweb::mcp::telegram */
