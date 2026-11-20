// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"net/url"
	"testing"
)

// The pictures the files list gives a video that is no message's own
// file but plays a role in one, on the "role_stills" fixture (its
// header lists each video): the poster the chat shows it with, read
// only from stored files of messages the viewer sees.

// roleStillRows are the files page's rows of the fixture's videos, by
// id, as role sees them.
func roleStillRows(t *testing.T, e *adminEnv, role string) map[uint64]map[string]any {
	t.Helper()
	q := url.Values{"f": {`[{"field":"id","op":">","value":"897000"},` +
		`{"field":"id","op":"<","value":"897011"}]`}, "per": {"100"}}
	st, b := e.page(role, "/files?"+q.Encode())
	if st != 200 {
		t.Fatalf("files page: %d %v", st, b)
	}
	out := map[uint64]map[string]any{}
	for _, r := range b["props"].(map[string]any)["rows"].([]any) {
		r := r.(map[string]any)
		out[uint64(r["id"].(float64))] = r
	}
	return out
}

func TestRoleVideoStillsInFilesList(t *testing.T) {
	t.Parallel()
	e := newAdminEnv(t, "role_stills")
	rows := roleStillRows(t, e, "admin")
	if len(rows) != 10 {
		t.Fatalf("%d rows: %v", len(rows), rows)
	}
	for _, c := range []struct {
		id    uint64
		thumb uint64 // 0: null
	}{
		{897001, 897101}, // a rich block's thumbnail at its position
		{897002, 0},      // that thumbnail not stored
		{897003, 897103}, // an animated chat photo's still photo
		{897004, 897104}, // a video preview's cover before its photo
		{897005, 897106}, // an animation preview's photo
		{897006, 0},      // an album's next item is no cover
		{897007, 897108}, // its own message's thumbnail first
		{897008, 0},      // a paid video has none
		{897009, 897110}, // from another group's message
		{897010, 897111}, // from a private message
	} {
		r := rows[c.id]
		if c.thumb == 0 {
			if r["thumb"] != nil {
				t.Errorf("%d: thumb %v, want null", c.id, r["thumb"])
			}
		} else if strOf(r["thumb"]) != stillURL(c.thumb) {
			t.Errorf("%d: thumb %v, want %s", c.id, r["thumb"], stillURL(c.thumb))
		}
		if r["thumb"] != nil && r["thumb"] == r["url"] {
			t.Errorf("%d: thumb is the video's own URL", c.id)
		}
	}

	// A member granted Role Stills only gets the pictures of its
	// messages: not Hidden Stills' thumbnail, nor the private one
	// (whose video it does not see at all).
	if _, err := e.app.Exec("INSERT INTO web_member_grant_groups (web_user_id, federation_id, group_id) "+
		"VALUES (?, 0, ?)", e.uids["member"], int64(-1001000000090)); err != nil {
		t.Fatal(err)
	}
	mem := roleStillRows(t, e, "member")
	if _, ok := mem[897010]; ok || len(mem) != 9 {
		t.Fatalf("member sees %d rows: %v", len(mem), mem)
	}
	if r := mem[897009]; r["thumb"] != nil {
		t.Errorf("member: 897009 thumb %v, want null", r["thumb"])
	}
	if r := mem[897001]; strOf(r["thumb"]) != stillURL(897101) {
		t.Errorf("member: 897001 thumb %v", r["thumb"])
	}
}
