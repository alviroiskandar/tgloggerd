// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"regexp"
	"strings"
	"testing"
	"unicode/utf8"

	"tgloggerd/webapi/internal/logdb"
	"tgloggerd/webapi/internal/scope"
)

// The MCP administration (adminEnv): the sample exposes GNU/Weeb, Rust
// Programming and CTF Practice; the web's "admin" fixture has a live
// and a revoked token.

func TestMcpPages(t *testing.T) {
	t.Parallel()
	e := newAdminEnv(t)

	st, b := e.page("superadmin", "/mcp-admin")
	if st != 200 || b["route"] != "mcp" {
		t.Fatalf("index %d %v", st, b)
	}
	// GNU/Weeb 8 + Rust 2 + CTF Practice's messages.
	msgs := e.one("SELECT SUM(g.msg_count) FROM telegram_public_groups p " +
		"JOIN telegram_groups g ON g.id = p.group_id")
	if js(b["props"]) != canon(`{"exposed_groups": 3, "exposed_messages": `+msgs+`,
		"active_tokens": 1, "last_used": "2026-11-17T15:00:00Z",
		"endpoint": "http://example.com/mcp"}`) {
		t.Errorf("index %s", js(b["props"]))
	}

	_, b = e.page("superadmin", "/mcp-admin/groups")
	gs := b["props"].(map[string]any)["groups"].([]any)
	if len(gs) != 3 {
		t.Fatalf("groups %v", gs)
	}
	var titles []string
	for _, g := range gs {
		m := g.(map[string]any)
		titles = append(titles, m["group"].(map[string]any)["title"].(string))
		if m["group"].(map[string]any)["msg_count"] == nil || m["added"] == "" {
			t.Errorf("group %v", m)
		}
	}
	if !strings.Contains(js(gs), `"note":"community"`) || !strings.Contains(js(gs), `"public_now":true`) {
		t.Errorf("groups %s", js(gs))
	}

	// The fixture's tokens have no owner record: they are the
	// superadmins', shown as shared, and an admin sees neither.
	_, b = e.page("superadmin", "/mcp-admin/tokens")
	shared := `{"id": 0, "name": "Shared", "active": true}`
	if js(b["props"].(map[string]any)["tokens"]) != canon(`[
		{"id": 2, "name": "old laptop", "owner": `+shared+`,
		 "minted_by": {"id": 2, "name": "ops", "role": "superadmin", "active": true},
		 "created": "2026-03-01T08:00:00Z",
		 "last_used": "2026-04-01T08:00:00Z", "revoked": "2026-05-01T08:00:00Z", "scope": []},
		{"id": 1, "name": "claude-desktop", "owner": `+shared+`,
		 "minted_by": {"id": 1, "name": "admin", "role": "superadmin", "active": true},
		 "created": "2026-10-01T08:00:00Z",
		 "last_used": "2026-11-17T15:00:00Z", "revoked": null, "scope": []}]`) {
		t.Errorf("tokens %s", js(b["props"]))
	}
	_, b = e.page("admin", "/mcp-admin/tokens")
	if tk := b["props"].(map[string]any)["tokens"]; js(tk) != "[]" {
		t.Errorf("admin's tokens %s", js(tk))
	}
	_, b = e.page("admin", "/mcp-admin")
	if m := b["props"].(map[string]any); m["active_tokens"] != float64(0) || m["last_used"] != nil {
		t.Errorf("admin's index %v", m)
	}

	for _, p := range []string{"/mcp-admin", "/mcp-admin/groups", "/mcp-admin/tokens"} {
		if st, b := e.page("member", p); st != 404 || b["route"] != "not_found" {
			t.Errorf("member %s: %d %v", p, st, b)
		}
	}
}

