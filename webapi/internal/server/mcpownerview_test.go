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

// An admin's MCP token reads only the exposed groups the admin sees on
// the web (decisions/2026-11-22-mcp-token-owner-view.md), on the "mcp"
// fixture: Kernel Hackers ID (mcpG1) and Rust Belajar (mcpG2) are
// exposed, and gw_admin (3) is an admin.

// adminToken gives the live token to gw_admin, with the groups given
// hidden from it.
func (e *mcpEnv) adminToken(hidden ...int64) {
	e.t.Helper()
	e.webExec("UPDATE web_resource_relations SET user_id = 3 WHERE resource_type = 'mcp_token' " +
		"AND resource_id = 1")
	for _, g := range hidden {
		e.webExec("INSERT INTO web_admin_hidden_groups (web_user_id, group_id) VALUES (3, ?)", g)
	}
}

func TestMCPAdminTokenWithoutHiddenGroups(t *testing.T) {
	t.Parallel()
	e := newMCPEnv(t, MCPConfig{})
	e.adminToken(mcpG1)

	only := func(when string, want ...int64) {
		t.Helper()
		kernel := slices.Contains(want, mcpG1)
		slices.Sort(want)
		if got := e.listedGroups(); fmt.Sprint(got) != fmt.Sprint(want) {
			t.Errorf("%s: list_groups %v, want %v", when, got, want)
		}
		r := sc(e.call("telegram_count_user_messages", map[string]any{"group_id": mcpG1,
			"user_id": 9300000001}))
		if n := r["message_count"]; (n != float64(0)) != kernel {
			t.Errorf("%s: count in Kernel Hackers ID %v", when, r)
		}
		b, _ := json.Marshal(e.call("telegram_get_messages", map[string]any{"group_id": mcpG1,
			"message_ids": []any{int64(101) << 20}}))
		if strings.Contains(strings.ToLower(string(b)), "kernel panic") != kernel {
			t.Errorf("%s: get_messages in Kernel Hackers ID %s", when, b)
		}
		got := groupIDs(e.call("telegram_search_messages", map[string]any{"limit": 200}))
		if _, in := got[float64(mcpG1)]; in != kernel {
			t.Errorf("%s: search by group %v", when, got)
		}
	}

	// No entries: every exposed group but the hidden one.
	only("no scope", mcpG2)
	// Entries narrow within what the admin sees, never beyond it.
	e.setScope("group", mcpG1)
	only("scoped to the hidden group")
	e.setScope("group", mcpG1, "group", mcpG2)
	only("scoped across the hidden group", mcpG2)
	e.setScope("federation", 0)
	only("scoped to Default", mcpG2)
	// Shown again: it counts at once.
	e.webExec("DELETE FROM web_admin_hidden_groups WHERE web_user_id = 3")
	only("shown again, Default entry", mcpG1, mcpG2)
	e.setScope()
	only("shown again, no scope", mcpG1, mcpG2)
	// Hidden again, with no entries.
	e.webExec("INSERT INTO web_admin_hidden_groups (web_user_id, group_id) VALUES (3, ?)", mcpG1)
	only("hidden again", mcpG2)
	// A superadmin's token is not bounded by another admin's hiding.
	e.webExec("UPDATE web_resource_relations SET user_id = 1 WHERE resource_type = 'mcp_token' " +
		"AND resource_id = 1")
	only("owned by a superadmin", mcpG1, mcpG2)
}

// tokenScope is token id's scope entries in the token list as role
// sees it: "kind id active reason", comma separated.
func (e *adminEnv) tokenScope(role string, id int) string {
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
				Active bool    `json:"active"`
				Reason *string `json:"inactive_reason"`
			} `json:"scope"`
		} `json:"tokens"`
	}
	raw, _ := json.Marshal(pg["props"])
	if err := json.Unmarshal(raw, &p); err != nil {
		e.t.Fatal(err)
	}
	for _, tk := range p.Tokens {
		if tk.ID == id {
			var out []string
			for _, x := range tk.Scope {
				r := "null"
				if x.Reason != nil {
					r = *x.Reason
				}
				out = append(out, fmt.Sprintf("%s %d %v %s", x.Kind, x.ID, x.Active, r))
			}
			return strings.Join(out, ", ")
		}
	}
	e.t.Fatalf("%s does not list token %d", role, id)
	return ""
}

