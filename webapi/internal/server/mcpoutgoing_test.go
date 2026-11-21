// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"bytes"
	"encoding/json"
	"fmt"
	"testing"
	"time"

	"tgloggerd/webapi/internal/daemon"
)

// The messages the logging account sent in a private chat, through
// every tool that reads one: they name the logging account as their
// sender and say is_outgoing, which group answers never carry. On the
// "mcp" fixture the logging account is Rina Hartono (9300000001) and
// the peer Tono Saputra (9300000002).

const (
	mcpSelf = int64(9300000001)
	mcpTono = int64(9300000002)
)

// newMCPEnvSelf is an MCP env whose logging account is known, as the
// daemon's status would report it.
func newMCPEnvSelf(t *testing.T, self int64) *mcpEnv {
	t.Helper()
	return newMCPEnv(t, MCPConfig{}, func(d *Deps) {
		dc, err := daemon.New("http://127.0.0.1:1", testKey, time.Minute)
		if err != nil {
			t.Fatal(err)
		}
		lg := newLoggerAccount(d.Log, dc, time.Now)
		lg.known = self
		d.logger = lg
	})
}

// privateChat gives the live token Tono's chat with one message each
// way: 1<<20 from Tono, 2<<20 from the logging account (sender_id
// NULL), both edited once.
func (e *mcpEnv) privateChat() {
	e.t.Helper()
	e.lgExec("INSERT INTO telegram_private_messages (id, chat_id, message_id, sender_id, is_outgoing, " +
		"date, edit_date, content_type, text) VALUES " +
		"(9001, 9300000002, 1 << 20, 9300000002, 0, 1794700111, 1794700500, 'text', " +
		" 'are the scheduler patches merged'), " +
		"(9002, 9300000002, 2 << 20, NULL, 1, 1794700222, 1794700999, 'text', " +
		" 'the scheduler patches went in today')")
	e.lgExec("INSERT INTO telegram_private_message_edits (private_message_id, content_type, text, " +
		"edit_date, created_at) VALUES (9001, 'text', 'scheduler?', 0, '2026-11-15 01:00:00'), " +
		"(9002, 'text', 'merged', 0, '2026-11-15 02:00:00')")
	e.setScope("private", mcpTono)
}

// byMessage is each message of an answer's list by message_id.
func byMessage(t *testing.T, ms any) map[int64]map[string]any {
	t.Helper()
	out := map[int64]map[string]any{}
	list, _ := ms.([]any)
	for _, m := range list {
		j := m.(map[string]any)
		out[int64(j["message_id"].(float64))] = j
	}
	if len(out) != 2 {
		t.Fatalf("want both messages: %v", ms)
	}
	return out
}

