// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"bytes"
	"encoding/json"
	"testing"
)

// The scope of an MCP token at /mcp (decisions/2026-11-20-mcp-token-
// scope.md), on the "mcp" fixture: Kernel Hackers ID (mcpG1) and Rust
// Belajar (mcpG2) are exposed, Secret Ops (mcpG3) and Keluarga are not.
// The live token (1) is the superadmin admin's.

const mcpG2 = int64(-1002000000002)

// sc is a tool answer's structuredContent, nil for an error.
func sc(r map[string]any) map[string]any {
	if r["isError"] == true {
		return nil
	}
	m, _ := r["structuredContent"].(map[string]any)
	return m
}

func (e *mcpEnv) webExec(q string, args ...any) {
	e.t.Helper()
	if _, err := e.web.Exec(q, args...); err != nil {
		e.t.Fatal(err)
	}
}

func (e *mcpEnv) lgExec(q string, args ...any) {
	e.t.Helper()
	if _, err := e.lg.Exec(q, args...); err != nil {
		e.t.Fatal(err)
	}
}

// setScope replaces the live token's scope: kind, id, kind, id, ...
func (e *mcpEnv) setScope(kv ...any) {
	e.t.Helper()
	e.webExec("DELETE FROM web_mcp_token_scopes WHERE token_id = 1")
	for i := 0; i < len(kv); i += 2 {
		e.webExec("INSERT INTO web_mcp_token_scopes (token_id, kind, target_id) VALUES (1, ?, ?)",
			kv[i], kv[i+1])
	}
}

func (e *mcpEnv) count(tool string, args map[string]any) any {
	e.t.Helper()
	return sc(e.call(tool, args))["count"]
}

// groupIDs is the group_id of every message of a search or recent
// answer.
func groupIDs(r map[string]any) map[float64]int {
	out := map[float64]int{}
	ms, _ := sc(r)["messages"].([]any)
	for _, m := range ms {
		g, _ := m.(map[string]any)["group_id"].(float64)
		out[g]++
	}
	return out
}

