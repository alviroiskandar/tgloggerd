// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <tgloggerd/DB.hpp>

#include <string>
#include <cstdint>

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
		"SELECT chat_id, webhook_url FROM discord_webhooks "
		"WHERE enabled = 1");

	std::vector<DiscordWebhook> out;
	out.reserve(rows.size());
	for (const auto &r : rows) {
		if (!r[0].has_value() || !r[1].has_value())
			continue;
		out.push_back({ std::stoll(*r[0]), *r[1] });
	}
	return out;
}

std::optional<uint64_t> DB::getUserPhotoFileId(int64_t user_id)
{
	auto rows = db_.query(
		"SELECT profile_photo_file_id FROM users WHERE id = ?",
		{ user_id });
	if (rows.empty() || !rows[0][0].has_value())
		return std::nullopt;
	return (uint64_t)std::stoull(*rows[0][0]);
}

ChatPhoto DB::getGroupPhoto(int64_t chat_id)
{
	ChatPhoto out;
	auto rows = db_.query(
		"SELECT photo_file_id, title FROM `groups` WHERE id = ?",
		{ chat_id });
	if (rows.empty())
		return out;
	if (rows[0][0].has_value())
		out.photo_file_id = (uint64_t)std::stoull(*rows[0][0]);
	out.title = rows[0][1].value_or("");
	return out;
}

std::optional<QuotedMessage> DB::getQuotedMessage(int64_t chat_id,
						  int64_t message_id)
{
	/* Group ids are negative, private (peer user) ids positive. */
	const char *sql = chat_id < 0
		? "SELECT COALESCE(u.first_name,''), COALESCE(u.last_name,''), "
		  "gm.text FROM group_messages gm "
		  "LEFT JOIN users u ON u.id = gm.sender_user_id "
		  "WHERE gm.chat_id = ? AND gm.message_id = ?"
		: "SELECT COALESCE(u.first_name,''), COALESCE(u.last_name,''), "
		  "pm.text FROM private_messages pm "
		  "LEFT JOIN users u ON u.id = pm.sender_id "
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
	return q;
}

std::optional<FileInfo> DB::getFileInfo(uint64_t files_id)
{
	auto rows = db_.query(
		"SELECT file_type, file_ext, on_disk FROM files WHERE id = ?",
		{ files_id });
	if (rows.empty())
		return std::nullopt;
	FileInfo fi;
	fi.file_type = rows[0][0].value_or("unknown");
	fi.ext       = rows[0][1].value_or("");
	fi.on_disk   = rows[0][2].has_value() && *rows[0][2] == "1";
	return fi;
}

void DB::recordSentMessage(int64_t chat_id, int64_t message_id,
			   const std::string &webhook_url,
			   const std::string &discord_message_id,
			   const char *kind, const std::string &content)
{
	db_.execute(
		"INSERT INTO discord_sent_messages "
		"(chat_id, message_id, webhook_url, discord_message_id, kind, "
		"content) VALUES (?, ?, ?, ?, ?, ?)",
		{ chat_id, message_id, webhook_url, discord_message_id,
		  std::string(kind), content });
}

std::vector<SentMessage> DB::getSentMessages(int64_t chat_id,
					     int64_t message_id,
					     const char *kind)
{
	std::string sql =
		"SELECT webhook_url, discord_message_id, content "
		"FROM discord_sent_messages WHERE chat_id = ? AND message_id = ?";
	std::vector<mysql::Param> params = { chat_id, message_id };
	if (kind) {
		sql += " AND kind = ?";
		params.push_back(std::string(kind));
	}

	auto rows = db_.query(sql, params);
	std::vector<SentMessage> out;
	out.reserve(rows.size());
	for (const auto &r : rows) {
		if (!r[0].has_value() || !r[1].has_value())
			continue;
		out.push_back({ *r[0], *r[1], r[2].value_or("") });
	}
	return out;
}

void DB::updateSentContent(int64_t chat_id, int64_t message_id,
			   const char *kind, const std::string &content)
{
	db_.execute(
		"UPDATE discord_sent_messages SET content = ? "
		"WHERE chat_id = ? AND message_id = ? AND kind = ?",
		{ content, chat_id, message_id, std::string(kind) });
}

void DB::deleteSentMessages(int64_t chat_id, int64_t message_id)
{
	db_.execute(
		"DELETE FROM discord_sent_messages "
		"WHERE chat_id = ? AND message_id = ?",
		{ chat_id, message_id });
}

void DB::pruneSentMessages(int days)
{
	db_.execute(
		"DELETE FROM discord_sent_messages "
		"WHERE created_at < (UTC_TIMESTAMP() - INTERVAL ? DAY)",
		{ (int64_t)days });
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