// The token list marks an admin's entries for groups hidden from it
// inactive, outside_owner_grants. adminEnv's sample exposes GNU/Weeb
// (-1001000000001), Rust Programming (-1001000000003) and CTF Practice
// (-1001000000010); the admin is account 3.
func TestMcpTokenListAdminHiddenGroups(t *testing.T) {
	t.Parallel()
	e := newAdminEnv(t)
	const gnuweeb, rust, ctf = int64(-1001000000001), int64(-1001000000003), int64(-1001000000010)
	exec := func(q string, args ...any) {
		t.Helper()
		if _, err := e.app.Exec(q, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec("INSERT INTO web_federations (id, name) VALUES (50, 'Rust only'), (51, 'CTF only'), " +
		"(52, 'Nothing exposed')")
	exec("INSERT INTO web_federation_groups (federation_id, group_id) VALUES (50, ?), (51, ?), "+
		"(52, -1001999999999)", rust, ctf)
	exec("INSERT INTO web_admin_hidden_groups (web_user_id, group_id) VALUES (3, ?), (3, ?)",
		gnuweeb, ctf)
	// Token 90 is the admin's, 91 a superadmin's, with the same scope.
	for id, owner := range map[int]int{90: 3, 91: 1} {
		exec("INSERT INTO web_mcp_tokens (id, web_user_id, name, token_sha256) VALUES "+
			"(?, ?, ?, UNHEX(SHA2(?, 256)))", id, owner, fmt.Sprint("t", id), fmt.Sprint("tok", id))
		exec("INSERT INTO web_resource_relations (resource_type, resource_id, relation, user_id) "+
			"VALUES ('mcp_token', ?, 'owner', ?)", id, owner)
		exec("INSERT INTO web_mcp_token_scopes (token_id, kind, target_id) VALUES "+
			"(?, 'group', ?), (?, 'group', ?), (?, 'group', ?), (?, 'federation', 0), "+
			"(?, 'federation', 50), (?, 'federation', 51), (?, 'federation', 52)",
			id, gnuweeb, id, rust, id, ctf, id, id, id, id)
	}
	const all = "federation 0 true null, federation 50 true null, federation 51 true null, " +
		"federation 52 true null, group -1001000000010 true null, " +
		"group -1001000000003 true null, group -1001000000001 true null"
	if got := e.tokenScope("superadmin", 91); got != all {
		t.Errorf("superadmin's token: %s", got)
	}
	// Default reaches only GNU/Weeb now, hidden; CTF only its hidden
	// group; a federation with no exposed group is not out of reach
	// because of the hiding.
	want := "federation 0 false outside_owner_grants, federation 50 true null, " +
		"federation 51 false outside_owner_grants, federation 52 true null, " +
		"group -1001000000010 false outside_owner_grants, group -1001000000003 true null, " +
		"group -1001000000001 false outside_owner_grants"
	for _, role := range []string{"superadmin", "admin"} {
		if got := e.tokenScope(role, 90); got != want {
			t.Errorf("admin's token as %s: %s\nwant %s", role, got, want)
		}
	}
	// Shown again: the entries count at once.
	exec("DELETE FROM web_admin_hidden_groups WHERE web_user_id = 3")
	if got := e.tokenScope("superadmin", 90); got != all {
		t.Errorf("after showing every group: %s\nwant %s", got, all)
	}
	// An unexposed group says so first, hidden or not.
	exec("INSERT INTO web_admin_hidden_groups (web_user_id, group_id) VALUES (3, ?)", gnuweeb)
	e.want(204, "superadmin", "DELETE", fmt.Sprintf("/api/mcp/groups/%d", gnuweeb), nil)
	if got := e.tokenScope("superadmin", 90); !strings.Contains(got, "group -1001000000001 false not_exposed") {
		t.Errorf("unexposed hidden group: %s", got)
	}
}