func TestMCPPrivateOutgoing(t *testing.T) {
	t.Parallel()
	e := newMCPEnvSelf(t, mcpSelf)
	e.privateChat()
	in, out := int64(1)<<20, int64(2)<<20

	senderOf := func(j map[string]any) string {
		return fmt.Sprintf("%v %v %v", j["sender_user_id"], j["sender_name"], j["is_outgoing"])
	}
	wantIn := fmt.Sprintf("%v Tono Saputra false", float64(mcpTono))
	wantOut := fmt.Sprintf("%v Rina Hartono true", float64(mcpSelf))
	check := func(what string, ms map[int64]map[string]any) {
		t.Helper()
		if got := senderOf(ms[in]); got != wantIn {
			t.Errorf("%s: incoming %s, want %s", what, got, wantIn)
		}
		if got := senderOf(ms[out]); got != wantOut || ms[out]["sender_username"] != "rina_h" {
			t.Errorf("%s: outgoing %s %v, want %s rina_h", what, got, ms[out]["sender_username"], wantOut)
		}
	}
	check("search", byMessage(t, sc(e.call("telegram_search_messages",
		map[string]any{"group_id": mcpTono}))["messages"]))
	check("recent", byMessage(t, sc(e.call("telegram_list_recent_messages",
		map[string]any{"group_id": mcpTono}))["messages"]))
	check("get_messages", byMessage(t, sc(e.call("telegram_get_messages",
		map[string]any{"group_id": mcpTono, "message_ids": []any{in, out}, "include_history": true}))["found"]))

	// compact keeps is_outgoing only when true; fields may name it.
	ms := byMessage(t, sc(e.call("telegram_search_messages",
		map[string]any{"group_id": mcpTono, "compact": true}))["messages"])
	if _, ok := ms[in]["is_outgoing"]; ok || ms[out]["is_outgoing"] != true {
		t.Errorf("compact: %v / %v", ms[in], ms[out])
	}
	ms = byMessage(t, sc(e.call("telegram_search_messages",
		map[string]any{"group_id": mcpTono, "fields": []any{"text", "is_outgoing"}}))["messages"])
	if ms[in]["is_outgoing"] != false || ms[out]["is_outgoing"] != true || ms[out]["sender_user_id"] != nil {
		t.Errorf("fields: %v / %v", ms[in], ms[out])
	}
	ms = byMessage(t, sc(e.call("telegram_search_messages",
		map[string]any{"group_id": mcpTono, "fields": []any{"text"}}))["messages"])
	if _, ok := ms[out]["is_outgoing"]; ok {
		t.Errorf("fields without is_outgoing: %v", ms[out])
	}

	for mid, want := range map[int64]string{
		in:  fmt.Sprintf("%v false", float64(mcpTono)),
		out: fmt.Sprintf("%v true", float64(mcpSelf)),
	} {
		h := sc(e.call("telegram_get_message_history", map[string]any{"group_id": mcpTono, "message_id": mid}))
		if got := fmt.Sprintf("%v %v", h["sender_user_id"], h["is_outgoing"]); got != want {
			t.Errorf("history of %d: %s, want %s", mid, got, want)
		}
	}
	if w := sc(e.call("telegram_popular_words", map[string]any{"group_id": mcpTono, "start_date": 0})); w["messages_scanned"] != float64(2) {
		t.Errorf("popular words %v", w)
	}

	s := sc(e.call("telegram_list_group_senders", map[string]any{"group_id": mcpTono, "include_total": true}))
	senders := map[float64]float64{}
	for _, x := range s["senders"].([]any) {
		j := x.(map[string]any)
		senders[j["user_id"].(float64)] = j["messages_in_group"].(float64)
	}
	if len(senders) != 2 || senders[float64(mcpSelf)] != 1 || senders[float64(mcpTono)] != 1 ||
		s["total_senders"] != float64(2) {
		t.Errorf("senders %v", s)
	}
	for _, u := range []int64{mcpSelf, mcpTono} {
		c := sc(e.call("telegram_count_user_messages", map[string]any{"group_id": mcpTono, "user_id": u}))
		if c["message_count"] != float64(1) {
			t.Errorf("count of %d: %v", u, c)
		}
	}
	if n := e.count("telegram_search_messages", map[string]any{"group_id": mcpTono,
		"filter": map[string]any{"field": "sender_user_id", "op": "=", "value": mcpSelf}}); n != float64(1) {
		t.Errorf("filter on the logging account: %v", n)
	}

	// Group messages never say is_outgoing, the logging account's own
	// included.
	for _, c := range []struct {
		tool string
		args map[string]any
	}{
		{"telegram_search_messages", map[string]any{"group_id": mcpG1}},
		{"telegram_list_recent_messages", map[string]any{"limit": 200}},
		{"telegram_get_messages", map[string]any{"group_id": mcpG1, "message_ids": []any{int64(101) << 20}}},
		{"telegram_get_message_history", map[string]any{"group_id": mcpG1, "message_id": int64(101) << 20}},
	} {
		b, _ := json.Marshal(e.call(c.tool, c.args))
		if bytes.Contains(b, []byte("is_outgoing")) {
			t.Errorf("%s %v: %s", c.tool, c.args, b)
		}
	}
}

// Before the daemon has said which account it logs, an outgoing
// message still says is_outgoing, with sender 0 and no name.
func TestMCPPrivateOutgoingSelfUnknown(t *testing.T) {
	t.Parallel()
	e := newMCPEnv(t, MCPConfig{})
	e.privateChat()
	ms := byMessage(t, sc(e.call("telegram_search_messages",
		map[string]any{"group_id": mcpTono}))["messages"])
	if j := ms[int64(2)<<20]; j["sender_user_id"] != float64(0) || j["sender_name"] != "" || j["is_outgoing"] != true {
		t.Errorf("outgoing %v", j)
	}
	if j := ms[int64(1)<<20]; j["sender_user_id"] != float64(mcpTono) || j["is_outgoing"] != false {
		t.Errorf("incoming %v", j)
	}
	if n := e.count("telegram_list_group_senders", map[string]any{"group_id": mcpTono}); n != float64(1) {
		t.Errorf("senders %v", n)
	}
}
