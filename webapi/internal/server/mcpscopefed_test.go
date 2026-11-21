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

// A new federation entry the viewer may not be told about is refused
// like a new entry for a hidden group, and a held one is kept
// (decisions/2026-11-22-mcp-scope-federation-options.md); a superadmin
// may write any federation.
func TestMcpScopeFederationWrite(t *testing.T) {
	t.Parallel()
	e := newAdminEnv(t)
	exec := e.fedSample()
	const gnuweeb = int64(-1001000000001)
	fed := func(id int64) map[string]any { return map[string]any{"kind": "federation", "id": id} }
	stored := func() string {
		t.Helper()
		return e.appOne("SELECT GROUP_CONCAT(CONCAT(kind, ' ', target_id) ORDER BY kind, target_id " +
			"SEPARATOR ', ') FROM web_mcp_token_scopes WHERE token_id = 90")
	}
	refused := func(what, role, method, path string, body map[string]any) {
		t.Helper()
		st, bd, _ := e.call(role, method, path, body)
		if st != 400 || bd["error"] != "bad_request" ||
			!strings.Contains(fmt.Sprint(bd["message"]), "scope entry federation") {
			t.Errorf("%s: %d %v", what, st, bd)
		}
	}
	tokens := e.appOne("SELECT COUNT(*) FROM web_mcp_tokens")

	// Minting: the admin may not name GNU only or LKI only, whose one
	// group is hidden from it, exposed or not; it may name Empty,
	// Mixed and Default. The member may not name LKI only, nor
	// Default, which holds no group it is granted.
	for _, c := range []struct {
		role string
		fed  int64
	}{{"admin", 60}, {"admin", 61}, {"member", 61}, {"member", 0}} {
		refused(fmt.Sprintf("%s minting federation %d", c.role, c.fed), c.role, "POST",
			"/api/mcp/tokens", map[string]any{"name": "x", "scope": []any{fed(c.fed)}})
	}
	if got := e.appOne("SELECT COUNT(*) FROM web_mcp_tokens"); got != tokens {
		t.Errorf("tokens minted by refused requests: %s, was %s", got, tokens)
	}
	e.want(201, "admin", "POST", "/api/mcp/tokens", map[string]any{"name": "x",
		"scope": []any{fed(0), fed(62), fed(63)}})
	e.want(201, "member", "POST", "/api/mcp/tokens", map[string]any{"name": "x",
		"scope": []any{fed(60), fed(62), fed(63)}})

	// The member's token 90 holds GNU only and GNU/Weeb.
	exec("INSERT INTO web_mcp_tokens (id, web_user_id, name, token_sha256) VALUES " +
		"(90, 5, 't90', UNHEX(SHA2('tok90', 256)))")
	exec("INSERT INTO web_resource_relations (resource_type, resource_id, relation, user_id) " +
		"VALUES ('mcp_token', 90, 'owner', 5)")
	exec("INSERT INTO web_mcp_token_scopes (token_id, kind, target_id) VALUES "+
		"(90, 'federation', 60), (90, 'group', ?)", gnuweeb)
	const both = "federation 60, group -1001000000001"
	held := []any{fed(60), map[string]any{"kind": "group", "id": gnuweeb}}
	audits := len(e.audits())
	// The admin's form sends it back with a new name: it saves, the
	// held entries stay, and only the rename is audited.
	e.want(204, "admin", "PATCH", "/api/mcp/tokens/90", map[string]any{"name": "renamed", "scope": held})
	if got := stored(); got != both {
		t.Errorf("after the round trip: %q", got)
	}
	if a := e.audits()[audits:]; len(a) != 1 || !strings.Contains(a[0], "mcp_token_rename") {
		t.Errorf("round trip audits %v, want only the rename", a)
	}
	// Adding LKI only is refused, and changes nothing.
	refused("admin adding LKI only", "admin", "PATCH", "/api/mcp/tokens/90",
		map[string]any{"scope": append(slices.Clip(held), fed(61))})
	if got := stored(); got != both {
		t.Errorf("after the refusal: %q", got)
	}
	// Dropped, GNU only cannot come back from the admin.
	e.want(204, "admin", "PATCH", "/api/mcp/tokens/90", map[string]any{"scope": held[1:]})
	refused("admin adding GNU only back", "admin", "PATCH", "/api/mcp/tokens/90",
		map[string]any{"scope": held})
	if got := stored(); got != "group -1001000000001" {
		t.Errorf("after dropping GNU only: %q", got)
	}
	// A superadmin may write every federation.
	e.want(204, "superadmin", "PATCH", "/api/mcp/tokens/90",
		map[string]any{"scope": []any{fed(0), fed(60), fed(61), fed(62), fed(63)}})
	if got := stored(); got != "federation 0, federation 60, federation 61, federation 62, federation 63" {
		t.Errorf("superadmin's write: %q", got)
	}
}
