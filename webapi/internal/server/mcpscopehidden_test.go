// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"encoding/json"
	"fmt"
	"strings"
	"testing"
)

// hiddenScope is token id's scope entries in the token list as role
// sees it, each "kind id name active reason" (null for a nil active or
// reason), and whether the list says the token reads nothing: every
// entry's active false.
func (e *adminEnv) hiddenScope(role string, id int) (string, bool) {
	e.t.Helper()
	st, pg := e.page(role, "/mcp-admin/tokens")
	if st != 200 {
		e.t.Fatalf("%s tokens page %d %v", role, st, pg)
	}
	var p struct {
		Tokens []struct {
			ID    int `json:"id"`
			Scope []struct {
				Kind   string  `json:"kind"`
				ID     int64   `json:"id"`
				Name   string  `json:"name"`
				Active *bool   `json:"active"`
				Reason *string `json:"inactive_reason"`
			} `json:"scope"`
		} `json:"tokens"`
	}
	raw, _ := json.Marshal(pg["props"])
	if err := json.Unmarshal(raw, &p); err != nil {
		e.t.Fatal(err)
	}
	for _, tk := range p.Tokens {
		if tk.ID != id {
			continue
		}
		var out []string
		nothing := len(tk.Scope) > 0
		for _, x := range tk.Scope {
			act, r := "null", "null"
			if x.Active != nil {
				act = fmt.Sprint(*x.Active)
			}
			if x.Reason != nil {
				r = *x.Reason
			}
			nothing = nothing && x.Active != nil && !*x.Active
			out = append(out, fmt.Sprintf("%s %d %q %s %s", x.Kind, x.ID, x.Name, act, r))
		}
		return strings.Join(out, ", "), nothing
	}
	e.t.Fatalf("%s does not list token %d", role, id)
	return "", false
}

