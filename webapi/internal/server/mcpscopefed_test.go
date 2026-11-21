// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"encoding/json"
	"fmt"
	"slices"
	"strings"
	"testing"
)

// fedOptions is the federation ids the token form offers role,
// ascending (the form lists them by name).
func (e *adminEnv) fedOptions(role string) string {
	e.t.Helper()
	st, pg := e.page(role, "/mcp-admin/tokens")
	if st != 200 {
		e.t.Fatalf("%s tokens page %d %v", role, st, pg)
	}
	var p struct {
		Options struct {
			Federations []struct {
				ID uint64 `json:"id"`
			} `json:"federations"`
		} `json:"scope_options"`
	}
	raw, _ := json.Marshal(pg["props"])
	if err := json.Unmarshal(raw, &p); err != nil {
		e.t.Fatal(err)
	}
	var ids []uint64
	for _, f := range p.Options.Federations {
		ids = append(ids, f.ID)
	}
	slices.Sort(ids)
	return strings.Trim(fmt.Sprint(ids), "[]")
}

// fedSample adds federations 60 (GNU/Weeb only), 61 (Linux Kernel
// Indonesia only, not exposed), 62 (no group) and 63 (Linux Kernel
// Indonesia and CTF Practice), hides GNU/Weeb and Linux Kernel
// Indonesia from the admin (account 3), and grants the member (account
// 5, with mcp_tokens) GNU/Weeb in 60 and CTF Practice in 63. It
// returns an exec on the web database.
func (e *adminEnv) fedSample() func(q string, args ...any) {
	t := e.t
	t.Helper()
	const gnuweeb, lki, ctf = int64(-1001000000001), int64(-1001000000002), int64(-1001000000010)
	exec := func(q string, args ...any) {
		t.Helper()
		if _, err := e.app.Exec(q, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec("INSERT INTO web_member_abilities (web_user_id, ability) VALUES (5, 'mcp_tokens')")
	exec("INSERT INTO web_federations (id, name) VALUES (60, 'GNU only'), (61, 'LKI only'), " +
		"(62, 'Empty'), (63, 'Mixed')")
	exec("INSERT INTO web_federation_groups (federation_id, group_id) VALUES "+
		"(60, ?), (61, ?), (63, ?), (63, ?)", gnuweeb, lki, lki, ctf)
	exec("INSERT IGNORE INTO web_federation_members (federation_id, web_user_id) VALUES " +
		"(60, 5), (63, 5)")
	exec("INSERT INTO web_member_grant_groups (federation_id, web_user_id, group_id) VALUES "+
		"(60, 5, ?), (63, 5, ?)", gnuweeb, ctf)
	exec("INSERT INTO web_admin_hidden_groups (web_user_id, group_id) VALUES (3, ?), (3, ?)",
		gnuweeb, lki)
	return exec
}

// The token form offers a viewer only the federations it may be told
// about, by the rule of the token list's hidden entries
// (decisions/2026-11-22-mcp-scope-federation-options.md): one with
// groups, exposed or not, none of which the viewer sees is left out;
// one with no group stays; Default's groups are those in no other
// federation. A superadmin is offered every federation.
func TestMcpScopeFederationOptions(t *testing.T) {
	t.Parallel()
	e := newAdminEnv(t)
	exec := e.fedSample()
	check := func(what, role, want string) {
		t.Helper()
		if got := e.fedOptions(role); got != want {
			t.Errorf("%s, %s offered %q, want %q", what, role, got, want)
		}
	}
	// The admin: GNU only (its exposed group hidden) and LKI only (its
	// unexposed group hidden) are left out; Mixed holds CTF Practice,
	// Empty no group, Default groups the admin sees.
	check("start", "admin", "0 62 63")
	check("start", "superadmin", "0 60 61 62 63")
	// The member's grants are all in other federations: Default is
	// left out until it is granted a group in no federation.
	check("start", "member", "60 62 63")
	exec("INSERT INTO web_member_grant_groups (federation_id, web_user_id, group_id) VALUES "+
		"(0, 5, ?)", int64(-1001000000003))
	check("granted in Default", "member", "0 60 62 63")

	// Every group in no federation hidden from the admin too: Default
	// is left out.
	rows, err := e.ro.Query("SELECT id FROM telegram_groups WHERE id NOT IN (?, ?, ?)",
		int64(-1001000000001), int64(-1001000000002), int64(-1001000000010))
	if err != nil {
		t.Fatal(err)
	}
	var rest []int64
	for rows.Next() {
		var g int64
		if err := rows.Scan(&g); err != nil {
			t.Fatal(err)
		}
		rest = append(rest, g)
	}
	if err := rows.Close(); err != nil || len(rest) == 0 {
		t.Fatalf("groups in no federation: %v %v", rest, err)
	}
	for _, g := range rest {
		exec("INSERT INTO web_admin_hidden_groups (web_user_id, group_id) VALUES (3, ?)", g)
	}
	check("Default hidden", "admin", "62 63")
	check("Default hidden", "superadmin", "0 60 61 62 63")

	// Nothing hidden from the admin any more: every federation.
	exec("DELETE FROM web_admin_hidden_groups WHERE web_user_id = 3")
	check("shown", "admin", "0 60 61 62 63")
}
