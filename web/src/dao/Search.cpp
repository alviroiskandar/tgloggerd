// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "dao/Search.hpp"

#include "views/Render.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
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

bool rowBool(const drogon::orm::Row &r, const char *col)
{
	return !r[col].isNull() && r[col].as<int>() != 0;
}

/* Bytes -> a short human string (e.g. "12.3 MB"); mirrors dao::browse. */
std::string humanSize(uint64_t bytes)
{
	static const char *unit[] = { "B", "KB", "MB", "GB", "TB" };
	double v = (double)bytes;
	int u = 0;
	while (v >= 1024.0 && u < 4) {
		v /= 1024.0;
		u++;
	}
	char buf[32];
	if (u == 0)
		snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
	else
		snprintf(buf, sizeof(buf), "%.1f %s", v, unit[u]);
	return buf;
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
	{ "msg_count",  "Messages",  FType::Int,      FKind::Column, "u.msg_count", "", "", "", INT_OPS,  true,  true,  "" },
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

/*
 * The displayed columns, in order. The mapRow below emits each row's values in
 * exactly this order. `type` drives cell rendering; `sortKey` names the
 * searchable field to ORDER BY (must be one of the sortable registry fields).
 */
const DisplayCol kUserCols[] = {
	/* key, label, type, sortKey */
	{ "photo",              "",                 "photo",    ""           },
	{ "id",                 "ID",               "id",       "id"         },
	{ "name",               "Name",             "name",     "first_name" },
	{ "username",           "Username",         "username", ""           },
	{ "type",               "Type",             "text",     "type"       },
	{ "msg_count",          "Messages",         "int",      "msg_count"  },
	{ "is_verified",        "Verified",         "bool",     ""           },
	{ "is_premium",         "Premium",          "bool",     ""           },
	{ "is_scam",            "Scam",             "bool",     ""           },
	{ "is_fake",            "Fake",             "bool",     ""           },
	{ "is_support",         "Support",          "bool",     ""           },
	{ "phone",              "Phone",            "text",     ""           },
	{ "language",           "Language",         "text",     ""           },
	{ "restriction_reason", "Restriction",      "longtext", ""           },
	{ "has_sensitive",      "Sensitive",        "bool",     ""           },
	{ "restricts_new_chats","Restricts chats",  "bool",     ""           },
	{ "paid_star_count",    "Paid stars",       "int",      "paid_star_count" },
	{ "personal_chat_id",   "Personal chat",    "int",      ""           },
	{ "emoji_status",       "Emoji status",     "int",      ""           },
	{ "bio",                "Bio",              "longtext", ""           },
	{ "created_at",         "Created",          "datetime", "created_at" },
	{ "updated_at",         "Updated",          "datetime", "updated_at" },
};

/* Positional row aligned to kUserCols. Photo cell is the raw file id (0 = none);
 * the controller tokenises it to a /files/<token> URL. */
nlohmann::json mapRowUser(const drogon::orm::Row &r)
{
	auto strOrEmpty = [&](const char *c) -> std::string {
		return r[c].isNull() ? std::string() : r[c].as<std::string>();
	};

	nlohmann::json a = nlohmann::json::array();
	a.push_back(r["profile_photo_file_id"].isNull()
			    ? (int64_t)0 : r["profile_photo_file_id"].as<int64_t>());
	a.push_back(r["id"].as<int64_t>());
	a.push_back(displayName(r));
	a.push_back(escCol(r, "username"));
	a.push_back(r["type"].as<std::string>());
	a.push_back(r["msg_count"].as<std::string>());
	a.push_back(rowBool(r, "is_verified"));
	a.push_back(rowBool(r, "is_premium"));
	a.push_back(rowBool(r, "is_scam"));
	a.push_back(rowBool(r, "is_fake"));
	a.push_back(rowBool(r, "is_support"));
	a.push_back(escCol(r, "phone"));
	a.push_back(escCol(r, "language"));
	a.push_back(escCol(r, "restriction_reason"));
	a.push_back(rowBool(r, "has_sensitive"));
	a.push_back(rowBool(r, "restricts_new_chats"));
	a.push_back(strOrEmpty("paid_star_count"));
	a.push_back(strOrEmpty("personal_chat_id"));
	a.push_back(strOrEmpty("emoji_status"));
	a.push_back(escCol(r, "bio"));
	a.push_back(r["created_at"].as<std::string>());
	a.push_back(r["updated_at"].as<std::string>());
	return a;
}

const SearchSchema kUsersSchema = {
	/* fromJoin   */ "users u LEFT JOIN user_extra_info e ON e.user_id = u.id",
	/* selectCols */ "u.id, u.first_name, u.last_name, u.type, u.is_verified, "
			 "u.is_premium, u.is_scam, u.is_fake, u.is_support, "
			 "u.profile_photo_file_id, u.created_at, u.updated_at, "
			 "u.msg_count, "
			 "(SELECT un.username FROM user_usernames un "
			 "WHERE un.user_id = u.id AND un.kind='active' "
			 "ORDER BY un.position LIMIT 1) AS username, "
			 "COALESCE(e.phone_number,'') AS phone, "
			 "COALESCE(e.language_code,'') AS language, "
			 "COALESCE(e.restriction_reason,'') AS restriction_reason, "
			 "COALESCE(e.has_sensitive_content,0) AS has_sensitive, "
			 "COALESCE(e.restricts_new_chats,0) AS restricts_new_chats, "
			 "COALESCE(e.paid_message_star_count,0) AS paid_star_count, "
			 "e.personal_chat_id AS personal_chat_id, "
			 "e.emoji_status_custom_emoji_id AS emoji_status, "
			 "COALESCE(e.bio,'') AS bio",
	/* idCol      */ "u.id",
	/* exFk       */ "user_id",
	/* defaultSort*/ "u.id",
	/* defaultOrder*/ "DESC",
	/* fields     */ kUserFields,
	/* nFields    */ sizeof(kUserFields) / sizeof(kUserFields[0]),
	/* cols       */ kUserCols,
	/* nCols      */ sizeof(kUserCols) / sizeof(kUserCols[0]),
	/* mapRow     */ &mapRowUser,
};

/* --- groups registry ------------------------------------------------------ */
/* `groups` is a reserved word (backticked); group ids are negative Telegram
 * chat ids; there is no group_extra table (description lives on groups). */

const SearchField kGroupFields[] = {
	{ "id",          "Group ID",   FType::Int,      FKind::Column, "g.id",          "", "", "", INT_OPS,  true,  true,  "" },
	{ "title",       "Title",      FType::Text,     FKind::Column, "g.title",       "", "", "", TEXT_OPS, true,  true,  "" },
	{ "description", "Description", FType::Text,     FKind::Column, "g.description", "", "", "", TEXT_OPS, false, true,  "" },
	{ "type",        "Type",       FType::Enum,     FKind::Column, "g.type",        "", "", "", ENUM_OPS, true,  true,  "basic_group,supergroup,channel" },
	{ "msg_count",   "Messages",   FType::Int,      FKind::Column, "g.msg_count",   "", "", "", INT_OPS,  true,  true,  "" },
	{ "has_photo",   "Has photo",  FType::Bool,     FKind::Column, "g.photo_file_id","", "", "", NULL_OPS, false, false, "" },
	{ "created_at",  "Created",    FType::Datetime, FKind::Column, "g.created_at",  "", "", "", DT_OPS,   true,  true,  "" },
	{ "updated_at",  "Updated",    FType::Datetime, FKind::Column, "g.updated_at",  "", "", "", DT_OPS,   true,  true,  "" },
	{ "username",    "Username (current)", FType::Text, FKind::Exists, "", "group_usernames x", "x.username", "AND x.kind='active'", EXISTS_OPS, false, true, "" },
	{ "hist_title",       "Title (ever)",       FType::Text, FKind::Exists, "", "group_hist_title x",            "x.title",       "", EXISTS_POS, false, false, "" },
	{ "hist_description", "Description (ever)",  FType::Text, FKind::Exists, "", "group_hist_description x",      "x.description", "", EXISTS_POS, false, false, "" },
	{ "hist_username",    "Username (ever)",     FType::Text, FKind::Exists, "", "group_hist_usernames_events x", "x.username",    "", EXISTS_OPS, false, false, "" },
};

const DisplayCol kGroupCols[] = {
	{ "photo",       "",            "photo",    ""           },
	{ "id",          "ID",          "id",       "id"         },
	{ "title",       "Title",       "name",     "title"      },
	{ "username",    "Username",     "username", ""           },
	{ "type",        "Type",        "text",     "type"       },
	{ "msg_count",   "Messages",    "int",      "msg_count"  },
	{ "description", "Description",  "longtext", ""           },
	{ "admin_count", "Admins",      "int",      ""           },
	{ "created_at",  "Created",     "datetime", "created_at" },
	{ "updated_at",  "Updated",     "datetime", "updated_at" },
};

nlohmann::json mapRowGroup(const drogon::orm::Row &r)
{
	nlohmann::json a = nlohmann::json::array();
	a.push_back(r["photo_file_id"].isNull()
			    ? (int64_t)0 : r["photo_file_id"].as<int64_t>());
	a.push_back(r["id"].as<int64_t>());
	std::string title = escCol(r, "title");
	a.push_back(title.empty() ? std::string("(untitled)") : title);
	a.push_back(escCol(r, "username"));
	a.push_back(r["type"].as<std::string>());
	a.push_back(r["msg_count"].as<std::string>());
	a.push_back(escCol(r, "description"));
	a.push_back(r["admin_count"].isNull() ? std::string("0")
					      : r["admin_count"].as<std::string>());
	a.push_back(r["created_at"].as<std::string>());
	a.push_back(r["updated_at"].as<std::string>());
	return a;
}

const SearchSchema kGroupsSchema = {
	/* fromJoin   */ "`groups` g",
	/* selectCols */ "g.id, g.title, g.description, g.type, g.photo_file_id, "
			 "g.created_at, g.updated_at, g.msg_count, "
			 "(SELECT gu.username FROM group_usernames gu "
			 "WHERE gu.group_id = g.id AND gu.kind='active' "
			 "ORDER BY gu.position LIMIT 1) AS username, "
			 "(SELECT COUNT(*) FROM group_admins ga "
			 "WHERE ga.group_id = g.id) AS admin_count",
	/* idCol      */ "g.id",
	/* exFk       */ "group_id",
	/* defaultSort*/ "g.id",
	/* defaultOrder*/ "DESC",
	/* fields     */ kGroupFields,
	/* nFields    */ sizeof(kGroupFields) / sizeof(kGroupFields[0]),
	/* cols       */ kGroupCols,
	/* nCols      */ sizeof(kGroupCols) / sizeof(kGroupCols[0]),
	/* mapRow     */ &mapRowGroup,
};

/* --- files registry ------------------------------------------------------- */
/* The content-addressed file store. Files have no profile page: the id and
 * thumbnail cells link to the tokenised media download instead (detail_base is
 * empty, see FilesController), and the thumbnail is rendered by the files-only
 * "filethumb" cell type. */

const SearchField kFileFields[] = {
	{ "id",         "File ID",   FType::Int,      FKind::Column, "f.id",             "", "", "", INT_OPS,  true,  true,  "" },
	{ "file_type",  "Type",      FType::Enum,     FKind::Column, "f.file_type",      "", "", "", ENUM_OPS, true,  true,  "photo,video,document,audio,voice,sticker,animation,unknown" },
	{ "name",       "Name",      FType::Text,     FKind::Column, "f.orig_file_name", "", "", "", TEXT_OPS, true,  true,  "" },
	{ "ext",        "Extension", FType::Text,     FKind::Column, "COALESCE(f.file_ext,'')", "", "", "", TEXT_OPS, true, true, "" },
	{ "size",       "Size",      FType::Int,      FKind::Column, "f.file_size",      "", "", "", INT_OPS,  true,  true,  "" },
	{ "hits",       "Hits",      FType::Int,      FKind::Column, "f.hit_count",      "", "", "", INT_OPS,  true,  true,  "" },
	{ "stored",     "Stored",    FType::Bool,     FKind::Column, "f.on_disk",        "", "", "", BOOL_OPS, false, true,  "" },
	{ "tg_file_id", "TG file id",FType::Text,     FKind::Column, "f.tg_file_id",     "", "", "", TEXT_OPS, false, false, "" },
	{ "created_at", "Created",   FType::Datetime, FKind::Column, "f.created_at",     "", "", "", DT_OPS,   true,  true,  "" },
	{ "updated_at", "Updated",   FType::Datetime, FKind::Column, "f.updated_at",     "", "", "", DT_OPS,   true,  false, "" },
};

const DisplayCol kFileCols[] = {
	{ "thumb",      "",          "filethumb", ""          },
	{ "id",         "ID",        "id",        "id"        },
	{ "file_type",  "Type",      "text",      "file_type" },
	{ "name",       "Name",      "text",      "name"      },
	{ "ext",        "Ext",       "text",      "ext"       },
	{ "size",       "Size",      "text",      "size"      },
	{ "hits",       "Hits",      "int",       "hits"      },
	{ "stored",     "Stored",    "bool",      ""          },
	{ "created_at", "First seen","datetime",  "created_at"},
};

/* Positional row aligned to kFileCols. The thumb cell is the raw file id (0
 * never occurs for a real file); the controller tokenises it to /files/<token>,
 * which serves as both the thumbnail src and the row's download link. */
nlohmann::json mapRowFile(const drogon::orm::Row &r)
{
	nlohmann::json a = nlohmann::json::array();
	a.push_back(r["id"].as<int64_t>());          /* thumb: raw id -> token */
	a.push_back(r["id"].as<int64_t>());
	a.push_back(r["file_type"].as<std::string>());
	a.push_back(escCol(r, "orig_file_name"));
	a.push_back(escCol(r, "file_ext"));
	a.push_back(humanSize(r["file_size"].as<uint64_t>()));
	a.push_back(r["hit_count"].as<std::string>());
	a.push_back(rowBool(r, "on_disk"));
	a.push_back(escCol(r, "created_at"));
	return a;
}

const SearchSchema kFilesSchema = {
	/* fromJoin   */ "files f",
	/* selectCols */ "f.id, f.file_type, f.file_ext, f.orig_file_name, "
			 "f.file_size, f.hit_count, f.on_disk, f.created_at",
	/* idCol      */ "f.id",
	/* exFk       */ "",              /* no EXISTS fields for files */
	/* defaultSort*/ "f.id",
	/* defaultOrder*/ "DESC",
	/* fields     */ kFileFields,
	/* nFields    */ sizeof(kFileFields) / sizeof(kFileFields[0]),
	/* cols       */ kFileCols,
	/* nCols      */ sizeof(kFileCols) / sizeof(kFileCols[0]),
	/* mapRow     */ &mapRowFile,
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

nlohmann::json colsJson(const SearchSchema &s)
{
	nlohmann::json arr = nlohmann::json::array();
	for (size_t i = 0; i < s.nCols; i++) {
		nlohmann::json o;
		o["key"]   = std::string(s.cols[i].key);
		o["label"] = std::string(s.cols[i].label);
		o["type"]  = std::string(s.cols[i].type);
		o["sort"]  = std::string(s.cols[i].sortKey);
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

			/* LIKE binds the value verbatim -- no implicit "%q%"
			 * wrapping -- so a plain term matches literally and the
			 * user opts into wildcards by typing % or _ themselves. */
			bool likeish = (ot->op == OP_LIKE || ot->op == OP_NLIKE);
			std::string val = c.v;

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

const SearchSchema &groupsSchema(void)
{
	return kGroupsSchema;
}

const SearchSchema &filesSchema(void)
{
	return kFilesSchema;
}

const SearchSchema *schemaByName(const std::string &entity)
{
	if (entity == "users")
		return &kUsersSchema;
	if (entity == "groups")
		return &kGroupsSchema;
	if (entity == "files")
		return &kFilesSchema;
	return nullptr;
}

/* Longest raw `search` JSON we will even attempt to parse. */
static constexpr size_t kMaxSearchBytes = 8192;

bool parseConditions(const std::string &raw, std::vector<Condition> &out,
		     std::string &err)
{
	if (raw.empty())
		return true;
	if (raw.size() > kMaxSearchBytes) {
		err = "search parameter too large";
		return false;
	}

	nlohmann::json j;
	try {
		j = nlohmann::json::parse(raw);
	} catch (const std::exception &) {
		err = "search must be valid JSON";
		return false;
	}
	if (!j.is_array()) {
		err = "search must be a JSON array";
		return false;
	}

	for (const auto &item : j) {
		if (!item.is_object()) {
			err = "each condition must be a JSON object";
			return false;
		}
		Condition c;
		auto strField = [&](const char *k, std::string &dst,
				    bool required) -> bool {
			if (!item.contains(k) || item[k].is_null()) {
				if (required) {
					err = std::string("missing '") + k +
					      "' in a condition";
					return false;
				}
				return true;
			}
			if (!item[k].is_string()) {
				err = std::string("'") + k + "' must be a string";
				return false;
			}
			dst = item[k].get<std::string>();
			return true;
		};
		if (!strField("c", c.c, true) || !strField("o", c.o, true) ||
		    !strField("n", c.n, false))
			return false;
		if (item.contains("v") && !item["v"].is_null()) {
			if (!item["v"].is_string()) {
				err = "'v' must be a string";
				return false;
			}
			c.v = item["v"].get<std::string>();
			c.hasV = true;
		}
		out.push_back(std::move(c));
	}
	return true;
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
	out["cols"]       = colsJson(schema);
	out["limit"]      = limit;
	out["offset"]     = offset;
	out["max_offset"] = MAX_OFFSET; /* deepest reachable offset (pager cap) */
	out["sort"]       = usedSort;
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
		/* Every debug string is Render::esc()'d (the SQL contains '<'/'>'
		 * comparison operators, the binds are user input), so the SSR
		 * template and the JS renderer both insert them verbatim. */
		dbg["sql"]       = Render::esc(pageSql);
		dbg["count_sql"] = Render::esc(countSql);
		nlohmann::json b = nlohmann::json::array();
		for (const auto &v : binds)
			b.push_back(Render::esc(v));
		dbg["bind"] = std::move(b);

		/* EXPLAIN as {columns, rows[]} so the UI renders it as a table.
		 * FORMAT=TRADITIONAL forces the classic multi-column plan (MySQL
		 * defaults to the single-column tree format here). */
		std::string explainSql = "EXPLAIN FORMAT=TRADITIONAL " + pageSql;
		auto eres = co_await db->execSqlCoro(explainSql,
						     std::as_const(binds));
		nlohmann::json exCols = nlohmann::json::array();
		for (drogon::orm::Result::SizeType ci = 0; ci < eres.columns(); ci++)
			exCols.push_back(Render::esc(eres.columnName(ci)));
		nlohmann::json exRows = nlohmann::json::array();
		for (const auto &r : eres) {
			nlohmann::json row = nlohmann::json::array();
			for (drogon::orm::Result::SizeType ci = 0;
			     ci < eres.columns(); ci++) {
				const char *name = eres.columnName(ci);
				row.push_back(r[name].isNull()
					? std::string()
					: Render::esc(r[name].as<std::string>()));
			}
			exRows.push_back(std::move(row));
		}
		dbg["explain"] = { { "columns", std::move(exCols) },
				   { "rows", std::move(exRows) } };
		out["debug"] = std::move(dbg);
	}

	co_return out;
}

} /* namespace tgweb::dao::search */