// A member's token holding groups hidden from the admin that views it
// tells the admin nothing about them (decisions/
// 2026-11-22-mcp-scope-hidden-entries.md): an exposed and an unexposed
// hidden group look the same, and so does a federation that has groups
// and none the admin sees, whatever their exposure; a superadmin sees
// the real state. adminEnv's sample exposes GNU/Weeb (-1001000000001)
// and CTF Practice (-1001000000010), not Linux Kernel Indonesia
// (-1001000000002); the admin is account 3, the member account 5.
func TestMcpTokenListHiddenFromViewer(t *testing.T) {
	t.Parallel()
	e := newAdminEnv(t)
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
		"(0, 5), (60, 5), (61, 5), (63, 5)")
	exec("INSERT INTO web_member_grant_groups (federation_id, web_user_id, group_id) VALUES "+
		"(60, 5, ?), (61, 5, ?), (63, 5, ?)", gnuweeb, lki, ctf)
	exec("INSERT INTO web_admin_hidden_groups (web_user_id, group_id) VALUES (3, ?), (3, ?)",
		gnuweeb, lki)
	// Token 90 holds the hidden groups, a visible one and the
	// federations; 91 only a hidden exposed group; 92 a hidden group
	// and a visible one; 93 Default.
	for id, scope := range map[int][][2]any{
		90: {{"group", gnuweeb}, {"group", lki}, {"group", ctf}, {"federation", 60},
			{"federation", 61}, {"federation", 62}, {"federation", 63}},
		91: {{"group", gnuweeb}},
		92: {{"group", gnuweeb}, {"group", ctf}},
		93: {{"federation", 0}},
	} {
		exec("INSERT INTO web_mcp_tokens (id, web_user_id, name, token_sha256) VALUES "+
			"(?, 5, ?, UNHEX(SHA2(?, 256)))", id, fmt.Sprint("t", id), fmt.Sprint("tok", id))
		exec("INSERT INTO web_resource_relations (resource_type, resource_id, relation, user_id) "+
			"VALUES ('mcp_token', ?, 'owner', 5)", id)
		for _, en := range scope {
			exec("INSERT INTO web_mcp_token_scopes (token_id, kind, target_id) VALUES (?, ?, ?)",
				id, en[0], en[1])
		}
	}
	check := func(what, role string, id int, want string, wantNothing bool) {
		t.Helper()
		got, nothing := e.hiddenScope(role, id)
		if got != want {
			t.Errorf("%s, token %d as %s:\n got %s\nwant %s", what, id, role, got, want)
		}
		if nothing != wantNothing {
			t.Errorf("%s, token %d as %s: reads nothing %v, want %v", what, id, role, nothing, wantNothing)
		}
	}
	defName := func() string {
		t.Helper()
		var n string
		if err := e.app.QueryRow("SELECT name FROM web_federations WHERE id = 0").Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}()

	// The admin: both hidden groups, exposed or not, and the
	// federations whose only group is hidden (exposed or not) tell
	// nothing. Mixed holds a group the admin sees, and Empty no group
	// at all: both show their real state.
	adminView := func(ctfState string) string {
		return `federation 60 "" null hidden_from_viewer, ` +
			`federation 61 "" null hidden_from_viewer, ` +
			`federation 62 "Empty" false outside_owner_grants, ` +
			`federation 63 "Mixed" true null, ` +
			`group -1001000000010 "CTF Practice" ` + ctfState + `, ` +
			`group -1001000000002 "" null hidden_from_viewer, ` +
			`group -1001000000001 "" null hidden_from_viewer`
	}
	check("start", "admin", 90, adminView("true null"), false)
	check("start", "superadmin", 90, `federation 60 "GNU only" true null, `+
		`federation 61 "LKI only" true null, federation 62 "Empty" false outside_owner_grants, `+
		`federation 63 "Mixed" true null, group -1001000000010 "CTF Practice" true null, `+
		`group -1001000000002 "Linux Kernel Indonesia" false not_exposed, `+
		`group -1001000000001 "GNU/Weeb" true null`, false)
	// Only hidden entries: the admin cannot tell it reads nothing, nor
	// that it reads something.
	check("start", "admin", 91, `group -1001000000001 "" null hidden_from_viewer`, false)
	check("start", "superadmin", 91, `group -1001000000001 "GNU/Weeb" true null`, false)
	// Default: the member sees no group in it (its grants are all in
	// other federations), the admin does.
	defReal := fmt.Sprintf(`federation 0 %q false outside_owner_grants`, defName)
	check("start", "member", 93, `federation 0 "" null hidden_from_viewer`, false)
	check("start", "admin", 93, defReal, true)
	check("start", "superadmin", 93, defReal, true)

	// GNU/Weeb withdrawn from MCP and CTF Practice's grant withdrawn:
	// nothing changes for the admin on GNU/Weeb, nor on GNU only, now
	// reaching no exposed group; the superadmin sees token 91 read
	// nothing now.
	e.want(204, "superadmin", "DELETE", fmt.Sprintf("/api/mcp/groups/%d", gnuweeb), nil)
	exec("DELETE FROM web_member_grant_groups WHERE web_user_id = 5 AND group_id = ?", ctf)
	check("withdrawn", "admin", 90, adminView("false outside_owner_grants"), false)
	check("withdrawn", "superadmin", 90, `federation 60 "GNU only" true null, `+
		`federation 61 "LKI only" true null, federation 62 "Empty" false outside_owner_grants, `+
		`federation 63 "Mixed" true null, `+
		`group -1001000000010 "CTF Practice" false outside_owner_grants, `+
		`group -1001000000002 "Linux Kernel Indonesia" false not_exposed, `+
		`group -1001000000001 "GNU/Weeb" false not_exposed`, false)
	check("withdrawn", "admin", 91, `group -1001000000001 "" null hidden_from_viewer`, false)
	check("withdrawn", "superadmin", 91, `group -1001000000001 "GNU/Weeb" false not_exposed`, true)
	// One visible inactive entry beside a hidden one: still not
	// "reads nothing" to the admin, though it is to the superadmin.
	check("withdrawn", "admin", 92, `group -1001000000010 "CTF Practice" false outside_owner_grants, `+
		`group -1001000000001 "" null hidden_from_viewer`, false)
	check("withdrawn", "superadmin", 92, `group -1001000000010 "CTF Practice" false outside_owner_grants, `+
		`group -1001000000001 "GNU/Weeb" false not_exposed`, true)

	// A deleted federation is not hidden: it says so. Deleting LKI
	// only also takes the member's grant of Linux Kernel Indonesia
	// there, which was what Mixed still reached.
	exec("DELETE FROM web_federations WHERE id = 61")
	check("deleted", "admin", 90, strings.NewReplacer(
		`federation 61 "" null hidden_from_viewer`, `federation 61 "" false federation_deleted`,
		`"Mixed" true null`, `"Mixed" false outside_owner_grants`).
		Replace(adminView("false outside_owner_grants")), false)

	// Shown to the admin again: the real state at once.
	exec("DELETE FROM web_admin_hidden_groups WHERE web_user_id = 3")
	check("shown", "admin", 91, `group -1001000000001 "GNU/Weeb" false not_exposed`, true)
	check("shown", "admin", 90, `federation 60 "GNU only" true null, `+
		`federation 61 "" false federation_deleted, federation 62 "Empty" false outside_owner_grants, `+
		`federation 63 "Mixed" false outside_owner_grants, `+
		`group -1001000000010 "CTF Practice" false outside_owner_grants, `+
		`group -1001000000002 "Linux Kernel Indonesia" false not_exposed, `+
		`group -1001000000001 "GNU/Weeb" false not_exposed`, false)
}