func TestMcpGroups(t *testing.T) {
	t.Parallel()
	e := newAdminEnv(t)
	const kernel = -1001000000002 // Linux Kernel Indonesia, not exposed

	b := e.want(201, "admin", "POST", "/api/mcp/groups", map[string]any{"group_id": kernel,
		"note": "  " + strings.Repeat("n", 300) + "  "})
	if b["group_id"] != float64(kernel) {
		t.Errorf("expose %v", b)
	}
	if got := e.one("SELECT CONCAT_WS(' ', CHAR_LENGTH(note), added_by) FROM telegram_public_groups "+
		"WHERE group_id = ?", kernel); got != "255 3" {
		t.Errorf("stored %q", got)
	}
	if st, b, _ := e.call("superadmin", "POST", "/api/mcp/groups", map[string]any{"group_id": kernel}); st != 409 ||
		b["error"] != "already_exposed" {
		t.Errorf("again %d %v", st, b)
	}
	e.want(204, "superadmin", "DELETE", fmt.Sprintf("/api/mcp/groups/%d", kernel), nil)
	if e.one("SELECT COUNT(*) FROM telegram_public_groups WHERE group_id = ?", kernel) != "0" {
		t.Error("not withdrawn")
	}
	e.want(404, "superadmin", "DELETE", fmt.Sprintf("/api/mcp/groups/%d", kernel), nil)
	e.want(201, "superadmin", "POST", "/api/mcp/groups", map[string]any{"group_id": kernel, "note": "again"})

	want := []string{
		fmt.Sprintf("3 mcp_expose group %d %s", kernel, strings.Repeat("n", 255-len(fmt.Sprintf("group %d ", kernel)))),
		fmt.Sprintf("3 owner_set mcp_group %d", kernel),
		fmt.Sprintf("1 mcp_unexpose group %d", kernel),
		fmt.Sprintf("1 resource_drop mcp_group %d", kernel),
		fmt.Sprintf("1 mcp_expose group %d again", kernel),
		fmt.Sprintf("1 owner_set mcp_group %d", kernel),
	}
	if got := e.audits(); strings.Join(got, "\n") != strings.Join(want, "\n") {
		t.Errorf("audit\n%s\nwant\n%s", strings.Join(got, "\n"), strings.Join(want, "\n"))
	}

	for _, c := range []struct {
		role   string
		method string
		path   string
		body   map[string]any
		status int
	}{
		{"superadmin", "POST", "/api/mcp/groups", map[string]any{"group_id": -1009999999999}, 404},
		{"superadmin", "POST", "/api/mcp/groups", map[string]any{"group_id": 9299000001}, 400},
		{"superadmin", "POST", "/api/mcp/groups", map[string]any{}, 400},
		{"superadmin", "DELETE", "/api/mcp/groups/-1009999999999", nil, 404},
		{"superadmin", "DELETE", "/api/mcp/groups/x", nil, 404},
		{"member", "POST", "/api/mcp/groups", map[string]any{"group_id": -1001000000004}, 404},
		{"member", "DELETE", "/api/mcp/groups/-1001000000001", nil, 404},
		{"", "POST", "/api/mcp/groups", map[string]any{"group_id": -1001000000004}, 401},
	} {
		if st, b, _ := e.call(c.role, c.method, c.path, c.body); st != c.status {
			t.Errorf("%s %s %s %v: %d %v", c.role, c.method, c.path, c.body, st, b)
		}
	}
	if st, _, _ := e.call("admin", "POST", "/api/mcp/groups", map[string]any{"group_id": -1001000000004}, false); st != 403 {
		t.Errorf("no csrf %d", st)
	}
	if n := e.one("SELECT COUNT(*) FROM telegram_public_groups"); n != "4" {
		t.Errorf("%s exposed after refusals", n)
	}
	if len(e.audits()) != 6 {
		t.Errorf("refusals audited: %v", e.audits())
	}
}

