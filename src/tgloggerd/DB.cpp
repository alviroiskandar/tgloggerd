// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <tgloggerd/DB.hpp>

#include <string>
#include <cstdint>
#include <stdexcept>

namespace tgloggerd {

DB::DB(const mysql::Config &cfg)
	: db_(cfg)
{
}

DB::~DB(void) = default;

void DB::ping(void)
{
	db_.query("SELECT 1");
}

std::vector<DiscordWebhook> DB::loadDiscordWebhooks(void)
{
	auto rows = db_.query(
		"SELECT telegram_chat_id, webhook_url, fwd_sticker, fwd_text, "
		"fwd_photo, fwd_video, fwd_document, fwd_other, fwd_animation, "
		"fwd_video_note, fwd_voice, fwd_audio, fwd_poll, fwd_checklist, "
		"fwd_dice, fwd_game, fwd_location, fwd_contact, fwd_story, "
		"fwd_paid_media, fwd_invoice, fwd_service "
		"FROM telegram_discord_webhooks WHERE enabled = 1");

	/* Column order of the fwd_* flags above. */
	static const unsigned bits[] = {
		FWD_STICKER, FWD_TEXT, FWD_PHOTO, FWD_VIDEO, FWD_DOCUMENT,
		FWD_OTHER, FWD_ANIMATION, FWD_VIDEO_NOTE, FWD_VOICE, FWD_AUDIO,
		FWD_POLL, FWD_CHECKLIST, FWD_DICE, FWD_GAME, FWD_LOCATION,
		FWD_CONTACT, FWD_STORY, FWD_PAID_MEDIA, FWD_INVOICE, FWD_SERVICE,
	};

	std::vector<DiscordWebhook> out;
	out.reserve(rows.size());
	for (const auto &r : rows) {
		if (!r[0].has_value() || !r[1].has_value())
			continue;

		unsigned types = 0;
		for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
			const auto &f = r[2 + i];
			if (f.has_value() && *f != "0")
				types |= bits[i];
		}
		out.push_back({ std::stoll(*r[0]), *r[1], types });
	}
	return out;
}

std::vector<std::pair<int64_t, int64_t>> DB::listMediaMessagesWithoutFile(void)
{
	static const char *media =
		"('photo','video','document','audio','voice','sticker','animation')";
	auto rows = db_.query(
		std::string("SELECT chat_id, message_id FROM telegram_group_messages"
			    " WHERE file_id IS NULL AND deleted_at IS NULL"
			    " AND content_type IN ") + media +
		" UNION ALL SELECT chat_id, message_id FROM telegram_private_messages"
		" WHERE file_id IS NULL AND deleted_at IS NULL"
		" AND content_type IN " + media);

	std::vector<std::pair<int64_t, int64_t>> out;
	out.reserve(rows.size());
	for (const auto &r : rows) {
		if (!r[0].has_value() || !r[1].has_value())
			continue;
		out.emplace_back(std::stoll(*r[0]), std::stoll(*r[1]));
	}
	return out;
}

/*
 * The repair selection (listMessagesToRepair) of one message table: rows
 * without their kind or detail, and rich rows stored before their mapping,
 * which have a tdlib_type but no curated "blocks" key (decision
 * 2026-11-19-msg-rich-reread).
 */
static std::string repair_where(void)
{
	return " WHERE deleted_at IS NULL AND ((tdlib_type IS NULL"
	       " AND content_type IN ('unknown','service'))"
	       " OR (content_type = 'rich'"
	       " AND JSON_EXTRACT(content_extra, '$.blocks') IS NULL))";
}

std::vector<std::pair<int64_t, int64_t>> DB::listMessagesToRepair(void)
{
	auto rows = db_.query(
		"SELECT chat_id, message_id FROM telegram_group_messages" +
		repair_where() +
		" UNION ALL SELECT chat_id, message_id"
		" FROM telegram_private_messages" + repair_where());

	std::vector<std::pair<int64_t, int64_t>> out;
	out.reserve(rows.size());
	for (const auto &r : rows) {
		if (!r[0].has_value() || !r[1].has_value())
			continue;
		out.emplace_back(std::stoll(*r[0]), std::stoll(*r[1]));
	}
	return out;
}

