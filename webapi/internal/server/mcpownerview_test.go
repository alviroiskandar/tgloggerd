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
		if tk.ID == id {
			var out []string
			for _, x := range tk.Scope {
				act, r := "null", "null"
				if x.Active != nil {
					act = fmt.Sprint(*x.Active)
				}
				if x.Reason != nil {
					r = *x.Reason
				}
				out = append(out, fmt.Sprintf("%s %d %s %s", x.Kind, x.ID, act, r))
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
	if got := e.tokenScope("superadmin", 90); got != want {
		t.Errorf("admin's token as superadmin: %s\nwant %s", got, want)
	}
	// To the admin itself the hidden groups, and CTF only, whose one
	// group is hidden, tell nothing; Default holds groups the admin
	// sees, so it shows its real state.
	want = "federation 0 false outside_owner_grants, federation 50 true null, " +
		"federation 51 null hidden_from_viewer, federation 52 true null, " +
		"group -1001000000010 null hidden_from_viewer, group -1001000000003 true null, " +
		"group -1001000000001 null hidden_from_viewer"
	if got := e.tokenScope("admin", 90); got != want {
		t.Errorf("admin's token as admin: %s\nwant %s", got, want)
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

// An entry a token holds for a group hidden from the viewer goes out
// without its name and without whether the group is exposed, and a
// PATCH that sends it back saves; a new entry for such a group is
// still 400. adminEnv's sample: GNU/Weeb (-1001000000001) and CTF
// Practice (-1001000000010) are exposed, Linux Kernel Indonesia
// (-1001000000002) is not; the admin is account 3.
func TestMcpTokenHiddenEntryFromViewer(t *testing.T) {
	t.Parallel()
	e := newAdminEnv(t)
	const gnuweeb, lki, ctf = int64(-1001000000001), int64(-1001000000002), int64(-1001000000010)
	exec := func(q string, args ...any) {
		t.Helper()
		if _, err := e.app.Exec(q, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec("INSERT INTO web_mcp_tokens (id, web_user_id, name, token_sha256) VALUES " +
		"(90, 3, 't90', UNHEX(SHA2('tok90', 256)))")
	exec("INSERT INTO web_resource_relations (resource_type, resource_id, relation, user_id) " +
		"VALUES ('mcp_token', 90, 'owner', 3)")
	exec("INSERT INTO web_mcp_token_scopes (token_id, kind, target_id) VALUES "+
		"(90, 'group', ?), (90, 'group', ?)", gnuweeb, ctf)
	exec("INSERT INTO web_admin_hidden_groups (web_user_id, group_id) VALUES (3, ?), (3, ?)",
		gnuweeb, lki)

	names := func(role string) map[int64]string {
		t.Helper()
		_, pg := e.page(role, "/mcp-admin/tokens")
		var p struct {
			Tokens []struct {
				ID    int `json:"id"`
				Scope []struct {
					ID   int64  `json:"id"`
					Name string `json:"name"`
				} `json:"scope"`
			} `json:"tokens"`
		}
		raw, _ := json.Marshal(pg["props"])
		if err := json.Unmarshal(raw, &p); err != nil {
			t.Fatal(err)
		}
		out := map[int64]string{}
		for _, tk := range p.Tokens {
			if tk.ID == 90 {
				for _, x := range tk.Scope {
					out[x.ID] = x.Name
				}
			}
		}
		return out
	}
	if n := names("superadmin"); n[gnuweeb] != "GNU/Weeb" || n[ctf] != "CTF Practice" {
		t.Errorf("superadmin's names: %v", n)
	}
	if n := names("admin"); n[gnuweeb] != "" || n[ctf] != "CTF Practice" {
		t.Errorf("admin's names: %v", n)
	}
	// Withdrawn from MCP, the hidden group still tells the admin
	// nothing; a superadmin sees why.
	e.want(204, "superadmin", "DELETE", fmt.Sprintf("/api/mcp/groups/%d", gnuweeb), nil)
	if got := e.tokenScope("admin", 90); got != "group -1001000000010 true null, "+
		"group -1001000000001 null hidden_from_viewer" {
		t.Errorf("admin's view of an unexposed hidden group: %s", got)
	}
	if got := e.tokenScope("superadmin", 90); got != "group -1001000000010 true null, "+
		"group -1001000000001 false not_exposed" {
		t.Errorf("superadmin's view of it: %s", got)
	}
	audits := len(e.audits())

	// The form sends the whole scope back, with a new name: it saves,
	// and the held hidden entry stays.
	held := []map[string]any{{"kind": "group", "id": gnuweeb}, {"kind": "group", "id": ctf}}
	e.want(204, "admin", "PATCH", "/api/mcp/tokens/90", map[string]any{"name": "renamed", "scope": held})
	if got := e.tokenScope("superadmin", 90); got != "group -1001000000010 true null, "+
		"group -1001000000001 false not_exposed" {
		t.Errorf("after the round trip: %s", got)
	}
	if a := e.audits()[audits:]; len(a) != 1 || !strings.Contains(a[0], "mcp_token_rename") {
		t.Errorf("round trip audits %v, want only the rename", a)
	}
	// Dropping it works too; it cannot come back from the admin.
	e.want(204, "admin", "PATCH", "/api/mcp/tokens/90", map[string]any{"scope": held[1:]})
	if got := e.tokenScope("superadmin", 90); got != "group -1001000000010 true null" {
		t.Errorf("after dropping the hidden entry: %s", got)
	}
	for _, g := range []int64{gnuweeb, lki} {
		e.want(400, "admin", "PATCH", "/api/mcp/tokens/90", map[string]any{"scope": append(slices.Clip(held[1:]),
			map[string]any{"kind": "group", "id": g})})
		e.want(400, "admin", "POST", "/api/mcp/tokens", map[string]any{"name": "new",
			"scope": []map[string]any{{"kind": "group", "id": g}}})
	}
	if got := e.tokenScope("superadmin", 90); got != "group -1001000000010 true null" {
		t.Errorf("after the refusals: %s", got)
	}
}