// A token scoped to one federation reads only that federation's
// exposed groups, through every tool; a group in the federation that
// is not exposed stays out of reach.
func TestMCPScopeFederation(t *testing.T) {
	t.Parallel()
	e := newMCPEnv(t, MCPConfig{})
	e.webExec("INSERT INTO web_federations (id, name) VALUES (50, 'Rust only')")
	e.webExec("INSERT INTO web_federation_groups (federation_id, group_id) VALUES (50, ?), (50, ?)",
		mcpG2, mcpG3)
	e.setScope("federation", 50)

	// What only Kernel Hackers ID (outside the scope) and Secret Ops
	// (in the federation, not exposed) hold: titles, usernames,
	// messages, edits, previews, admins, history.
	secrets := []string{"Kernel Hackers", "kernelhackers_id", "Kernel panic", "io_uring",
		"patch v2", "Meetup notes", "Founder", "lore.kernel.org", "Build #4",
		"Secret Ops", "secretops", "secret kernel plan", "kernel kernel kernel", "Boss"}
	mids := []any{int64(101) << 20, int64(103) << 20, int64(301) << 20}
	for _, g := range []int64{mcpG1, mcpG3} {
		for _, c := range []struct {
			tool string
			args map[string]any
		}{
			{"telegram_list_groups", map[string]any{"limit": 200}},
			{"telegram_get_group", map[string]any{"group_ids": []any{g, mcpG2}}},
			{"telegram_get_group_history", map[string]any{"group_id": g}},
			{"telegram_list_group_admins", map[string]any{"group_id": g}},
			{"telegram_list_recent_messages", map[string]any{"limit": 200}},
			{"telegram_list_recent_messages", map[string]any{"group_id": g}},
			{"telegram_list_group_senders", map[string]any{"group_id": g, "include_total": true}},
			{"telegram_count_user_messages", map[string]any{"group_id": g, "user_id": 9300000001}},
			{"telegram_popular_words", map[string]any{"group_id": g, "start_date": 0, "include_stopwords": true}},
			{"telegram_search_messages", map[string]any{"limit": 200, "include_total": true}},
			{"telegram_search_messages", map[string]any{"group_id": g, "include_total": true}},
			{"telegram_search_messages", map[string]any{"filter": map[string]any{
				"field": "group_id", "op": "in", "value": []any{mcpG1, mcpG3}}, "include_total": true}},
			{"telegram_search_messages", map[string]any{"order_by": "edit_date", "limit": 200,
				"filter": map[string]any{"field": "edited", "op": "is_not_null"}}},
			{"telegram_get_messages", map[string]any{"group_id": g, "message_ids": mids, "include_history": true}},
			{"telegram_get_message_history", map[string]any{"group_id": g, "message_id": mids[1]}},
		} {
			r := e.call(c.tool, c.args)
			b, _ := json.Marshal(r)
			for _, s := range secrets {
				if bytes.Contains(b, []byte(s)) {
					t.Errorf("%s %v answers %q: %s", c.tool, c.args, s, b)
				}
			}
		}
	}

	// The federation's exposed group is answered.
	l := sc(e.call("telegram_list_groups", map[string]any{}))
	if l["count"] != float64(1) || l["groups"].([]any)[0].(map[string]any)["group_id"] != float64(mcpG2) {
		t.Errorf("list_groups %v", l)
	}
	if _, ok := l["private_chats"]; ok {
		t.Errorf("list_groups names private chats: %v", l)
	}
	for _, tool := range []string{"telegram_search_messages", "telegram_list_recent_messages"} {
		if g := groupIDs(e.call(tool, map[string]any{"limit": 200})); len(g) != 1 || g[float64(mcpG2)] != 3 {
			t.Errorf("%s: messages by group %v", tool, g)
		}
	}
	if r := sc(e.call("telegram_get_group", map[string]any{"group_id": mcpG2})); r == nil {
		t.Error("get_group of the federation's group")
	}
	if r := sc(e.call("telegram_count_user_messages", map[string]any{"group_id": mcpG1,
		"user_id": 9300000001})); r["message_count"] != float64(0) ||
		r["note"] != "That group is outside this token's scope, so the count is zero regardless "+
			"of what the user posted. See telegram_list_groups." {
		t.Errorf("count outside the scope %v", r)
	}

	// The federation's membership is read on every request.
	e.webExec("INSERT INTO web_federation_groups (federation_id, group_id) VALUES (50, ?)", mcpG1)
	if n := e.count("telegram_list_groups", map[string]any{}); n != float64(2) {
		t.Errorf("after adding a group to the federation: %v groups", n)
	}
	e.webExec("DELETE FROM web_federation_groups WHERE federation_id = 50 AND group_id = ?", mcpG2)
	if g := groupIDs(e.call("telegram_search_messages", map[string]any{"limit": 200})); len(g) != 1 ||
		g[float64(mcpG1)] != 13 {
		t.Errorf("after moving the federation to Kernel Hackers ID: %v", g)
	}

	// A deleted federation reaches nothing, and the token does not fall
	// back to every exposed group.
	e.webExec("DELETE FROM web_federations WHERE id = 50")
	for tool, args := range map[string]map[string]any{
		"telegram_list_groups":          {},
		"telegram_list_recent_messages": {},
		"telegram_search_messages":      {},
	} {
		if n := e.count(tool, args); n != float64(0) {
			t.Errorf("%s after deleting the federation: %v", tool, n)
		}
	}
}