static const char *message_table(bool is_group)
{
	return is_group ? "telegram_group_messages" : "telegram_private_messages";
}

/*
 * The recheck's reads name the (chat_id, message_id) unique key: left to
 * itself the optimizer can take the deleted_at key (a ref on NULL, which
 * nearly every row is) or the (chat_id, media_album_id) key and scan the
 * whole chat.
 */
static std::string recheck_from(bool is_group)
{
	return is_group
		? " FROM telegram_group_messages FORCE INDEX (uq_group_messages_chat_msg)"
		: " FROM telegram_private_messages FORCE INDEX (uq_private_messages_chat_msg)";
}

recheck::Stored DB::listRecheckStored(int64_t chat_id, bool is_group,
				      const recheck::Span &span)
{
	const std::string t = recheck_from(is_group);
	recheck::Stored s;
	auto rows = db_.query(
		"SELECT message_id" + t +
		" WHERE chat_id = ? AND message_id > ? AND message_id < ?"
		" AND deleted_at IS NULL",
		{ chat_id, span.lo, span.hi });
	s.between.reserve(rows.size());
	for (const auto &r : rows)
		if (r[0].has_value())
			s.between.push_back(std::stoll(*r[0]));
	s.older_exists = !db_.query(
		"SELECT 1" + t + " WHERE chat_id = ? AND message_id < ?"
		" AND deleted_at IS NULL LIMIT 1", { chat_id, span.lo }).empty();
	return s;
}

std::vector<int64_t> DB::markDeletedOnRecheck(
	int64_t chat_id, bool is_group, const std::vector<int64_t> &message_ids)
{
	/*
	 * The flag tells a deletion concluded from a recheck apart from one
	 * Telegram reported. JSON_SET on a NULL document is NULL, hence the
	 * empty object; CAST makes it JSON true, not the integer 1.
	 */
	const std::string sql = std::string("UPDATE ") + message_table(is_group) +
		" SET deleted_at = NOW(),"
		" meta = JSON_SET(IF(JSON_TYPE(meta) = 'OBJECT', meta,"
		" JSON_OBJECT()), '$.deleted_on_recheck', CAST('true' AS JSON))"
		" WHERE chat_id = ? AND message_id = ? AND deleted_at IS NULL";
	std::vector<int64_t> marked;
	if (message_ids.empty())
		return marked;
	db_.transaction([&](mysql::Transaction &tx) {
		for (int64_t id : message_ids)
			if (tx.execute(sql, { chat_id, id }) > 0)
				marked.push_back(id);
	});
	return marked;
}

std::unordered_map<int64_t, int64_t> DB::newestStoredMessageIds(
	const std::vector<int64_t> &chat_ids)
{
	std::unordered_map<int64_t, int64_t> out;
	for (int64_t c : chat_ids) {
		/* MAX over the unique key's prefix: one index lookup. */
		auto rows = db_.query(c < 0
			? "SELECT MAX(message_id) FROM telegram_group_messages"
			  " FORCE INDEX (uq_group_messages_chat_msg) WHERE chat_id = ?"
			: "SELECT MAX(message_id) FROM telegram_private_messages"
			  " FORCE INDEX (uq_private_messages_chat_msg) WHERE chat_id = ?",
			{ c });
		if (!rows.empty() && rows[0][0].has_value())
			out[c] = std::stoll(*rows[0][0]);
	}
	return out;
}