// The note of an exposed group is reworded in place: who exposed it
// and when stay, and the audit row keeps the old and the new note.
func TestMcpGroupNote(t *testing.T) {
	t.Parallel()
	e := newAdminEnv(t)
	const gnu = -1001000000001 // GNU/Weeb, exposed with "community"
	path := fmt.Sprintf("/api/mcp/groups/%d", gnu)
	row := func() string {
		return e.one("SELECT CONCAT_WS(' ', added_by, created_at) FROM telegram_public_groups "+
			"WHERE group_id = ?", gnu)
	}
	note := func() string {
		return e.one("SELECT note FROM telegram_public_groups WHERE group_id = ?", gnu)
	}
	if _, err := e.ro.Exec("UPDATE telegram_public_groups SET added_by = 1, "+
		"created_at = '2026-01-01 00:00:00' WHERE group_id = ?", gnu); err != nil {
		t.Fatal(err)
	}
	before := row()

	e.want(204, "admin", "PATCH", path, map[string]any{"note": "  kernel hackers  "})
	if got := note(); got != "kernel hackers" {
		t.Errorf("note %q", got)
	}
	if got := row(); got != before {
		t.Errorf("exposer and date %q, want %q", got, before)
	}
	_, b := e.page("admin", "/mcp-admin/groups")
	if !strings.Contains(js(b["props"]), `"note":"kernel hackers"`) {
		t.Errorf("page %s", js(b["props"]))
	}
	// Unchanged: nothing to audit.
	e.want(204, "superadmin", "PATCH", path, map[string]any{"note": "kernel hackers"})
	// Clipped to the column.
	e.want(204, "superadmin", "PATCH", path, map[string]any{"note": strings.Repeat("n", 300)})
	if got := note(); got != strings.Repeat("n", 255) {
		t.Errorf("clipped note %d characters", len(got))
	}
	// An empty note clears it.
	e.want(204, "admin", "PATCH", path, map[string]any{"note": ""})
	if got := note(); got != "" {
		t.Errorf("cleared note %q", got)
	}
	if got := row(); got != before {
		t.Errorf("exposer and date %q, want %q", got, before)
	}

	prefix := fmt.Sprintf("group %d ", gnu)
	room := 255 - len(prefix) - len(" -> ")
	want := []string{
		fmt.Sprintf("3 mcp_note %scommunity -> kernel hackers", prefix),
		fmt.Sprintf("1 mcp_note %skernel hackers -> %s", prefix, strings.Repeat("n", room-len("kernel hackers"))),
		fmt.Sprintf("3 mcp_note %s%s -> ", prefix, strings.Repeat("n", room)),
	}
	if got := e.audits(); strings.Join(got, "\n") != strings.Join(want, "\n") {
		t.Errorf("audit\n%s\nwant\n%s", strings.Join(got, "\n"), strings.Join(want, "\n"))
	}

	if _, err := e.ro.Exec("UPDATE telegram_public_groups SET note = 'community' WHERE group_id = ?",
		gnu); err != nil {
		t.Fatal(err)
	}
	for _, c := range []struct {
		role   string
		path   string
		body   any
		status int
	}{
		{"superadmin", "/api/mcp/groups/-1009999999999", map[string]any{"note": "x"}, 404},
		{"superadmin", "/api/mcp/groups/-1001000000002", map[string]any{"note": "x"}, 404}, // not exposed
		{"superadmin", "/api/mcp/groups/x", map[string]any{"note": "x"}, 404},
		{"superadmin", "/api/mcp/groups/1001000000001", map[string]any{"note": "x"}, 404},
		{"superadmin", path, map[string]any{}, 400},
		{"superadmin", path, map[string]any{"note": nil}, 400},
		{"superadmin", path, map[string]any{"note": 5}, 400},
		{"member", path, map[string]any{"note": "x"}, 404},
		{"", path, map[string]any{"note": "x"}, 401},
	} {
		if st, b, _ := e.call(c.role, "PATCH", c.path, c.body); st != c.status {
			t.Errorf("%s %s %v: %d %v", c.role, c.path, c.body, st, b)
		}
	}
	if st, _, _ := e.call("admin", "PATCH", path, map[string]any{"note": "x"}, false); st != 403 {
		t.Errorf("no csrf %d", st)
	}
	if got := note(); got != "community" {
		t.Errorf("note after refusals %q", got)
	}
	if e.one("SELECT COUNT(*) FROM telegram_public_groups WHERE group_id = -1001000000002") != "0" {
		t.Error("a refused note exposed a group")
	}
	if len(e.audits()) != len(want) {
		t.Errorf("refusals audited: %v", e.audits())
	}
}

// noteChange gives each note its share of the audit detail.
func TestMcpNoteChange(t *testing.T) {
	t.Parallel()
	long := strings.Repeat("o", 200)
	got := noteChange("group -1 ", long, strings.Repeat("n", 200))
	if utf8.RuneCountInString(got) != 255 || !strings.Contains(got, strings.Repeat("o", 121)+" -> "+strings.Repeat("n", 121)) {
		t.Errorf("both long: %d %q", len(got), got)
	}
	if got := noteChange("group -1 ", "a", "b"); got != "group -1 a -> b" {
		t.Errorf("short %q", got)
	}
	got = noteChange("group -1 ", strings.Repeat("\u00e9", 250), "b")
	if utf8.RuneCountInString(got) != 255 || !strings.HasSuffix(got, "\u00e9 -> b") {
		t.Errorf("runes %d %q", utf8.RuneCountInString(got), got)
	}
}

