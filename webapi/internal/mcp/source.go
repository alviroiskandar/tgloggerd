// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package mcp

import "strings"

// msgSource is the message table a tool reads and the spelling of what
// differs between the group and the private chat tables: the sender's
// column, the edits' and the link previews' key, the chat's title. The
// tools write their statements once, over a source; groupSource is
// every tool's, privateSource (private.go) only that of a private chat
// the token's scope reaches, which the gate picks (gate.go).
type msgSource struct {
	table     string // the message table, aliased m in every statement
	edits     string // its edits table
	editKey   string // the edits' (and the link previews') column of m.id
	sender    string // m's sender user column
	channel   string // m.is_channel_post, or what stands in for it
	chatJoin  string // the join that gives the chat's title, as g.title
	chatTitle string
	schema    schema // the fields a filter may name
	// selectCols decorates an already-chosen set of messages. The
	// statement timeout is here, on the outermost SELECT: MySQL
	// ignores the hint inside a derived table, so it has to be on the
	// statement that wraps the id selection.
	selectCols string
	// joins are the display joins; they only ever run over the rows
	// already chosen, never over the table searched.
	joins string
}

func newSource(s msgSource) *msgSource {
	s.selectCols = "SELECT /*+ MAX_EXECUTION_TIME(5000) */ " +
		"m.message_id, m.chat_id, " + s.sender + " AS sender_user_id, m.date, " +
		"FROM_UNIXTIME(m.date) AS sent_at, m.content_type, m.text, " +
		"m.is_forwarded, " + s.channel + " AS is_channel_post, m.reply_to_msg_id, m.deleted_at, " +
		"m.edit_date, " +
		s.chatTitle + " AS group_title, " +
		"TRIM(CONCAT(COALESCE(su.first_name,''), ' ', " +
		"            COALESCE(su.last_name,''))) AS sender_name, " +
		"(SELECT x.username FROM telegram_user_usernames x " +
		"  WHERE x.user_id = " + s.sender + " AND x.kind = 'active' " +
		"  ORDER BY x.position LIMIT 1) AS sender_username, " +
		"f.id AS file_id, f.file_type, f.file_size, f.file_ext, " +
		"f.orig_file_name, f.on_disk, m.id AS id_internal, " +
		"(SELECT COUNT(1) FROM " + s.edits + " pe " +
		"  WHERE pe." + s.editKey + " = m.id) AS prev_versions, " +
		"lp.url AS lp_url, lp.display_url AS lp_display_url, " +
		"lp.site_name AS lp_site_name, lp.title AS lp_title, " +
		"LEFT(lp.description, 1000) AS lp_description, lp.author AS lp_author, " +
		"lp.type AS lp_type, lp.instant_view_version AS lp_iv, " +
		"lp.photo_file_id AS lp_photo_id, lpf.file_type AS lp_photo_type, " +
		"lpf.file_size AS lp_photo_size, lpf.on_disk AS lp_photo_on_disk "
	s.joins = s.chatJoin +
		"LEFT JOIN telegram_users su ON su.id = " + s.sender + " " +
		"LEFT JOIN telegram_files f ON f.id = m.file_id " +
		"LEFT JOIN telegram_link_previews lp ON lp." + s.editKey + " = m.id " +
		"LEFT JOIN telegram_files lpf ON lpf.id = lp.photo_file_id "
	return &s
}

// respell is a filter schema with the group table's column names
// replaced by another table's.
func respell(s schema, r *strings.Replacer) schema {
	out := make(schema, len(s))
	for i, f := range s {
		f.expr = r.Replace(f.expr)
		out[i] = f
	}
	return out
}

// groupSource is the group messages, the table every tool reads unless
// the gate picks a private chat.
var groupSource = newSource(msgSource{
	table:     "telegram_group_messages",
	edits:     "telegram_group_message_edits",
	editKey:   "group_message_id",
	sender:    "m.sender_user_id",
	channel:   "m.is_channel_post",
	chatJoin:  "JOIN `telegram_groups` g ON g.id = m.chat_id ",
	chatTitle: "g.title",
	schema:    msgSchema,
})