size_t DB::countStoredMessages(int64_t chat_id, bool is_group,
			       const std::vector<int64_t> &message_ids)
{
	if (message_ids.empty())
		return 0;
	std::string sql = std::string("SELECT COUNT(*) FROM ") +
		message_table(is_group) + (is_group
			? " FORCE INDEX (uq_group_messages_chat_msg)"
			: " FORCE INDEX (uq_private_messages_chat_msg)") +
		" WHERE chat_id = ? AND message_id IN (";
	/* The ids are integers, so they go into the statement as literals;
	 * binding each as a parameter copied a vector of variants, which
	 * GCC 13 wrongly warns may be uninitialized. */
	for (size_t i = 0; i < message_ids.size(); i++) {
		if (i)
			sql += ", ";
		sql += std::to_string(message_ids[i]);
	}
	sql += ")";
	auto rows = db_.query(sql, { chat_id });
	if (rows.empty() || !rows[0][0].has_value())
		return 0;
	return (size_t)std::stoull(*rows[0][0]);
}

std::unordered_map<int64_t, int64_t> DB::storedNearNewest(
	const std::unordered_map<int64_t, int64_t> &newest)
{
	std::unordered_map<int64_t, int64_t> out;
	for (const auto &[chat, max] : newest) {
		if (!recheck::isSupergroupId(chat) || max <= 0)
			continue;
		/* A range on the unique key's prefix. */
		auto rows = db_.query(
			"SELECT COUNT(*) FROM telegram_group_messages"
			" FORCE INDEX (uq_group_messages_chat_msg)"
			" WHERE chat_id = ? AND message_id > ? AND message_id <= ?",
			{ chat, max - recheck::kDenseWindow, max });
		if (!rows.empty() && rows[0][0].has_value())
			out[chat] = std::stoll(*rows[0][0]);
	}
	return out;
}

std::vector<int64_t> DB::restoreRecheckDeleted(
	int64_t chat_id, bool is_group, const std::vector<int64_t> &message_ids)
{
	const std::string sql = std::string("UPDATE ") + message_table(is_group) +
		" SET deleted_at = NULL,"
		" meta = JSON_REMOVE(meta, '$.deleted_on_recheck')"
		" WHERE chat_id = ? AND message_id = ? AND deleted_at IS NOT NULL"
		" AND JSON_EXTRACT(meta, '$.deleted_on_recheck') ="
		" CAST('true' AS JSON)";
	std::vector<int64_t> restored;
	if (message_ids.empty())
		return restored;
	db_.transaction([&](mysql::Transaction &tx) {
		for (int64_t id : message_ids)
			if (tx.execute(sql, { chat_id, id }) > 0)
				restored.push_back(id);
	});
	return restored;
}

bool DB::chatKnown(int64_t chat_id)
{
	return !db_.query(chat_id > 0
		? "SELECT 1 FROM telegram_users WHERE id = ?"
		: "SELECT 1 FROM `telegram_groups` WHERE id = ?",
		{ chat_id }).empty();
}

/*
 * The thumbnail selection (listMessagesWithoutThumbnail) of one message
 * table @t, whose rows telegram_message_files points at through @key:
 * FROM and WHERE, the table aliased m. The rows come off the
 * (content_type, date) index, forced so that a table where these kinds
 * are common is not scanned whole; the files row is read by its primary
 * key and only for a sticker, and the message files by their unique key
 * (key, role, position). A sticker is a video sticker when its stored
 * file's extension is webm (TDLib names a downloaded one so) or, for one
 * stored without its file, when its curated detail says format webm.
 */
static std::string thumbnail_from(const char *t, const char *key)
{
	const std::string tt = t;
	return " FROM " + tt + " m FORCE INDEX (idx_" +
	       tt.substr(sizeof("telegram_") - 1) + "_ctype_date)"
	       " LEFT JOIN telegram_files f"
	       " ON f.id = m.file_id AND m.content_type = 'sticker'"
	       " WHERE m.content_type IN"
	       " ('video','animation','video_note','sticker')"
	       " AND m.deleted_at IS NULL"
	       " AND (m.content_type <> 'sticker' OR f.file_ext = 'webm'"
	       " OR JSON_UNQUOTE(JSON_EXTRACT(m.content_extra, '$.format'))"
	       " = 'webm')"
	       " AND NOT EXISTS (SELECT 1 FROM telegram_message_files x"
	       " WHERE x." + std::string(key) + " = m.id"
	       " AND x.role IN ('thumbnail','cover'))";
}

