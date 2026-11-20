// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"net/url"
	"strings"
	"testing"
)

// The role a file plays in its message on the files list, on the
// "file_roles" fixture (its header lists each file): FileRow.role and
// the message it names, and the role filter, for a viewer who sees
// everything and for a member who sees one of the groups.

const (
	roleLab   = -1001000000060
	roleVault = -1001000000061
)

// roleRows are the files page's rows of the fixture, newest first, as
// role sees them with the extra filter conditions (JSON, "" for none).
func roleRows(t *testing.T, e *adminEnv, role, extra string) []map[string]any {
	t.Helper()
	f := `[{"field":"id","op":">","value":"891000"},{"field":"id","op":"<","value":"891100"}`
	if extra != "" {
		f += "," + extra
	}
	q := url.Values{"f": {f + "]"}, "per": {"100"}}
	st, b := e.page(role, "/files?"+q.Encode())
	if st != 200 {
		t.Fatalf("files page %s: %d %v", extra, st, b)
	}
	props := b["props"].(map[string]any)
	if err, ok := props["error"]; ok && err != nil {
		t.Fatalf("files page %s: %s", extra, js(err))
	}
	var out []map[string]any
	for _, r := range props["rows"].([]any) {
		out = append(out, r.(map[string]any))
	}
	return out
}

func roleIDs(rows []map[string]any) string {
	var ids []string
	for _, r := range rows {
		ids = append(ids, js(r["id"]))
	}
	return strings.Join(ids, ",")
}

