// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <tgloggerd/DB.hpp>

#include <string>
#include <optional>

namespace tgloggerd {

namespace models {

const char *to_string(MessageContentType t)
{
	switch (t) {
	case MessageContentType::Text:		return "text";
	case MessageContentType::Photo:		return "photo";
	case MessageContentType::Video:		return "video";
	case MessageContentType::Document:	return "document";
	case MessageContentType::Audio:		return "audio";
	case MessageContentType::Voice:		return "voice";
	case MessageContentType::Sticker:	return "sticker";
	case MessageContentType::Animation:	return "animation";
	case MessageContentType::Service:	return "service";
	case MessageContentType::Unknown:	return "unknown";
	}
	return "unknown";
}

const char *to_string(ForwardOriginType t)
{
	switch (t) {
	case ForwardOriginType::User:		return "user";
	case ForwardOriginType::HiddenUser:	return "hidden_user";
	case ForwardOriginType::Chat:		return "chat";
	case ForwardOriginType::Channel:	return "channel";
	}
	return "user";
}

MessageContentType message_content_type_from_string(const std::string &s)
{
	if (s == "text")	return MessageContentType::Text;
	if (s == "photo")	return MessageContentType::Photo;
	if (s == "video")	return MessageContentType::Video;
	if (s == "document")	return MessageContentType::Document;
	if (s == "audio")	return MessageContentType::Audio;
	if (s == "voice")	return MessageContentType::Voice;
	if (s == "sticker")	return MessageContentType::Sticker;
	if (s == "animation")	return MessageContentType::Animation;
	if (s == "service")	return MessageContentType::Service;
	return MessageContentType::Unknown;
}

} /* namespace models */

/*
 * The following DB helpers implement the parts of message upserting that
 * are identical for private and group messages. They take the target
 * table and foreign-key column names (compile-time literals, never user
 * input) so the same logic serves private_message_* and group_message_*.
 */

bool DB::snapshotMessageEditIfChanged(mysql::Transaction &tx,
				      const char *edits_table,
				      const char *fk_column,
				      uint64_t message_row_id,
				      const models::MessageContent &old_content,
				      int64_t old_edit_date,
				      const models::MessageContent &new_content,
				      int64_t new_edit_date)
{
	/*
	 * Only a genuine edit (edit_date advanced) that actually changed the
	 * content is worth a snapshot. The edits table is append-only; the
	 * live row in the *_messages table always holds the current content.
	 */
	if (new_edit_date <= old_edit_date)
		return false;
	/*
	 * file_id is deliberately excluded from the comparison: media files
	 * are linked asynchronously after the row is written, so new_content
	 * never carries one at build time. Comparing it would flag every
	 * media message as edited. The old file_id is still snapshotted.
	 */
	if (old_content.content_type == new_content.content_type &&
	    old_content.text == new_content.text &&
	    old_content.entities == new_content.entities)
		return false;

	mysql::Param text_param = std::monostate{};
	if (old_content.text.has_value())
		text_param = *old_content.text;

	mysql::Param entities_param = std::monostate{};
	if (old_content.entities.has_value())
		entities_param = *old_content.entities;

	mysql::Param file_param = std::monostate{};
	if (old_content.file_id.has_value())
		file_param = (int64_t)*old_content.file_id;

	std::string sql = std::string("INSERT INTO ") + edits_table + " (" +
		fk_column + ", content_type, text, entities, file_id, edit_date)"
		" VALUES (?, ?, ?, ?, ?, ?)";

	/* The snapshot records the content *before* the edit, keyed by the
	 * edit_date that triggered it (the new edit_date of the live row). */
	tx.execute(sql, {
		(int64_t)message_row_id,
		std::string(models::to_string(old_content.content_type)),
		text_param,
		entities_param,
		file_param,
		(int64_t)new_edit_date,
	});
	return true;
}

void DB::insertForwardInfo(mysql::Transaction &tx, const char *table,
			   const char *fk_column, uint64_t message_row_id,
			   const models::ForwardInfo &info)
{
	/*
	 * Idempotent: fk_column is UNIQUE in the *_fwd_info tables, so skip
	 * if forward info for this message has already been recorded.
	 */
	std::string exists_sql = std::string("SELECT 1 FROM ") + table +
		" WHERE " + fk_column + " = ?";
	if (!tx.query(exists_sql, { (int64_t)message_row_id }).empty())
		return;

	mysql::Param sender_id = std::monostate{};
	if (info.origin_sender_user_id.has_value())
		sender_id = (int64_t)*info.origin_sender_user_id;

	mysql::Param sender_name = std::monostate{};
	if (info.origin_sender_name.has_value())
		sender_name = *info.origin_sender_name;

	mysql::Param chat_id_param = std::monostate{};
	if (info.origin_chat_id.has_value())
		chat_id_param = (int64_t)*info.origin_chat_id;

	mysql::Param msg_id_param = std::monostate{};
	if (info.origin_message_id.has_value())
		msg_id_param = (int64_t)*info.origin_message_id;

	std::string sql = std::string("INSERT INTO ") + table + " (" +
		fk_column + ", origin_type, origin_sender_user_id,"
		" origin_sender_name, origin_chat_id, origin_message_id,"
		" origin_date) VALUES (?, ?, ?, ?, ?, ?, ?)";

	tx.execute(sql, {
		(int64_t)message_row_id,
		std::string(models::to_string(info.origin_type)),
		sender_id,
		sender_name,
		chat_id_param,
		msg_id_param,
		(int64_t)info.origin_date,
	});
}

} /* namespace tgloggerd */
