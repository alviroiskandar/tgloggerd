// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package mcp

import (
	"context"
	"slices"
	"strings"
)

// THE EXPOSURE GATE.
//
// Nothing is readable through MCP until an administrator adds the group
// to telegram_public_groups, a curated list; whether a group has a
// public username is never consulted (migrations/000022 says why). A
// token's scope (scope.go) narrows the list further, and is the one way
// to a private chat. Both meet in ONE place, readable: every tool asks
// it what it may read, and nothing else reads the list or the scope.
// Where it is enforced:
//
//   - Every tool that returns MESSAGES goes through msgGate: search,
//     recent, get_messages, message history, popular words. It also
//     picks the table: a private chat in the token's scope, named by
//     its id, is read from the private messages (private.go).
//   - The tools that return a GROUP or a per-group aggregate carry the
//     readable groups as a condition in their own statement
//     (readable.groupCond on the table's group column): list_groups,
//     get_group, get_group_history, list_group_admins,
//     list_group_senders, count_user_messages. The last two count a
//     private chat in scope as well.
//   - The user tools (get_users, get_user, get_user_history) are not
//     gated: they describe accounts, not conversations, as in the
//     Drogon web.
//
// Direct messages are out of reach but through the scope: only
// private.go names their table, which a grep for its name checks
// (mcp_test.go).
//
// The gate binds the allowed ids as constants rather than a semi-join
// on the list. With the subquery MySQL drives the join from the list
// and never picks the (chat_id, <timestamp>) indexes; with constants an
// ordered scan is a reverse range scan of one index (web/docs/mcp.md,
// Performance notes). The list is curated, so it is small. An empty
// list compiles to 1=0, which matches nothing.

type gate struct {
	sql     string // " AND m.chat_id IN (?,?)" or " AND 1=0"
	binds   []any
	nothing bool       // nothing readable: the statement can be skipped
	src     *msgSource // the table to read: the groups', or a private chat's
}

// readable is what this request may read: the exposed groups the
// token's scope allows, in the list's order, and the private chats of
// its scope.
type readable struct {
	groups   []int64
	private  []int64
	exposed  map[int64]bool // the whole list, for telling "not exposed" from "not in scope"
	narrowed bool
}

func (r readable) hasGroup(g int64) bool { return slices.Contains(r.groups, g) }

func (r readable) hasPrivate(c int64) bool { return slices.Contains(r.private, c) }

// groupCond is the readable groups as a condition on col: " AND col IN
// (?,...)", or " AND 1=0" for none.
func (r readable) groupCond(col string) (string, []any) {
	if len(r.groups) == 0 {
		return " AND 1=0", nil
	}
	binds := make([]any, len(r.groups))
	for i, g := range r.groups {
		binds[i] = g
	}
	return " AND " + col + " IN (" + placeholders(len(r.groups), ",") + ")", binds
}

// readable reads the curated list and applies the request's scope.
func (t *telegram) readable(ctx context.Context) (readable, error) {
	rows, err := execTool(ctx, t.db, "SELECT group_id FROM telegram_public_groups", nil, queryFailed)
	if err != nil {
		return readable{}, err
	}
	reach := reachOf(ctx)
	out := readable{exposed: map[int64]bool{}, narrowed: reach.narrowed, private: reach.private}
	for _, r := range rows {
		g := r.i64("group_id")
		out.exposed[g] = true
		if reach.allowsGroup(g) {
			out.groups = append(out.groups, g)
		}
	}
	return out, nil
}

// msgGate builds the gate over m.chat_id, narrowed to args["group_id"]
// when that is an integer. A named group that is not readable gives a
// gate that matches nothing rather than an error: refusing would
// confirm the group exists. A named private chat in the token's scope
// gives a gate over the private messages.
func (t *telegram) msgGate(ctx context.Context, args map[string]any) (gate, error) {
	want, named := intValue(args["group_id"])
	return t.chatGate(ctx, want, named)
}

// chatGate is msgGate for chat want when named, else for every
// readable group.
func (t *telegram) chatGate(ctx context.Context, want int64, named bool) (gate, error) {
	rd, err := t.readable(ctx)
	if err != nil {
		return gate{}, err
	}
	groups := rd.groups
	if named {
		if rd.hasPrivate(want) {
			return gate{sql: " AND m.chat_id IN (?)", binds: []any{want}, src: t.privateSource()}, nil
		}
		groups = nil
		if rd.hasGroup(want) {
			groups = []int64{want}
		}
	}
	if len(groups) == 0 {
		return gate{sql: " AND 1=0", nothing: true, src: groupSource}, nil
	}
	g := gate{sql: " AND m.chat_id IN (" + strings.Repeat("?,", len(groups)-1) + "?)", src: groupSource}
	for _, id := range groups {
		g.binds = append(g.binds, id)
	}
	return g, nil
}
