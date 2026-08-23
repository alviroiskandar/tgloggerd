// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "DB.hpp"

#include <cstdlib>

namespace discordd {

/* Snowflakes exceed INT64_MAX only in theory, but bind them as unsigned. */
static inline mysql::Param u64(uint64_t v)
{
	return mysql::Param{v};
}

static inline mysql::Param i64(int64_t v)
{
	return mysql::Param{v};
}

static int64_t row_i64(const mysql::Row &r, size_t i)
{
	if (i >= r.size() || !r[i].has_value())
		return 0;
	return strtoll(r[i]->c_str(), nullptr, 10);
}

static uint64_t row_u64(const mysql::Row &r, size_t i)
{
	if (i >= r.size() || !r[i].has_value())
		return 0;
	return strtoull(r[i]->c_str(), nullptr, 10);
}

static std::string row_str(const mysql::Row &r, size_t i)
{
	if (i >= r.size() || !r[i].has_value())
		return std::string();
	return *r[i];
}

DB::DB(const mysql::Config &cfg) : db_(new mysql::Database(cfg))
{
}

DB::~DB(void) = default;

void DB::ping(void)
{
	db_->query("SELECT 1");
}

std::vector<Route> DB::loadRoutes(void)
{
	static const char *SQL =
		"SELECT r.id, r.discord_channel_id, r.telegram_chat_id, "
		"       r.telegram_bot_id, b.token, b.bot_user_id, b.username "
		"FROM discord_telegram_routes r "
		"JOIN telegram_bots b ON b.id = r.telegram_bot_id "
		"WHERE r.enabled = 1 AND b.enabled = 1";

	std::vector<Route> out;
	for (const auto &r : db_->query(SQL)) {
		Route rt;
		rt.id = row_u64(r, 0);
		rt.discord_channel_id = row_u64(r, 1);
		rt.telegram_chat_id = row_i64(r, 2);
		rt.telegram_bot_id = row_u64(r, 3);
		rt.bot_token = row_str(r, 4);
		rt.bot_user_id = row_i64(r, 5);
		rt.bot_username = row_str(r, 6);
		out.push_back(std::move(rt));
	}
	return out;
}

void DB::setBotUserId(uint64_t bot_row_id, int64_t user_id,
		      const std::string &username)
{
	db_->execute("UPDATE telegram_bots SET bot_user_id = ?, username = ? "
		     "WHERE id = ?",
		     {i64(user_id), username, u64(bot_row_id)});
}

void DB::upsertGuild(uint64_t id, const std::string &name)
{
	if (!id)
		return;
	db_->execute("INSERT INTO discord_guilds (id, name) VALUES (?, ?) "
		     "ON DUPLICATE KEY UPDATE name = VALUES(name)",
		     {u64(id), name});
}

void DB::upsertChannel(uint64_t id, uint64_t guild_id,
		       const std::string &name, int type)
{
	if (!id)
		return;
	db_->execute("INSERT INTO discord_channels (id, guild_id, name, type) "
		     "VALUES (?, ?, ?, ?) "
		     "ON DUPLICATE KEY UPDATE guild_id = VALUES(guild_id), "
		     "  name = VALUES(name), type = VALUES(type)",
		     {u64(id), u64(guild_id), name, (int64_t)type});
}

void DB::upsertUser(uint64_t id, const std::string &username,
		    const std::string &global_name,
		    const std::string &discriminator, const std::string &avatar,
		    bool is_bot)
{
	if (!id)
		return;
	db_->execute(
		"INSERT INTO discord_users "
		"  (id, username, global_name, discriminator, avatar, is_bot) "
		"VALUES (?, ?, ?, ?, ?, ?) "
		"ON DUPLICATE KEY UPDATE username = VALUES(username), "
		"  global_name = VALUES(global_name), "
		"  discriminator = VALUES(discriminator), "
		"  avatar = VALUES(avatar), is_bot = VALUES(is_bot)",
		{u64(id), username, global_name, discriminator, avatar,
		 (int64_t)(is_bot ? 1 : 0)});
}

void DB::upsertMessage(uint64_t id, uint64_t channel_id, uint64_t guild_id,
		       uint64_t author_id, uint64_t webhook_id,
		       const std::string &content, uint64_t reply_to_message_id,
		       uint64_t sent_at_ms, bool edited)
{
	/*
	 * FROM_UNIXTIME with a fractional argument gives DATETIME(3); the
	 * snowflake carries milliseconds, so divide rather than truncate.
	 */
	const double secs = (double)sent_at_ms / 1000.0;

	if (edited) {
		db_->execute(
			"INSERT INTO discord_messages "
			"  (id, channel_id, guild_id, author_id, webhook_id, "
			"   content, reply_to_message_id, sent_at, edited_at) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, FROM_UNIXTIME(?), NOW(3)) "
			"ON DUPLICATE KEY UPDATE content = VALUES(content), "
			"  edited_at = NOW(3)",
			{u64(id), u64(channel_id), u64(guild_id), u64(author_id),
			 u64(webhook_id), content, u64(reply_to_message_id),
			 secs});
		return;
	}

	db_->execute(
		"INSERT INTO discord_messages "
		"  (id, channel_id, guild_id, author_id, webhook_id, content, "
		"   reply_to_message_id, sent_at) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, FROM_UNIXTIME(?)) "
		"ON DUPLICATE KEY UPDATE content = VALUES(content)",
		{u64(id), u64(channel_id), u64(guild_id), u64(author_id),
		 u64(webhook_id), content, u64(reply_to_message_id), secs});
}

void DB::upsertAttachment(uint64_t id, uint64_t message_id,
			  const std::string &filename,
			  const std::string &content_type, uint64_t size,
			  const std::string &url, int width, int height)
{
	if (!id)
		return;
	db_->execute("INSERT INTO discord_attachments "
		     "  (id, message_id, filename, content_type, size, url, "
		     "   width, height) "
		     "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
		     "ON DUPLICATE KEY UPDATE url = VALUES(url), "
		     "  filename = VALUES(filename), size = VALUES(size)",
		     {u64(id), u64(message_id), filename, content_type,
		      u64(size), url, (int64_t)width, (int64_t)height});
}

void DB::markMessageDeleted(uint64_t message_id)
{
	db_->execute("UPDATE discord_messages SET deleted_at = NOW() "
		     "WHERE id = ? AND deleted_at IS NULL",
		     {u64(message_id)});
}

bool DB::alreadyForwarded(uint64_t discord_message_id, uint64_t route_id)
{
	auto rows = db_->query("SELECT 1 FROM discord_telegram_sent_messages "
			       "WHERE discord_message_id = ? AND route_id = ? "
			       "LIMIT 1",
			       {u64(discord_message_id), u64(route_id)});
	return !rows.empty();
}

void DB::recordForwarded(uint64_t discord_message_id, uint64_t route_id,
			 int64_t telegram_chat_id, int64_t telegram_message_id)
{
	db_->execute("INSERT IGNORE INTO discord_telegram_sent_messages "
		     "  (discord_message_id, route_id, telegram_chat_id, "
		     "   telegram_message_id) "
		     "VALUES (?, ?, ?, ?)",
		     {u64(discord_message_id), u64(route_id),
		      i64(telegram_chat_id), i64(telegram_message_id)});
}

std::vector<ForwardedMessage> DB::getForwarded(uint64_t discord_message_id)
{
	static const char *SQL =
		"SELECT f.route_id, f.telegram_chat_id, f.telegram_message_id, "
		"       r.telegram_bot_id, b.token "
		"FROM discord_telegram_sent_messages f "
		"JOIN discord_telegram_routes r ON r.id = f.route_id "
		"JOIN telegram_bots b ON b.id = r.telegram_bot_id "
		"WHERE f.discord_message_id = ?";

	std::vector<ForwardedMessage> out;
	for (const auto &r : db_->query(SQL, {u64(discord_message_id)})) {
		ForwardedMessage fm;
		fm.route_id = row_u64(r, 0);
		fm.telegram_chat_id = row_i64(r, 1);
		fm.telegram_message_id = row_i64(r, 2);
		fm.telegram_bot_id = row_u64(r, 3);
		fm.bot_token = row_str(r, 4);
		out.push_back(std::move(fm));
	}
	return out;
}

std::optional<ReplyTarget> DB::resolveReply(uint64_t discord_reply_to_id,
					    int64_t telegram_chat_id)
{
	if (!discord_reply_to_id)
		return std::nullopt;

	/*
	 * 1. discordd forwarded the replied-to message itself. Constrain to
	 *    the destination chat: replying across chats is meaningless.
	 */
	auto rows = db_->query(
		"SELECT telegram_chat_id, telegram_message_id "
		"FROM discord_telegram_sent_messages "
		"WHERE discord_message_id = ? AND telegram_chat_id = ? LIMIT 1",
		{u64(discord_reply_to_id), i64(telegram_chat_id)});
	if (!rows.empty()) {
		ReplyTarget t;
		t.telegram_chat_id = row_i64(rows[0], 0);
		t.telegram_message_id = row_i64(rows[0], 1);
		if (t.telegram_message_id)
			return t;
	}

	/*
	 * 2. The replied-to message is itself a mirror of a Telegram message,
	 *    posted into Discord by the Telegram -> Discord webhook forwarder.
	 *    That table keys the Discord id as a string, so bind it as text.
	 */
	rows = db_->query("SELECT telegram_chat_id, telegram_message_id "
			  "FROM telegram_discord_sent_messages "
			  "WHERE message_id = ? AND telegram_chat_id = ? "
			  "LIMIT 1",
			  {std::to_string(discord_reply_to_id),
			   i64(telegram_chat_id)});
	if (!rows.empty()) {
		ReplyTarget t;
		t.telegram_chat_id = row_i64(rows[0], 0);
		t.telegram_message_id = row_i64(rows[0], 1);
		if (t.telegram_message_id)
			return t;
	}

	return std::nullopt;
}

} /* namespace discordd */