// Default reaches the exposed groups in no other federation; a group
// entry follows the exposure list, and a withdrawn group's entry
// reaches nothing without widening the token; no entries is every
// exposed group again.
func TestMCPScopeGroupsAndDefault(t *testing.T) {
	t.Parallel()
	e := newMCPEnv(t, MCPConfig{})
	e.webExec("INSERT INTO web_federations (id, name) VALUES (50, 'Rust only')")
	e.webExec("INSERT INTO web_federation_groups (federation_id, group_id) VALUES (50, ?)", mcpG2)

	e.setScope("federation", 0)
	l := sc(e.call("telegram_list_groups", map[string]any{}))
	if l["count"] != float64(1) || l["groups"].([]any)[0].(map[string]any)["group_id"] != float64(mcpG1) {
		t.Errorf("Default: %v", l)
	}

	e.setScope("group", mcpG2, "group", mcpG3)
	if g := groupIDs(e.call("telegram_list_recent_messages", map[string]any{"limit": 200})); len(g) != 1 ||
		g[float64(mcpG2)] != 3 {
		t.Errorf("group entries: %v", g)
	}
	e.lgExec("DELETE FROM telegram_public_groups WHERE group_id = ?", mcpG2)
	for tool, args := range map[string]map[string]any{
		"telegram_list_groups":          {},
		"telegram_list_recent_messages": {},
		"telegram_search_messages":      {},
		"telegram_get_messages":         {"group_id": mcpG2, "message_ids": []any{int64(201) << 20}},
	} {
		if n := e.count(tool, args); n != float64(0) {
			t.Errorf("%s with its only group withdrawn: %v", tool, n)
		}
	}
	if r := e.call("telegram_get_group", map[string]any{"group_id": mcpG2}); r["isError"] != true {
		t.Errorf("get_group of a withdrawn group: %v", r)
	}
	e.lgExec("INSERT INTO telegram_public_groups (group_id, note, added_by) VALUES (?, '', 1)", mcpG2)
	if n := e.count("telegram_list_groups", map[string]any{}); n != float64(1) {
		t.Errorf("exposed again: %v", n)
	}

	e.setScope()
	if n := e.count("telegram_list_groups", map[string]any{}); n != float64(2) {
		t.Errorf("no scope: %v groups, want every exposed one", n)
	}
}