func TestFileRoles(t *testing.T) {
	t.Parallel()
	e := newAdminEnv(t, "file_roles")

	rows := roleRows(t, e, "admin", "")
	if got := roleIDs(rows); got != "891010,891009,891008,891007,891006,891005,891004,891003,891002,891001" {
		t.Fatalf("rows %s", got)
	}
	// By id: the role (null for the message's own file) and the
	// message the row names.
	want := map[string]struct{ role, message string }{
		"891001": {`{"message_kind":"text","role":"link_preview"}`, `{"id":5101,"scope":"group"}`},
		"891002": {`null`, `{"id":5102,"scope":"group"}`},
		"891003": {`{"message_kind":"video","role":"cover"}`, `{"id":5102,"scope":"group"}`},
		"891004": {`{"message_kind":"video","role":"thumbnail"}`, `{"id":5102,"scope":"group"}`},
		"891005": {`{"message_kind":"game","role":"game"}`, `{"id":5201,"scope":"private"}`},
		// Its own file in 5103 before a link preview's picture in 5104.
		"891006": {`null`, `{"id":5103,"scope":"group"}`},
		"891007": {`{"message_kind":"video","role":"cover"}`, `{"id":5111,"scope":"group"}`},
		"891008": {`null`, `{"id":5111,"scope":"group"}`},
		"891009": {`{"message_kind":"text","role":"link_preview"}`, `{"id":5202,"scope":"private"}`},
		"891010": {`null`, `{"id":5105,"scope":"group"}`},
	}
	for _, r := range rows {
		id := js(r["id"])
		if _, sent := r["role"]; !sent {
			t.Errorf("%s: no role sent", id)
		}
		if w := want[id]; js(r["role"]) != w.role || js(r["message"]) != w.message {
			t.Errorf("%s: role %s message %s, want %s %s", id, js(r["role"]), js(r["message"]),
				w.role, w.message)
		}
	}
	// The chat and sender are those of the message in the role, and a
	// role's message lends the file no length of its own.
	r := rows[9]
	if js(r["chat"]) != `{"id":-1001000000060,"kind":"group","title":"Role Lab"}` ||
		js(r["sender"]) != `{"id":9299000001,"name":"Dewi Kusuma"}` {
		t.Errorf("891001: chat %s sender %s", js(r["chat"]), js(r["sender"]))
	}
	if r := rows[5]; js(r["chat"]) != `{"id":9299000002,"kind":"user","title":"Bima Pratama"}` ||
		js(r["sender"]) != `{"id":9299000002,"name":"Bima Pratama"}` {
		t.Errorf("891005: chat %s sender %s", js(r["chat"]), js(r["sender"]))
	}
	if r := rows[1]; r["sender"] != nil {
		t.Errorf("891009, outgoing: sender %s", js(r["sender"]))
	}
	if _, ok := rows[7]["duration"]; ok {
		t.Errorf("891003: duration %v", rows[7]["duration"])
	}

	// The preview of one file is the same row.
	q := url.Values{"file": {"891003"}}
	if _, b := e.page("admin", "/files?"+q.Encode()); js(b["props"].(map[string]any)["preview"].(map[string]any)["role"]) !=
		`{"message_kind":"video","role":"cover"}` {
		t.Errorf("preview %v", b["props"].(map[string]any)["preview"])
	}

	// The filter: a file that plays the role in any message, its own
	// file elsewhere or not (891006, 891010).
	for _, c := range []struct{ cond, ids string }{
		{`{"field":"role","op":"=","value":"link_preview"}`, "891009,891006,891001"},
		{`{"field":"role","op":"=","value":"cover"}`, "891007,891003"},
		{`{"field":"role","op":"=","value":"paid"}`, "891010"},
		{`{"field":"role","op":"in","value":["cover","game","link_preview"]}`,
			"891009,891007,891006,891005,891003,891001"},
		{`{"field":"role","op":"!=","value":"cover"}`,
			"891010,891009,891008,891006,891005,891004,891002,891001"},
		{`{"field":"role","op":"=","value":"rich"}`, ""},
	} {
		if got := roleIDs(roleRows(t, e, "admin", c.cond)); got != c.ids {
			t.Errorf("admin %s: %s, want %s", c.cond, got, c.ids)
		}
	}

	// The search API lists the field and refuses a role it does not
	// know.
	b := e.want(200, "admin", "GET", "/api/search/files", nil)
	var field string
	for _, f := range b["fields"].([]any) {
		if f := f.(map[string]any); f["key"] == "role" {
			field = js(f)
		}
	}
	if field != `{"enum":"cover,live_video,paid,preview,poll,game,invoice,chat_photo,thumbnail,`+
		`rich,gift,background,preview_media,link_preview","key":"role","label":"Role in a message",`+
		`"operators":["=","!=","in"],"sortable":false,"type":"enum"}` {
		t.Errorf("role field %s", field)
	}
	sq := url.Values{"search": {`[{"c":"role","o":"=","v":"cover","n":"AND"},` +
		`{"c":"id","o":">","v":"891000"}]`}}
	if b := e.want(200, "admin", "GET", "/api/search/files?"+sq.Encode(), nil); num(b["total"]) != 2 {
		t.Errorf("search cover: %v", b["total"])
	}
	sq = url.Values{"search": {`[{"c":"role","o":"=","v":"main"}]`}}
	if b := e.want(400, "admin", "GET", "/api/search/files?"+sq.Encode(), nil); b["error"] != "bad_search" {
		t.Errorf("bad role: %v", b)
	}

	// A member granted Role Lab sees the files of its messages: their
	// own files and the files playing a role in them, which are seldom
	// any message's own file (891001, 891003, 891004), filtered by role
	// or not, and never one of a message it does not see: 891007's
	// cover, 891008 and 891010's paid role are in Role Vault, 891005's
	// game and 891009's preview in a private chat.
	if _, err := e.app.Exec("INSERT INTO web_member_grant_groups (web_user_id, federation_id, group_id) "+
		"VALUES (?, 0, ?)", e.uids["member"], int64(roleLab)); err != nil {
		t.Fatal(err)
	}
	mem := roleRows(t, e, "member", "")
	if got := roleIDs(mem); got != "891010,891006,891004,891003,891002,891001" {
		t.Errorf("member rows %s", got)
	}
	memRoles := map[string]string{
		"891010": `null`, "891006": `null`, "891002": `null`,
		"891004": `{"message_kind":"video","role":"thumbnail"}`,
		"891003": `{"message_kind":"video","role":"cover"}`,
		"891001": `{"message_kind":"text","role":"link_preview"}`,
	}
	for _, r := range mem {
		if id := js(r["id"]); js(r["role"]) != memRoles[id] {
			t.Errorf("member: %s role %s, want %s", id, js(r["role"]), memRoles[id])
		}
	}
	for _, c := range []struct{ cond, ids string }{
		{`{"field":"role","op":"=","value":"link_preview"}`, "891006,891001"},
		{`{"field":"role","op":"=","value":"cover"}`, "891003"},
		{`{"field":"role","op":"=","value":"thumbnail"}`, "891004"},
		{`{"field":"role","op":"=","value":"paid"}`, ""},
		{`{"field":"role","op":"in","value":["cover","paid","game","link_preview"]}`,
			"891006,891003,891001"},
		{`{"field":"role","op":"!=","value":"paid"}`, "891010,891006,891004,891003,891002,891001"},
		{`{"field":"role","op":"!=","value":"cover"}`, "891010,891006,891004,891002,891001"},
	} {
		if got := roleIDs(roleRows(t, e, "member", c.cond)); got != c.ids {
			t.Errorf("member %s: %s, want %s", c.cond, got, c.ids)
		}
	}

	// The preview of one file (an id = search) agrees with the list.
	for id, want := range map[string]string{
		"891003": `{"message_kind":"video","role":"cover"}`,
		"891001": `{"message_kind":"text","role":"link_preview"}`,
		"891007": "", "891005": "", "891009": "",
	} {
		q := url.Values{"file": {id}}
		_, b := e.page("member", "/files?"+q.Encode())
		p, _ := b["props"].(map[string]any)["preview"].(map[string]any)
		switch {
		case want == "" && p != nil:
			t.Errorf("member preview %s: %v", id, p)
		case want != "" && (p == nil || js(p["role"]) != want):
			t.Errorf("member preview %s: %v, want role %s", id, p, want)
		}
	}

	// The search API agrees, and takes OR (the files page joins its
	// filters with AND): beside an OR the role does not hold for every
	// row, so the scope's own condition stays, which takes the role
	// files of the member's messages, and no branch reaches a file of
	// Role Vault or of a private chat.
	for _, c := range []struct{ search, ids string }{
		{`[{"c":"id","o":">","v":"891000","n":"AND"},{"c":"id","o":"<","v":"891100"}]`,
			"891010,891006,891004,891003,891002,891001"},
		{`[{"c":"role","o":"=","v":"cover","n":"OR"},{"c":"id","o":"=","v":"891007"}]`, "891003"},
		{`[{"c":"id","o":"=","v":"891008","n":"OR"},{"c":"role","o":"=","v":"paid"}]`, ""},
		{`[{"c":"id","o":"=","v":"891004","n":"OR"},{"c":"id","o":"=","v":"891005","n":"OR"},` +
			`{"c":"id","o":"=","v":"891009","n":"OR"},{"c":"id","o":"=","v":"891001"}]`, "891004,891001"},
		{`[{"c":"id","o":"=","v":"891003","n":"OR"},{"c":"id","o":"=","v":"891007"}]`, "891003"},
	} {
		q := url.Values{"search": {c.search}}
		b := e.want(200, "member", "GET", "/api/search/files?"+q.Encode(), nil)
		var ids []string
		for _, r := range b["rows"].([]any) {
			ids = append(ids, js(r.([]any)[1]))
		}
		if got := strings.Join(ids, ","); got != c.ids || num(b["total"]) != int64(len(ids)) {
			t.Errorf("member search %s: %s (total %v), want %s", c.search, got, b["total"], c.ids)
		}
	}

	// An admin who hides Role Vault sees private chats: the role files
	// of private messages (891005, 891009) too, and still none of Role
	// Vault's (891007's cover, 891008).
	if _, err := e.app.Exec("INSERT INTO web_admin_hidden_groups (web_user_id, group_id) "+
		"VALUES (?, ?)", e.uids["admin"], int64(roleVault)); err != nil {
		t.Fatal(err)
	}
	if got := roleIDs(roleRows(t, e, "admin", "")); got !=
		"891010,891009,891006,891005,891004,891003,891002,891001" {
		t.Errorf("admin hiding Role Vault: %s", got)
	}
}