std::string DB::thumbnailCandidatesSql(void)
{
	return "SELECT m.chat_id, m.message_id" +
	       thumbnail_from("telegram_group_messages", "group_message_id") +
	       " UNION ALL SELECT m.chat_id, m.message_id" +
	       thumbnail_from("telegram_private_messages",
			      "private_message_id");
}

std::vector<std::pair<int64_t, int64_t>> DB::listMessagesWithoutThumbnail(void)
{
	auto rows = db_.query(thumbnailCandidatesSql());

	std::vector<std::pair<int64_t, int64_t>> out;
	out.reserve(rows.size());
	for (const auto &r : rows) {
		if (!r[0].has_value() || !r[1].has_value())
			continue;
		out.emplace_back(std::stoll(*r[0]), std::stoll(*r[1]));
	}
	return out;
}

std::vector<std::tuple<std::string, std::string, uint64_t>>
DB::countMessagesWithoutThumbnail(void)
{
	auto rows = db_.query(
		"SELECT 'group', m.content_type, COUNT(*)" +
		thumbnail_from("telegram_group_messages", "group_message_id") +
		" GROUP BY m.content_type"
		" UNION ALL SELECT 'private', m.content_type, COUNT(*)" +
		thumbnail_from("telegram_private_messages",
			       "private_message_id") +
		" GROUP BY m.content_type");

	std::vector<std::tuple<std::string, std::string, uint64_t>> out;
	for (const auto &r : rows)
		if (r[0].has_value() && r[1].has_value() && r[2].has_value())
			out.emplace_back(*r[0], *r[1], std::stoull(*r[2]));
	return out;
}

std::vector<std::tuple<std::string, std::string, uint64_t>>
DB::countMessagesToRepair(void)
{
	auto rows = db_.query(
		"SELECT 'group', content_type, COUNT(*)"
		" FROM telegram_group_messages" + repair_where() +
		" GROUP BY content_type"
		" UNION ALL SELECT 'private', content_type, COUNT(*)"
		" FROM telegram_private_messages" + repair_where() +
		" GROUP BY content_type");

	std::vector<std::tuple<std::string, std::string, uint64_t>> out;
	for (const auto &r : rows)
		if (r[0].has_value() && r[1].has_value() && r[2].has_value())
			out.emplace_back(*r[0], *r[1], std::stoull(*r[2]));
	return out;
}

std::vector<int64_t> DB::loadForwardingBotUserIds(void)
{
	/*
	 * bot_user_id is 0 until discordd logs the bot in for the first time,
	 * so skip those rather than treating 0 as a sender id.
	 */
	auto rows = db_.query("SELECT bot_user_id FROM telegram_bots "
			      "WHERE bot_user_id <> 0");

	std::vector<int64_t> out;
	out.reserve(rows.size());
	for (const auto &r : rows) {
		if (!r[0].has_value())
			continue;
		out.push_back(std::stoll(*r[0]));
	}
	return out;
}

std::map<uint64_t, std::string> DB::loadBotSignatures(void)
{
	static const char *COLS =
		"id, CONCAT_WS('|', name, enabled, status, "
		"COALESCE(status_detail, ''), COALESCE(status_at, ''), "
		"bot_user_id, username, updated_at)";
	auto rows = db_.query(std::string("SELECT ") + COLS +
			      " FROM telegram_bots UNION ALL SELECT " + COLS +
			      " FROM discord_bots");
	std::map<uint64_t, std::string> out;
	for (const auto &r : rows) {
		if (!r[0].has_value())
			continue;
		out[std::stoull(*r[0])] = r[1].value_or("");
	}
	return out;
}

std::optional<SenderUser> DB::getSenderUser(int64_t user_id)
{
	auto rows = db_.query(
		"SELECT profile_photo_file_id, first_name, last_name,"
		" is_contact, own_name_known FROM telegram_users WHERE id = ?",
		{ user_id });
	if (rows.empty())
		return std::nullopt;
	const auto &r = rows[0];
	SenderUser u;
	if (r[0].has_value())
		u.photo_file_id = (uint64_t)std::stoull(*r[0]);
	u.name.name = { r[1].value_or(""), r[2].value_or("") };
	if (r[3].has_value())
		u.name.is_contact = *r[3] != "0";
	u.name.own_name_known = r[4].value_or("1") != "0";
	return u;
}