// A group the scope hides is not exposed, listed or counted.
func TestMcpHiddenGroups(t *testing.T) {
	t.Parallel()
	e := newAdminEnv(t)
	ctx := context.Background()
	member := scope.For(scope.Viewer{ID: 5, Role: scope.Member})
	if ok, err := logdb.GroupVisible(ctx, e.ro, member, -1001000000002); ok || err != nil {
		t.Errorf("member sees the group: %v %v", ok, err)
	}
	gs, err := logdb.ExposedGroups(ctx, e.ro, member)
	if err != nil || len(gs) != 0 {
		t.Errorf("member lists %v %v", gs, err)
	}
	if n, m, err := logdb.ExposedStats(ctx, e.ro, member); n != 0 || m != 0 || err != nil {
		t.Errorf("member counts %d %d %v", n, m, err)
	}
}

var mcpTokenRe = regexp.MustCompile(`^tgmcp_[0-9a-f]{64}$`)

func TestMcpTokens(t *testing.T) {
	t.Parallel()
	e := newAdminEnv(t)
	b := e.want(201, "admin", "POST", "/api/mcp/tokens", map[string]any{"name": "  ci runner  "})
	tok, _ := b["token"].(string)
	id := uint64(b["id"].(float64))
	if !mcpTokenRe.MatchString(tok) {
		t.Fatalf("token %q", tok)
	}
	// Stored as its SHA-256 only, owned by the viewer.
	sum := sha256.Sum256([]byte(tok))
	if got := e.appOne("SELECT CONCAT_WS(' ', web_user_id, name, HEX(token_sha256)) "+
		"FROM web_mcp_tokens WHERE id = ?", id); got != "3 ci runner "+strings.ToUpper(hex.EncodeToString(sum[:])) {
		t.Errorf("stored %q", got)
	}
	// Never shown again.
	_, p := e.page("admin", "/mcp-admin/tokens")
	noSecrets(t, "tokens page", p, tok, tok[6:])
	if !strings.Contains(js(p), `"name":"ci runner"`) {
		t.Errorf("tokens page %s", js(p))
	}

	e.want(204, "admin", "POST", fmt.Sprintf("/api/mcp/tokens/%d/revoke", id), nil)
	e.want(204, "admin", "POST", fmt.Sprintf("/api/mcp/tokens/%d/revoke", id), nil)
	if e.appOne("SELECT revoked_at IS NOT NULL FROM web_mcp_tokens WHERE id = ?", id) != "1" {
		t.Error("not revoked")
	}
	e.want(404, "admin", "POST", "/api/mcp/tokens/999/revoke", nil)

	want := []string{
		fmt.Sprintf("3 owner_set mcp_token %d", id),
		fmt.Sprintf("3 mcp_token_create token %d ci runner", id),
		fmt.Sprintf("3 mcp_token_revoke token %d ci runner", id),
	}
	got := e.audits()
	if strings.Join(got, "\n") != strings.Join(want, "\n") {
		t.Errorf("audit\n%s\nwant\n%s", strings.Join(got, "\n"), strings.Join(want, "\n"))
	}
	noSecrets(t, "audit", got, tok[6:])

	for _, c := range []struct {
		role   string
		path   string
		body   map[string]any
		status int
	}{
		{"admin", "/api/mcp/tokens", map[string]any{"name": "   "}, 400},
		{"admin", "/api/mcp/tokens", map[string]any{}, 400},
		{"member", "/api/mcp/tokens", map[string]any{"name": "x"}, 404},
		{"member", "/api/mcp/tokens/1/revoke", nil, 404},
		{"", "/api/mcp/tokens", map[string]any{"name": "x"}, 401},
	} {
		if st, b, _ := e.call(c.role, "POST", c.path, c.body); st != c.status {
			t.Errorf("%s %s %v: %d %v", c.role, c.path, c.body, st, b)
		}
	}
	if st, _, _ := e.call("admin", "POST", "/api/mcp/tokens/1/revoke", nil, false); st != 403 {
		t.Errorf("no csrf %d", st)
	}
	if e.appOne("SELECT revoked_at IS NULL FROM web_mcp_tokens WHERE id = 1") != "1" {
		t.Error("token 1 revoked by a refused request")
	}
	// A long name is cut to the column's 64 characters.
	b = e.want(201, "admin", "POST", "/api/mcp/tokens", map[string]any{"name": strings.Repeat("e", 70)})
	if n := e.appOne("SELECT CHAR_LENGTH(name) FROM web_mcp_tokens WHERE id = ?", b["id"]); n != "64" {
		t.Errorf("name of %s characters", n)
	}
}

