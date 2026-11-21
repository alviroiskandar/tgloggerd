// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package mcp

import (
	"context"
	"strconv"
	"strings"
	"sync"
)

// PRIVATE CHATS. This is the only file of the package that names the
// direct messages' table (mcp_test.go checks it), and privateSource is
// read only when the gate (gate.go) picks it: for a private chat a
// superadmin's token has in its scope, named by its id in group_id.
// Every other token, and every call that does not name such a chat,
// reads the groups alone.
//
// The private table has no channel posts and calls its sender
// sender_id, NULL for the logging account itself; the chat's title is
// the peer's name. A message the logging account sent is given that
// account as its sender, as the web's chat shows it, once the daemon
// has said which account it is (self); until then its sender is 0. Its
// rows also say is_outgoing, which the groups' rows never carry.

// privateSender is the sender column of a private message: sender_id,
// else the logging account for an outgoing one. self is an id the
// daemon reported, never input, so it is written into the statement.
func privateSender(self int64) string {
	if self <= 0 {
		return "m.sender_id"
	}
	return "COALESCE(m.sender_id, IF(m.is_outgoing <> 0, " + strconv.FormatInt(self, 10) + ", NULL))"
}

func newPrivateSource(self int64) *msgSource {
	sender := privateSender(self)
	return newSource(msgSource{
		table:     "telegram_private_messages",
		edits:     "telegram_private_message_edits",
		editKey:   "private_message_id",
		sender:    sender,
		channel:   "0",
		chatJoin:  "LEFT JOIN telegram_users g ON g.id = m.chat_id ",
		chatTitle: "TRIM(CONCAT(COALESCE(g.first_name,''), ' ', COALESCE(g.last_name,'')))",
		outgoing:  "m.is_outgoing",
		schema: respell(msgSchema, strings.NewReplacer(
			"m.sender_user_id", sender,
			"m.is_channel_post", "0")),
	})
}

// privateSources keeps the private source built for the logging
// account's id, which changes at most once (unknown, then known).
type privateSources struct {
	mu   sync.Mutex
	self int64
	src  *msgSource
}

// privateSource is the private messages' source for the logging
// account as now known.
func (t *telegram) privateSource() *msgSource {
	var self int64
	if t.self != nil {
		self = t.self()
	}
	p := &t.private
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.src == nil || p.self != self {
		p.self, p.src = self, newPrivateSource(self)
	}
	return p.src
}

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