ChatPhoto DB::getGroupPhoto(int64_t chat_id)
{
	ChatPhoto out;
	auto rows = db_.query(
		"SELECT photo_file_id, title FROM `telegram_groups` WHERE id = ?",
		{ chat_id });
	if (rows.empty())
		return out;
	if (rows[0][0].has_value())
		out.photo_file_id = (uint64_t)std::stoull(*rows[0][0]);
	out.title = rows[0][1].value_or("");
	return out;
}

/*
 * The owning message, as a subquery, so the preview and its owner are resolved
 * in one statement. Group and private messages live in separate tables (and
 * separate columns here), which is the only thing that differs between the two.
 */
namespace {

/*
 * Truncate to at most @max_chars codepoints, never splitting one. The
 * link-preview VARCHAR columns are utf8mb4 and measured in CHARACTERS, and
 * under strict sql_mode an oversized bind rejects the whole INSERT rather
 * than truncating -- so the clamp has to happen here, and by codepoint, or a
 * multibyte value cut at a byte boundary would be rejected as malformed.
 */
std::string utf8_truncate_chars(const std::string &s, size_t max_chars)
{
	size_t chars = 0, i = 0;
	while (i < s.size() && chars < max_chars) {
		unsigned char b = (unsigned char)s[i];
		size_t len = b < 0x80 ? 1 :
			     (b >> 5) == 0x06 ? 2 :
			     (b >> 4) == 0x0E ? 3 :
			     (b >> 3) == 0x1E ? 4 : 1;
		if (i + len > s.size())
			break;
		i += len;
		chars++;
	}
	if (i >= s.size())
		return s;
	return s.substr(0, i);
}

} /* namespace */

void DB::upsertLinkPreview(const models::LinkPreview &lp, bool is_group)
{
	const char *sql = is_group
		? "INSERT INTO telegram_link_previews "
		  "  (group_message_id, url, display_url, site_name, title, "
		  "   description, description_entities, author, type, "
		  "   instant_view_version, has_large_media, show_large_media, "
		  "   show_above_text, show_media_above_description, "
		  "   skip_confirmation, extra) "
		  "SELECT m.id, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ? "
		  "  FROM telegram_group_messages m "
		  " WHERE m.chat_id = ? AND m.message_id = ? "
		  "ON DUPLICATE KEY UPDATE "
		  "  url = VALUES(url), display_url = VALUES(display_url), "
		  "  site_name = VALUES(site_name), title = VALUES(title), "
		  "  description = VALUES(description), author = VALUES(author), "
		  "  type = VALUES(type), "
		  "  instant_view_version = VALUES(instant_view_version), "
		  "  has_large_media = VALUES(has_large_media), "
		  "  show_large_media = VALUES(show_large_media), "
		  "  show_above_text = VALUES(show_above_text), "
		  "  description_entities = VALUES(description_entities), "
		  "  show_media_above_description = "
		  "    VALUES(show_media_above_description), "
		  "  skip_confirmation = VALUES(skip_confirmation), "
		  "  extra = VALUES(extra)"
		: "INSERT INTO telegram_link_previews "
		  "  (private_message_id, url, display_url, site_name, title, "
		  "   description, description_entities, author, type, "
		  "   instant_view_version, has_large_media, show_large_media, "
		  "   show_above_text, show_media_above_description, "
		  "   skip_confirmation, extra) "
		  "SELECT m.id, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ? "
		  "  FROM telegram_private_messages m "
		  " WHERE m.chat_id = ? AND m.message_id = ? "
		  "ON DUPLICATE KEY UPDATE "
		  "  url = VALUES(url), display_url = VALUES(display_url), "
		  "  site_name = VALUES(site_name), title = VALUES(title), "
		  "  description = VALUES(description), author = VALUES(author), "
		  "  type = VALUES(type), "
		  "  instant_view_version = VALUES(instant_view_version), "
		  "  has_large_media = VALUES(has_large_media), "
		  "  show_large_media = VALUES(show_large_media), "
		  "  show_above_text = VALUES(show_above_text), "
		  "  description_entities = VALUES(description_entities), "
		  "  show_media_above_description = "
		  "    VALUES(show_media_above_description), "
		  "  skip_confirmation = VALUES(skip_confirmation), "
		  "  extra = VALUES(extra)";

	mysql::Param entities = std::monostate{};
	if (lp.description_entities.has_value())
		entities = *lp.description_entities;
	mysql::Param extra = std::monostate{};
	if (lp.extra.has_value())
		extra = *lp.extra;

	/*
	 * url/display_url are TEXT (migration 000028) and stored whole. The
	 * metadata fields keep their VARCHAR limits and are clamped to them:
	 * they come from an arbitrary page's own markup, and losing the tail
	 * of a title is nothing next to losing the whole preview.
	 */
	db_.execute(sql, { lp.url, lp.display_url,
			   utf8_truncate_chars(lp.site_name, 255),
			   utf8_truncate_chars(lp.title, 512),
			   lp.description, entities,
			   utf8_truncate_chars(lp.author, 255), lp.type,
			   (int64_t)lp.instant_view_version,
			   (int64_t)(lp.has_large_media ? 1 : 0),
			   (int64_t)(lp.show_large_media ? 1 : 0),
			   (int64_t)(lp.show_above_text ? 1 : 0),
			   (int64_t)(lp.show_media_above_description ? 1 : 0),
			   (int64_t)(lp.skip_confirmation ? 1 : 0), extra,
			   lp.chat_id, lp.message_id });
}