// Who manages which MCP token (decisions/2026-11-20-mcp-tokens-to-
// superadmin.md): the owner, an admin for the members it manages, every
// superadmin; members need mcp_tokens, and only superadmins transfer.
// In adminEnv, "member" (researcher, 5) and mod_kernel (6) hold
// mcp_tokens; mod_kernel is moved out of Default into a federation the
// "admin" (gw_admin, 3) does not administer. The fixture's tokens 1
// and 2 have no owner record, so they are the superadmins'.
func TestMcpTokenRoles(t *testing.T) {
	t.Parallel()
	e := newAdminEnv(t)
	for _, q := range []string{
		"INSERT INTO web_member_abilities (web_user_id, ability) VALUES (5, 'mcp_tokens'), (6, 'mcp_tokens')",
		"INSERT INTO web_federations (id, name) VALUES (50, 'Elsewhere')",
		"DELETE FROM web_federation_members WHERE federation_id = 0 AND web_user_id = 6",
		"INSERT INTO web_federation_members (federation_id, web_user_id) VALUES (50, 6)",
	} {
		if _, err := e.app.Exec(q); err != nil {
			t.Fatal(err)
		}
	}
	mint := func(role, name string, owner uint64) uint64 {
		t.Helper()
		body := map[string]any{"name": name}
		if owner != 0 {
			body["owner_id"] = owner
		}
		b := e.want(201, role, "POST", "/api/mcp/tokens", body)
		return uint64(b["id"].(float64))
	}
	a := mint("member", "member's own", 0)
	b := mint("admin", "admin's own", 0)
	c := mint("admin", "for the member", 5)
	d := mint("superadmin", "for mod_kernel", 6)
	owner := func(id uint64) string {
		t.Helper()
		return e.appOne("SELECT CONCAT(user_id, '/', (SELECT web_user_id FROM web_mcp_tokens WHERE id = ?)) "+
			"FROM web_resource_relations WHERE resource_type = 'mcp_token' AND resource_id = ? "+
			"AND relation = 'owner'", id, id)
	}
	for id, want := range map[uint64]string{a: "5/5", b: "3/3", c: "5/3", d: "6/1"} {
		if got := owner(id); got != want {
			t.Errorf("token %d owner/minter %s, want %s", id, got, want)
		}
	}

	// Owners a viewer may not mint for.
	for _, x := range []struct {
		role  string
		owner uint64
	}{
		{"admin", 6},  // a member it does not manage
		{"admin", 1},  // a superadmin
		{"admin", 4},  // another admin
		{"admin", 7},  // its member, but disabled
		{"member", 3}, // anyone else
		{"superadmin", 7},
		{"superadmin", 999},
	} {
		if st, bd, _ := e.call(x.role, "POST", "/api/mcp/tokens",
			map[string]any{"name": "x", "owner_id": x.owner}); st != 400 || bd["error"] != "bad_request" {
			t.Errorf("%s minting for %d: %d %v", x.role, x.owner, st, bd)
		}
	}

	type person struct {
		ID   uint64 `json:"id"`
		Name string `json:"name"`
	}
	type props struct {
		Tokens []struct {
			ID       uint64 `json:"id"`
			Owner    person `json:"owner"`
			MintedBy person `json:"minted_by"`
		} `json:"tokens"`
		Owners      []person `json:"owners"`
		CanTransfer bool     `json:"can_transfer"`
	}
	list := func(role string) props {
		t.Helper()
		st, pg := e.page(role, "/mcp-admin/tokens")
		if st != 200 {
			t.Fatalf("%s tokens page %d %v", role, st, pg)
		}
		var p props
		raw, _ := json.Marshal(pg["props"])
		if err := json.Unmarshal(raw, &p); err != nil {
			t.Fatal(err)
		}
		return p
	}
	ids := func(p props) string {
		var out []string
		for _, tk := range p.Tokens {
			out = append(out, fmt.Sprint(tk.ID))
		}
		return strings.Join(out, " ")
	}
	names := func(ps []person) string {
		var out []string
		for _, x := range ps {
			out = append(out, x.Name)
		}
		return strings.Join(out, " ")
	}
	for _, x := range []struct {
		role, tokens, owners string
		transfer             bool
	}{
		{"member", fmt.Sprint(c, " ", a), "researcher", false},
		{"admin", fmt.Sprint(c, " ", b, " ", a), "gw_admin researcher", false},
		{"superadmin", fmt.Sprint(d, " ", c, " ", b, " ", a, " 2 1"),
			"admin gw_admin mod_kernel ops researcher sec_lead", true},
	} {
		p := list(x.role)
		if ids(p) != x.tokens || names(p.Owners) != x.owners || p.CanTransfer != x.transfer {
			t.Errorf("%s: tokens %s owners %s transfer %v", x.role, ids(p), names(p.Owners), p.CanTransfer)
		}
	}
	p := list("superadmin")
	for _, tk := range p.Tokens {
		var want string
		switch tk.ID {
		case c:
			want = "5 researcher 3 gw_admin"
		case 1:
			want = "0 Shared 1 admin"
		case 2:
			want = "0 Shared 2 ops"
		default:
			continue
		}
		if got := fmt.Sprint(tk.Owner.ID, " ", tk.Owner.Name, " ", tk.MintedBy.ID, " ", tk.MintedBy.Name); got != want {
			t.Errorf("token %d: %s, want %s", tk.ID, got, want)
		}
	}
	// The index counts the viewer's tokens only.
	_, ix := e.page("member", "/mcp-admin")
	if m := ix["props"].(map[string]any); m["active_tokens"] != float64(2) || m["last_used"] != nil {
		t.Errorf("member's index %v", m)
	}

	// Routes on tokens the viewer does not manage are 404.
	for _, x := range []struct {
		role, method, path string
		body               map[string]any
	}{
		{"member", "POST", fmt.Sprintf("/api/mcp/tokens/%d/revoke", b), nil},
		{"member", "PATCH", fmt.Sprintf("/api/mcp/tokens/%d", d), map[string]any{"name": "x"}},
		{"member", "POST", "/api/mcp/tokens/1/revoke", nil},
		{"member", "POST", fmt.Sprintf("/api/mcp/tokens/%d/transfer", a), map[string]any{"owner_id": 3}},
		{"admin", "POST", fmt.Sprintf("/api/mcp/tokens/%d/revoke", d), nil},
		{"admin", "PATCH", "/api/mcp/tokens/1", map[string]any{"name": "x"}},
		{"admin", "POST", fmt.Sprintf("/api/mcp/tokens/%d/transfer", b), map[string]any{"owner_id": 5}},
		{"superadmin", "PATCH", "/api/mcp/tokens/999", map[string]any{"name": "x"}},
		{"superadmin", "POST", "/api/mcp/tokens/999/transfer", map[string]any{"owner_id": 3}},
	} {
		if st, bd, _ := e.call(x.role, x.method, x.path, x.body); st != 404 {
			t.Errorf("%s %s %s: %d %v", x.role, x.method, x.path, st, bd)
		}
	}
	if n := e.appOne("SELECT COUNT(*) FROM web_mcp_tokens WHERE revoked_at IS NOT NULL OR name = 'x'"); n != "1" {
		t.Errorf("%s tokens changed by refused requests (the fixture's revoked one is 1)", n)
	}

	// What each may do.
	e.want(204, "admin", "PATCH", fmt.Sprintf("/api/mcp/tokens/%d", a), map[string]any{"name": " renamed "})
	e.want(204, "admin", "PATCH", fmt.Sprintf("/api/mcp/tokens/%d", a), map[string]any{"name": "renamed"})
	if st, _, _ := e.call("admin", "PATCH", fmt.Sprintf("/api/mcp/tokens/%d", a), map[string]any{"name": " "}); st != 400 {
		t.Errorf("empty name %d", st)
	}
	e.want(204, "admin", "POST", fmt.Sprintf("/api/mcp/tokens/%d/revoke", c), nil)
	e.want(204, "member", "POST", fmt.Sprintf("/api/mcp/tokens/%d/revoke", a), nil)
	e.want(204, "superadmin", "PATCH", fmt.Sprintf("/api/mcp/tokens/%d", d), map[string]any{"name": "kernel ci"})
	e.want(204, "superadmin", "POST", fmt.Sprintf("/api/mcp/tokens/%d/transfer", d), map[string]any{"owner_id": 3})
	e.want(204, "superadmin", "POST", fmt.Sprintf("/api/mcp/tokens/%d/transfer", d), map[string]any{"owner_id": 3})
	e.want(204, "superadmin", "POST", "/api/mcp/tokens/1/transfer", map[string]any{"owner_id": 2})
	for _, body := range []map[string]any{{"owner_id": 7}, {"owner_id": 999}, {}} {
		if st, _, _ := e.call("superadmin", "POST", fmt.Sprintf("/api/mcp/tokens/%d/transfer", b), body); st != 400 {
			t.Errorf("transfer %v: %d", body, st)
		}
	}
	if got := owner(d) + " " + owner(1); got != "3/1 2/1" {
		t.Errorf("after the transfers %s", got)
	}
	if p := list("admin"); ids(p) != fmt.Sprint(d, " ", c, " ", b, " ", a) {
		t.Errorf("admin after the transfer: %s", ids(p))
	}
	want := []string{
		fmt.Sprintf("5 owner_set mcp_token %d", a),
		fmt.Sprintf("5 mcp_token_create token %d member's own", a),
		fmt.Sprintf("3 owner_set mcp_token %d", b),
		fmt.Sprintf("3 mcp_token_create token %d admin's own", b),
		fmt.Sprintf("3 owner_set mcp_token %d", c),
		fmt.Sprintf("3 mcp_token_create token %d for 5 for the member", c),
		fmt.Sprintf("1 owner_set mcp_token %d", d),
		fmt.Sprintf("1 mcp_token_create token %d for 6 for mod_kernel", d),
		fmt.Sprintf("3 mcp_token_rename token %d member's own -> renamed", a),
		fmt.Sprintf("3 mcp_token_revoke token %d for the member", c),
		fmt.Sprintf("5 mcp_token_revoke token %d renamed", a),
		fmt.Sprintf("1 mcp_token_rename token %d for mod_kernel -> kernel ci", d),
		fmt.Sprintf("1 owner_transfer mcp_token %d from 6 to 3", d),
		fmt.Sprintf("1 mcp_token_transfer token %d from 6 to 3", d),
		"1 owner_set mcp_token 1",
		"1 mcp_token_transfer token 1 from 0 to 2",
	}
	if got := e.audits(); strings.Join(got, "\n") != strings.Join(want, "\n") {
		t.Errorf("audit\n%s\nwant\n%s", strings.Join(got, "\n"), strings.Join(want, "\n"))
	}

	// The member with mcp_tokens has the tokens, not the groups.
	if st, _ := e.page("member", "/mcp-admin/groups"); st != 404 {
		t.Errorf("member's groups page %d", st)
	}
	if st, _, _ := e.call("member", "POST", "/api/mcp/groups", map[string]any{"group_id": -1001000000002}); st != 404 {
		t.Errorf("member exposing a group %d", st)
	}
	// Without mcp_tokens, nothing: not even its own tokens.
	if _, err := e.app.Exec("DELETE FROM web_member_abilities WHERE web_user_id = 5"); err != nil {
		t.Fatal(err)
	}
	for _, pth := range []string{"/mcp-admin", "/mcp-admin/tokens"} {
		if st, _ := e.page("member", pth); st != 404 {
			t.Errorf("member without mcp_tokens %s: %d", pth, st)
		}
	}
	if st, _, _ := e.call("member", "PATCH", fmt.Sprintf("/api/mcp/tokens/%d", a), map[string]any{"name": "y"}); st != 404 {
		t.Errorf("member without mcp_tokens renaming its token: %d", st)
	}
	// Its tokens stay with it; the admin still manages them.
	if p := list("admin"); !strings.Contains(" "+ids(p)+" ", fmt.Sprint(" ", a, " ")) {
		t.Errorf("admin lost the member's token: %s", ids(p))
	}
}