// A private chat: the live token reads it only once it is in its
// scope, while a superadmin owns the token, and through every message
// tool when named by its id; never by searching without a group_id,
// and never another private chat.
func TestMCPScopePrivate(t *testing.T) {
	t.Parallel()
	e := newMCPEnv(t, MCPConfig{})
	const tono, maya = int64(9300000002), int64(9300000005)
	e.lgExec("INSERT INTO telegram_private_messages (id, chat_id, message_id, sender_id, is_outgoing, " +
		"date, edit_date, content_type, text) VALUES " +
		"(9001, 9300000002, 1 << 20, 9300000002, 0, 1794700111, 0, 'text', " +
		" 'a private rumour about the scheduler'), " +
		"(9002, 9300000002, 2 << 20, NULL, 1, 1794700222, 1794700999, 'text', " +
		" 'my answer to the rumour'), " +
		"(9101, 9300000005, 1 << 20, 9300000005, 0, 1794700333, 0, 'text', 'maya whispers')")
	e.lgExec("INSERT INTO telegram_private_message_edits (private_message_id, content_type, text, " +
		"edit_date, created_at) VALUES (9002, 'text', 'first draft answer', 0, '2026-11-15 02:00:00')")

	type call struct {
		tool string
		args map[string]any
	}
	all := []call{
		{"telegram_search_messages", map[string]any{"group_id": tono, "include_total": true}},
		{"telegram_list_recent_messages", map[string]any{"group_id": tono}},
		{"telegram_get_messages", map[string]any{"group_id": tono,
			"message_ids": []any{int64(1) << 20, int64(2) << 20}, "include_history": true}},
		{"telegram_get_message_history", map[string]any{"group_id": tono, "message_id": int64(2) << 20}},
		{"telegram_popular_words", map[string]any{"group_id": tono, "start_date": 0}},
		{"telegram_list_group_senders", map[string]any{"group_id": tono}},
		{"telegram_count_user_messages", map[string]any{"group_id": tono, "user_id": tono}},
	}
	unreadable := func(when string) {
		t.Helper()
		for _, c := range append(all, call{"telegram_list_groups", map[string]any{}},
			call{"telegram_search_messages", map[string]any{"group_id": maya}}) {
			b, _ := json.Marshal(e.call(c.tool, c.args))
			for _, s := range []string{"rumour", "first draft", "maya whispers", "private_chats"} {
				if bytes.Contains(b, []byte(s)) {
					t.Errorf("%s: %s %v answers %q: %s", when, c.tool, c.args, s, b)
				}
			}
		}
	}
	unreadable("no scope")

	e.setScope("private", tono)
	l := sc(e.call("telegram_list_groups", map[string]any{}))
	pcs, _ := l["private_chats"].([]any)
	if l["count"] != float64(0) || len(pcs) != 1 ||
		pcs[0].(map[string]any)["chat_id"] != float64(tono) || pcs[0].(map[string]any)["name"] != "Tono Saputra" {
		t.Errorf("list_groups %v", l)
	}
	s := sc(e.call("telegram_search_messages", map[string]any{"group_id": tono, "include_total": true}))
	if s["count"] != float64(2) || s["total"] != float64(2) {
		t.Errorf("search the chat: %v", s)
	} else if m := s["messages"].([]any)[1].(map[string]any); m["group_title"] != "Tono Saputra" ||
		m["text"] != "a private rumour about the scheduler" || m["sender_user_id"] != float64(tono) {
		t.Errorf("search the chat: oldest %v", m)
	}
	for _, f := range []struct {
		filter map[string]any
		want   float64
	}{
		{map[string]any{"field": "text", "op": "match", "value": "rumour"}, 2},
		{map[string]any{"field": "sender_user_id", "op": "=", "value": tono}, 1},
		{map[string]any{"field": "is_channel_post", "op": "=", "value": false}, 2},
		{map[string]any{"field": "edited", "op": "is_not_null"}, 1},
	} {
		if n := e.count("telegram_search_messages", map[string]any{"group_id": tono, "filter": f.filter}); n != f.want {
			t.Errorf("search the chat by %v: %v, want %v", f.filter, n, f.want)
		}
	}
	if n := e.count("telegram_search_messages", map[string]any{}); n != float64(0) {
		t.Errorf("search without group_id: %v, want groups only (none in scope)", n)
	}
	if n := e.count("telegram_list_recent_messages", map[string]any{"group_id": tono}); n != float64(2) {
		t.Errorf("recent: %v", n)
	}
	g := sc(e.call("telegram_get_messages", all[2].args))
	if g["count"] != float64(2) {
		t.Errorf("get_messages %v", g)
	} else if pv := g["found"].([]any)[1].(map[string]any)["previous_versions"].([]any); len(pv) != 1 ||
		pv[0].(map[string]any)["text"] != "first draft answer" {
		t.Errorf("get_messages history %v", pv)
	}
	if h := sc(e.call("telegram_get_message_history", all[3].args)); h["previous_version_count"] != float64(1) {
		t.Errorf("history %v", h)
	}
	if w := sc(e.call("telegram_popular_words", all[4].args)); w["messages_scanned"] != float64(2) {
		t.Errorf("popular words %v", w)
	}
	if n := e.count("telegram_list_group_senders", all[5].args); n != float64(1) {
		t.Errorf("senders %v", n)
	}
	if c := sc(e.call("telegram_count_user_messages", all[6].args)); c["message_count"] != float64(1) {
		t.Errorf("count %v", c)
	}
	// Another private chat, and the group tools, stay out of reach.
	if b, _ := json.Marshal(e.call("telegram_search_messages", map[string]any{"group_id": maya})); bytes.Contains(b, []byte("maya")) {
		t.Errorf("another private chat: %s", b)
	}
	if r := e.call("telegram_get_group", map[string]any{"group_id": tono}); r["isError"] != true {
		t.Errorf("get_group of a private chat: %v", r)
	}

	// Given to an admin, the entry stays and reaches nothing; back with
	// a superadmin, it works again.
	e.webExec("UPDATE web_resource_relations SET user_id = 3 WHERE resource_type = 'mcp_token' " +
		"AND resource_id = 1")
	unreadable("owned by an admin")
	e.webExec("UPDATE web_resource_relations SET user_id = 2 WHERE resource_type = 'mcp_token' " +
		"AND resource_id = 1")
	if n := e.count("telegram_search_messages", map[string]any{"group_id": tono}); n != float64(2) {
		t.Errorf("owned by another superadmin: %v", n)
	}
	// Without an owner record the token is the superadmins'.
	e.webExec("DELETE FROM web_resource_relations WHERE resource_type = 'mcp_token' AND resource_id = 1")
	if n := e.count("telegram_search_messages", map[string]any{"group_id": tono}); n != float64(2) {
		t.Errorf("without an owner record: %v", n)
	}
}