void DB::setLinkPreviewPhoto(int64_t chat_id, int64_t message_id,
			     bool is_group, uint64_t file_id)
{
	const char *sql = is_group
		? "UPDATE telegram_link_previews p "
		  "  JOIN telegram_group_messages m ON m.id = p.group_message_id "
		  "   SET p.photo_file_id = ? "
		  " WHERE m.chat_id = ? AND m.message_id = ?"
		: "UPDATE telegram_link_previews p "
		  "  JOIN telegram_private_messages m "
		  "    ON m.id = p.private_message_id "
		  "   SET p.photo_file_id = ? "
		  " WHERE m.chat_id = ? AND m.message_id = ?";
	db_.execute(sql, { (int64_t)file_id, chat_id, message_id });
}

std::optional<DiscordOrigin> DB::getDiscordOrigin(int64_t chat_id,
						  int64_t message_id)
{
	/*
	 * The route supplies the channel when the message row itself is
	 * missing -- discordd records what it SENT before it necessarily has
	 * the message logged, and a jump link only needs guild + channel + id.
	 */
	auto rows = db_.query(
		"SELECT dts.discord_message_id, "
		"       COALESCE(dm.channel_id, rt.discord_channel_id), "
		"       COALESCE(dm.guild_id, ch.guild_id), "
		"       dm.author_id, du.username, du.discriminator, "
		"       du.avatar, dm.content "
		"FROM discord_telegram_sent_messages dts "
		"JOIN discord_telegram_routes rt ON rt.id = dts.route_id "
		"LEFT JOIN discord_messages dm ON dm.id = dts.discord_message_id "
		"LEFT JOIN discord_channels ch ON ch.id = rt.discord_channel_id "
		"LEFT JOIN discord_users du ON du.id = dm.author_id "
		"WHERE dts.telegram_chat_id = ? AND dts.telegram_message_id = ? "
		"LIMIT 1",
		{ chat_id, message_id });
	if (rows.empty())
		return std::nullopt;

	DiscordOrigin o;
	o.message_id    = rows[0][0].value_or("");
	o.channel_id    = rows[0][1].value_or("");
	o.guild_id      = rows[0][2].value_or("");
	o.author_id     = rows[0][3].value_or("");
	o.username      = rows[0][4].value_or("");
	o.discriminator = rows[0][5].value_or("");
	o.avatar        = rows[0][6].value_or("");
	o.content       = rows[0][7].value_or("");
	if (o.message_id.empty())
		return std::nullopt;
	return o;
}

