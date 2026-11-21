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

// A member's MCP token reads only the exposed groups the member's
// grants let it see (decisions/2026-11-21-mcp-token-owner-grants.md),
// on the "mcp" fixture: Kernel Hackers ID (mcpG1) and Rust Belajar
// (mcpG2) are exposed, Secret Ops (mcpG3) is not, and researcher (5)
// is a member filed in Default.

// memberToken gives the live token to researcher, with mcp_tokens and
// grants of groups in Default.
func (e *mcpEnv) memberToken(grants ...int64) {
	e.t.Helper()
	e.webExec("INSERT INTO web_member_abilities (web_user_id, ability) VALUES (5, 'mcp_tokens')")
	e.webExec("UPDATE web_resource_relations SET user_id = 5 WHERE resource_type = 'mcp_token' " +
		"AND resource_id = 1")
	for _, g := range grants {
		e.webExec("INSERT INTO web_member_grant_groups (federation_id, web_user_id, group_id) "+
			"VALUES (0, 5, ?)", g)
	}
}

// listedGroups is the group ids telegram_list_groups answers.
func (e *mcpEnv) listedGroups() []int64 {
	e.t.Helper()
	l := sc(e.call("telegram_list_groups", map[string]any{"limit": 200}))
	var out []int64
	gs, _ := l["groups"].([]any)
	for _, g := range gs {
		out = append(out, int64(g.(map[string]any)["group_id"].(float64)))
	}
	slices.Sort(out)
	return out
}

