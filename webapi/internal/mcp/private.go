// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package mcp

import (
	"context"
	"strconv"
	"strings"
)

// PRIVATE CHATS. This is the only file of the package that names the
// direct messages' table (mcp_test.go checks it), and privateSource is
// read only when the gate (gate.go) picks it: for a private chat a
// superadmin's token has in its scope, named by its id in group_id.
// Every other token, and every call that does not name such a chat,
// reads the groups alone.
//
// The private table has no channel posts and calls its sender
// sender_id (NULL for the logged-in account itself); the chat's title
// is the peer's name.
var privateSource = newSource(msgSource{
	table:     "telegram_private_messages",
	edits:     "telegram_private_message_edits",
	editKey:   "private_message_id",
	sender:    "m.sender_id",
	channel:   "0",
	chatJoin:  "LEFT JOIN telegram_users g ON g.id = m.chat_id ",
	chatTitle: "TRIM(CONCAT(COALESCE(g.first_name,''), ' ', COALESCE(g.last_name,'')))",
	schema: respell(msgSchema, strings.NewReplacer(
		"m.sender_user_id", "m.sender_id",
		"m.is_channel_post", "0")),
})

// privateChats lists the private chats a token reaches, with the
// peer's name, for telegram_list_groups; a chat whose peer the archive
// does not know is left out.
func (t *telegram) privateChats(ctx context.Context, chats []int64) ([]any, error) {
	binds := make([]any, len(chats))
	for i, c := range chats {
		binds[i] = strconv.FormatInt(c, 10)
	}
	rows, err := execTool(ctx, t.db, "SELECT u.id, "+
		"TRIM(CONCAT(COALESCE(u.first_name,''), ' ', COALESCE(u.last_name,''))) AS name, "+
		usernameSubq("u.id")+" AS username "+
		"FROM telegram_users u WHERE u.id IN ("+placeholders(len(chats), ",")+") "+
		"ORDER BY u.id", binds, queryFailed)
	if err != nil {
		return nil, err
	}
	out := []any{}
	for _, r := range rows {
		out = append(out, map[string]any{
			"chat_id":  r.i64("id"),
			"name":     r.str("name"),
			"username": r.str("username"),
		})
	}
	return out, nil
}