std::optional<QuotedMessage> DB::getQuotedMessage(int64_t chat_id,
						  int64_t message_id)
{
	/* Group ids are negative, private (peer user) ids positive. */
	const char *sql = chat_id < 0
		? "SELECT COALESCE(u.first_name,''), COALESCE(u.last_name,''), "
		  "gm.text, gm.sender_user_id, gm.sender_chat_id "
		  "FROM telegram_group_messages gm "
		  "LEFT JOIN telegram_users u ON u.id = gm.sender_user_id "
		  "WHERE gm.chat_id = ? AND gm.message_id = ?"
		: "SELECT COALESCE(u.first_name,''), COALESCE(u.last_name,''), "
		  "pm.text, pm.sender_id, NULL "
		  "FROM telegram_private_messages pm "
		  "LEFT JOIN telegram_users u ON u.id = pm.sender_id "
		  "WHERE pm.chat_id = ? AND pm.message_id = ?";

	auto rows = db_.query(sql, { chat_id, message_id });
	if (rows.empty())
		return std::nullopt;

	QuotedMessage q;
	std::string first = rows[0][0].value_or("");
	std::string last  = rows[0][1].value_or("");
	q.sender_name = first;
	if (!last.empty())
		q.sender_name += (q.sender_name.empty() ? "" : " ") + last;
	q.text = rows[0][2].value_or("");
	if (rows[0][3].has_value())
		q.sender_id = std::stoll(*rows[0][3]);
	if (rows[0][4].has_value())
		q.sender_chat_id = std::stoll(*rows[0][4]);
	return q;
}

std::optional<FileInfo> DB::getFileInfo(uint64_t files_id)
{
	auto rows = db_.query(
		"SELECT file_type, file_ext, on_disk FROM telegram_files WHERE id = ?",
		{ files_id });
	if (rows.empty())
		return std::nullopt;
	FileInfo fi;
	fi.file_type = rows[0][0].value_or("unknown");
	fi.ext       = rows[0][1].value_or("");
	fi.on_disk   = rows[0][2].has_value() && *rows[0][2] == "1";
	return fi;
}

uint64_t DB::internEndpoint(const std::string &webhook_url)
{
	{
		std::lock_guard<std::mutex> lk(endpoint_mtx_);
		auto it = endpoint_ids_.find(webhook_url);
		if (it != endpoint_ids_.end())
			return it->second;
	}

	/* Insert-or-ignore then look up by the UNIQUE url: pool-safe (no
	 * LAST_INSERT_ID, which would need the same connection). */
	db_.execute("INSERT IGNORE INTO discord_endpoints (webhook_url) "
		    "VALUES (?)", { webhook_url });
	auto rows = db_.query("SELECT id FROM discord_endpoints WHERE "
			      "webhook_url = ?", { webhook_url });
	if (rows.empty() || !rows[0][0].has_value())
		throw std::runtime_error("discord_endpoints intern failed");
	uint64_t id = (uint64_t)std::stoull(*rows[0][0]);

	std::lock_guard<std::mutex> lk(endpoint_mtx_);
	endpoint_ids_[webhook_url] = id;
	return id;
}

void DB::recordSentMessage(int64_t chat_id, int64_t message_id,
			   const std::string &webhook_url,
			   const std::string &discord_message_id,
			   const char *kind)
{
	uint64_t endpoint_id = internEndpoint(webhook_url);
	db_.execute(
		"INSERT INTO telegram_discord_sent_messages "
		"(telegram_chat_id, telegram_message_id, endpoint_id, message_id, "
		"kind) VALUES (?, ?, ?, ?, ?)",
		{ chat_id, message_id, endpoint_id, discord_message_id,
		  std::string(kind) });
}