func TestMCPMemberTokenWithinGrants(t *testing.T) {
	t.Parallel()
	e := newMCPEnv(t, MCPConfig{})
	// Granted Rust Belajar and Secret Ops, never Kernel Hackers ID.
	e.memberToken(mcpG2, mcpG3)

	only := func(when string, want ...int64) {
		t.Helper()
		slices.Sort(want)
		kernel := slices.Contains(want, mcpG1)
		if got := e.listedGroups(); fmt.Sprint(got) != fmt.Sprint(want) {
			t.Errorf("%s: list_groups %v, want %v", when, got, want)
		}
		wantMsgs := map[float64]int{}
		for _, g := range want {
			wantMsgs[float64(g)] = map[int64]int{mcpG1: 13, mcpG2: 3}[g]
		}
		for _, tool := range []string{"telegram_search_messages", "telegram_list_recent_messages"} {
			if got := groupIDs(e.call(tool, map[string]any{"limit": 200})); fmt.Sprint(got) != fmt.Sprint(wantMsgs) {
				t.Errorf("%s: %s by group %v, want %v", when, tool, got, wantMsgs)
			}
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
	}

	// No entries: the exposed groups within the grants, not every
	// exposed group; the granted Secret Ops stays unexposed.
	only("no scope", mcpG2)
	// Entries narrow within the grants, never beyond them.
	e.setScope("group", mcpG1)
	only("scoped outside the grants")
	e.setScope("group", mcpG1, "group", mcpG2)
	only("scoped across the grants", mcpG2)
	e.setScope("federation", 0)
	only("scoped to Default", mcpG2)

	// A grant withdrawn counts at once, with or without entries.
	e.webExec("DELETE FROM web_member_grant_groups WHERE web_user_id = 5 AND group_id = ?", mcpG2)
	only("grant withdrawn, Default entry")
	e.setScope()
	only("grant withdrawn, no scope")
	// Granted again, and a group moved into the member's reach.
	e.webExec("INSERT INTO web_member_grant_groups (federation_id, web_user_id, group_id) "+
		"VALUES (0, 5, ?), (0, 5, ?)", mcpG1, mcpG2)
	only("granted both", mcpG1, mcpG2)
	// A grant whose group left its federation counts for nothing, as
	// on the web.
	e.webExec("INSERT INTO web_federations (id, name) VALUES (50, 'Kernel only')")
	e.webExec("INSERT INTO web_federation_groups (federation_id, group_id) VALUES (50, ?)", mcpG1)
	only("Kernel Hackers ID moved out of Default", mcpG2)

	// Back with a superadmin, the grants no longer apply.
	e.webExec("UPDATE web_resource_relations SET user_id = 1 WHERE resource_type = 'mcp_token' " +
		"AND resource_id = 1")
	only("owned by a superadmin", mcpG1, mcpG2)
	// Without an owner record neither.
	e.webExec("DELETE FROM web_resource_relations WHERE resource_type = 'mcp_token' AND resource_id = 1")
	only("without an owner record", mcpG1, mcpG2)
}

// An admin's token is not bounded by the groups hidden from the admin
// (not decided yet): it reads every exposed group as before.
func TestMCPAdminTokenUnbounded(t *testing.T) {
	t.Parallel()
	e := newMCPEnv(t, MCPConfig{})
	e.webExec("UPDATE web_resource_relations SET user_id = 3 WHERE resource_type = 'mcp_token' " +
		"AND resource_id = 1")
	e.webExec("INSERT INTO web_admin_hidden_groups (web_user_id, group_id) VALUES (3, ?)", mcpG1)
	if got := e.listedGroups(); fmt.Sprint(got) != fmt.Sprint([]int64{mcpG2, mcpG1}) {
		t.Errorf("admin's token: %v", got)
	}
}

// The token list marks a member's entries outside the member's grants
// inactive, and follows the grants as they change; the same entries on
// a superadmin's token stay active. adminEnv's sample exposes GNU/Weeb
// (-1001000000001), Rust Programming (-1001000000003) and CTF Practice
// (-1001000000010); member is account 5.
func TestMcpTokenListMemberGrants(t *testing.T) {
	t.Parallel()
	e := newAdminEnv(t)
	const gnuweeb, rust, ctf = int64(-1001000000001), int64(-1001000000003), int64(-1001000000010)
	exec := func(q string, args ...any) {
		t.Helper()
		if _, err := e.app.Exec(q, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec("INSERT INTO web_member_abilities (web_user_id, ability) VALUES (5, 'mcp_tokens')")
	exec("INSERT INTO web_federations (id, name) VALUES (50, 'Rust only'), (51, 'CTF only')")
	exec("INSERT INTO web_federation_groups (federation_id, group_id) VALUES (50, ?), (51, ?)", rust, ctf)
	exec("INSERT IGNORE INTO web_federation_members (federation_id, web_user_id) VALUES (0, 5), (50, 5)")
	exec("INSERT INTO web_member_grant_groups (federation_id, web_user_id, group_id) VALUES "+
		"(0, 5, ?), (50, 5, ?)", gnuweeb, rust)
	// Token 90 is the member's, 91 a superadmin's, with the same scope.
	for id, owner := range map[int]int{90: 5, 91: 1} {
		exec("INSERT INTO web_mcp_tokens (id, web_user_id, name, token_sha256) VALUES "+
			"(?, ?, ?, UNHEX(SHA2(?, 256)))", id, owner, fmt.Sprint("t", id), fmt.Sprint("tok", id))
		exec("INSERT INTO web_resource_relations (resource_type, resource_id, relation, user_id) "+
			"VALUES ('mcp_token', ?, 'owner', ?)", id, owner)
		exec("INSERT INTO web_mcp_token_scopes (token_id, kind, target_id) VALUES "+
			"(?, 'group', ?), (?, 'group', ?), (?, 'federation', 0), (?, 'federation', 50), "+
			"(?, 'federation', 51)", id, gnuweeb, id, ctf, id, id, id)
	}
	active := func(role string, id int) string {
		t.Helper()
		st, pg := e.page(role, "/mcp-admin/tokens")
		if st != 200 {
			t.Fatalf("%s tokens page %d %v", role, st, pg)
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
			t.Fatal(err)
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
		t.Fatalf("%s does not list token %d", role, id)
		return ""
	}
	const all = "federation 0 true null, federation 50 true null, federation 51 true null, " +
		"group -1001000000010 true null, group -1001000000001 true null"
	if got := active("superadmin", 91); got != all {
		t.Errorf("superadmin's token: %s", got)
	}
	want := "federation 0 true null, federation 50 true null, " +
		"federation 51 false outside_owner_grants, " +
		"group -1001000000010 false outside_owner_grants, group -1001000000001 true null"
	for _, role := range []string{"superadmin", "member"} {
		if got := active(role, 90); got != want {
			t.Errorf("member's token as %s: %s\nwant %s", role, got, want)
		}
	}
	// Grants withdrawn: the entries stay, inactive.
	exec("DELETE FROM web_member_grant_groups WHERE web_user_id = 5 AND group_id = ?", gnuweeb)
	want = "federation 0 false outside_owner_grants, federation 50 true null, " +
		"federation 51 false outside_owner_grants, " +
		"group -1001000000010 false outside_owner_grants, " +
		"group -1001000000001 false outside_owner_grants"
	if got := active("superadmin", 90); got != want {
		t.Errorf("after withdrawing GNU/Weeb: %s\nwant %s", got, want)
	}
	exec("DELETE FROM web_federation_members WHERE web_user_id = 5 AND federation_id = 50")
	want = "federation 0 false outside_owner_grants, federation 50 false outside_owner_grants, " +
		"federation 51 false outside_owner_grants, " +
		"group -1001000000010 false outside_owner_grants, " +
		"group -1001000000001 false outside_owner_grants"
	if got := active("superadmin", 90); got != want {
		t.Errorf("after leaving Rust only: %s\nwant %s", got, want)
	}
	if got := active("superadmin", 91); got != all {
		t.Errorf("superadmin's token after the member's changes: %s", got)
	}
	// The other reasons come first: an unexposed group, a deleted
	// federation, a private chat on a token a superadmin does not own
	// (here the member's), and one of a user the archive does not know.
	e.want(204, "superadmin", "DELETE", fmt.Sprintf("/api/mcp/groups/%d", gnuweeb), nil)
	exec("DELETE FROM web_federations WHERE id = 51")
	exec("INSERT INTO web_mcp_token_scopes (token_id, kind, target_id) VALUES " +
		"(90, 'private', 9299000001), (91, 'private', 9299000001), (91, 'private', 9299999999)")
	want = "federation 0 false outside_owner_grants, federation 50 false outside_owner_grants, " +
		"federation 51 false federation_deleted, " +
		"group -1001000000010 false outside_owner_grants, " +
		"group -1001000000001 false not_exposed, private 9299000001 false private_not_superadmin"
	if got := active("superadmin", 90); got != want {
		t.Errorf("member's token, other reasons: %s\nwant %s", got, want)
	}
	want = "federation 0 true null, federation 50 true null, federation 51 false federation_deleted, " +
		"group -1001000000010 true null, group -1001000000001 false not_exposed, " +
		"private 9299000001 true null, private 9299999999 false user_unknown"
	if got := active("superadmin", 91); got != want {
		t.Errorf("superadmin's token, other reasons: %s\nwant %s", got, want)
	}
}