std::vector<SentMessage> DB::getSentMessages(int64_t chat_id,
					     int64_t message_id,
					     const char *kind)
{
	std::string sql =
		"SELECT e.webhook_url, m.message_id, m.kind "
		"FROM telegram_discord_sent_messages m "
		"JOIN discord_endpoints e ON e.id = m.endpoint_id "
		"WHERE m.telegram_chat_id = ? AND m.telegram_message_id = ?";
	std::vector<mysql::Param> params = { chat_id, message_id };
	if (kind) {
		sql += " AND m.kind = ?";
		params.push_back(std::string(kind));
	}

	auto rows = db_.query(sql, params);
	std::vector<SentMessage> out;
	out.reserve(rows.size());
	for (const auto &r : rows) {
		if (!r[0].has_value() || !r[1].has_value())
			continue;
		out.push_back({ *r[0], *r[1], r[2].value_or("text") });
	}
	return out;
}

std::optional<MessageForward> DB::getMessageForward(int64_t chat_id,
						    int64_t message_id)
{
	/* Group ids are negative, private (peer user) ids positive. */
	const char *sql = chat_id < 0
		? "SELECT text, reply_to_chat_id, reply_to_msg_id, file_id, "
		  "content_type "
		  "FROM telegram_group_messages WHERE chat_id = ? AND message_id = ?"
		: "SELECT text, reply_to_chat_id, reply_to_msg_id, file_id, "
		  "content_type "
		  "FROM telegram_private_messages WHERE chat_id = ? AND message_id = ?";

	auto rows = db_.query(sql, { chat_id, message_id });
	if (rows.empty())
		return std::nullopt;

	MessageForward mf;
	mf.text = rows[0][0].value_or("");
	if (rows[0][1].has_value())
		mf.reply_to_chat_id = std::stoll(*rows[0][1]);
	if (rows[0][2].has_value())
		mf.reply_to_msg_id = std::stoll(*rows[0][2]);
	if (rows[0][3].has_value())
		mf.file_id = (uint64_t)std::stoull(*rows[0][3]);
	mf.content_type = rows[0][4].value_or("");
	return mf;
}

void DB::deleteSentMessages(int64_t chat_id, int64_t message_id)
{
	db_.execute(
		"DELETE FROM telegram_discord_sent_messages "
		"WHERE telegram_chat_id = ? AND telegram_message_id = ?",
		{ chat_id, message_id });
}

void DB::recordTextHistory(mysql::Transaction &tx, const char *table,
			   const char *fk_column, const char *value_column,
			   int64_t entity_id, const std::string &value)
{
	/*
	 * Never store an empty snapshot: a user bio or group description is
	 * unknown when its row is first created (it arrives later with full
	 * info), and recording the empty placeholder is what filled these
	 * tables with blank rows.
	 */
	if (value.empty())
		return;

	/* Skip if unchanged since the last snapshot, so repeated full-info
	 * refreshes of the same text do not pile up duplicates. */
	std::string sel = std::string("SELECT ") + value_column + " FROM " +
		table + " WHERE " + fk_column + " = ? ORDER BY id DESC LIMIT 1";
	auto last = tx.query(sel, { entity_id });
	if (!last.empty() && last[0][0].value_or("") == value)
		return;

	std::string ins = std::string("INSERT INTO ") + table + " (" +
		fk_column + ", " + value_column + ") VALUES (?, ?)";
	tx.insert(ins, { entity_id, value });
}

bool DB::bumpMsgCount(mysql::Transaction &tx, const char *table, int64_t id)
{
	tx.execute(std::string("UPDATE ") + table +
		   " SET msg_count = msg_count + 1 WHERE id = ?", { id });

	auto r = tx.query(std::string("SELECT msg_count FROM ") + table +
			  " WHERE id = ?", { id });
	if (r.empty() || !r[0][0].has_value())
		return false;
	uint64_t c = std::stoull(*r[0][0]);
	return c != 0 && (c % 10 == 0);
}

} /* namespace tgloggerd */
