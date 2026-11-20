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

// The scope of an MCP token through the token administration
// (decisions/2026-11-20-mcp-token-scope.md), in adminEnv: the sample
// exposes GNU/Weeb (-1001000000001), Rust Programming and CTF
// Practice, not Linux Kernel Indonesia (-1001000000002); Dewi Kusuma
// is user 9299000001. Tokens 1 and 2 have no owner record, so they are
// the superadmins'.
func TestMcpTokenScopeAPI(t *testing.T) {
	t.Parallel()
	e := newAdminEnv(t)
	if _, err := e.app.Exec("INSERT INTO web_member_abilities (web_user_id, ability) " +
		"VALUES (5, 'mcp_tokens')"); err != nil {
		t.Fatal(err)
	}
	const gnuweeb, kernel, dewi = int64(-1001000000001), int64(-1001000000002), int64(9299000001)
	ent := func(kind string, id int64) map[string]any { return map[string]any{"kind": kind, "id": id} }

	type entry struct {
		Kind   string `json:"kind"`
		ID     int64  `json:"id"`
		Name   string `json:"name"`
		Active bool   `json:"active"`
	}
	type props struct {
		Tokens []struct {
			ID    uint64  `json:"id"`
			Scope []entry `json:"scope"`
		} `json:"tokens"`
		ScopeOptions struct {
			Federations []struct {
				ID   uint64 `json:"id"`
				Name string `json:"name"`
			} `json:"federations"`
			Groups []struct {
				ID   int64  `json:"id"`
				Name string `json:"name"`
			} `json:"groups"`
			Private bool `json:"private"`
		} `json:"scope_options"`
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
	scopeOf := func(role string, id uint64) string {
		t.Helper()
		for _, tk := range list(role).Tokens {
			if tk.ID == id {
				var out []string
				for _, x := range tk.Scope {
					out = append(out, fmt.Sprintf("%s %d %q %v", x.Kind, x.ID, x.Name, x.Active))
				}
				return strings.Join(out, ", ")
			}
		}
		t.Fatalf("%s does not list token %d", role, id)
		return ""
	}
	stored := func(id uint64) string {
		t.Helper()
		return e.appOne("SELECT GROUP_CONCAT(CONCAT(kind, ' ', target_id) ORDER BY kind, target_id "+
			"SEPARATOR ', ') FROM web_mcp_token_scopes WHERE token_id = ?", id)
	}

	// A superadmin mints with a scope; the list names each entry.
	b := e.want(201, "superadmin", "POST", "/api/mcp/tokens", map[string]any{"name": "scoped",
		"scope": []any{ent("private", dewi), ent("group", gnuweeb), ent("federation", 0),
			ent("group", gnuweeb)}})
	id := uint64(b["id"].(float64))
	if got := stored(id); got != "federation 0, group -1001000000001, private 9299000001" {
		t.Errorf("stored %q", got)
	}
	if got := scopeOf("superadmin", id); got != `federation 0 "Default" true, `+
		`group -1001000000001 "GNU/Weeb" true, private 9299000001 "Dewi Kusuma" true` {
		t.Errorf("listed %s", got)
	}
	o := list("superadmin").ScopeOptions
	if !o.Private || len(o.Federations) == 0 || o.Federations[0].Name != "Default" || len(o.Groups) != 3 {
		t.Errorf("superadmin's options %+v", o)
	}
	for _, g := range o.Groups {
		if g.ID == kernel {
			t.Errorf("options offer an unexposed group: %+v", o.Groups)
		}
	}
	if list("member").ScopeOptions.Private || list("admin").ScopeOptions.Private {
		t.Error("private chats offered to a non-superadmin")
	}
	if got := list("superadmin").Tokens[1].Scope; len(got) != 0 {
		t.Errorf("an unscoped token's scope %v", got)
	}

	// An admin may not give a private chat, to any token; nothing is
	// minted or changed.
	before := e.appOne("SELECT COUNT(*) FROM web_mcp_tokens")
	st, bd, _ := e.call("admin", "POST", "/api/mcp/tokens", map[string]any{"name": "x",
		"scope": []any{ent("private", dewi)}})
	if st != 403 || bd["error"] != "forbidden" || !strings.Contains(fmt.Sprint(bd["message"]), "superadmins") {
		t.Errorf("admin minting a private chat: %d %v", st, bd)
	}
	if e.appOne("SELECT COUNT(*) FROM web_mcp_tokens") != before {
		t.Error("a token minted by a refused request")
	}
	// An admin's token takes an unexposed group, inactive until it is
	// exposed.
	b = e.want(201, "admin", "POST", "/api/mcp/tokens", map[string]any{"name": "admin's",
		"scope": []any{ent("group", kernel)}})
	adm := uint64(b["id"].(float64))
	if got := scopeOf("admin", adm); got != `group -1001000000002 "Linux Kernel Indonesia" false` {
		t.Errorf("admin's token %s", got)
	}
	// Nor may a superadmin give one to a token an admin owns.
	st, bd, _ = e.call("superadmin", "PATCH", fmt.Sprintf("/api/mcp/tokens/%d", adm),
		map[string]any{"scope": []any{ent("private", dewi)}})
	if st != 403 || bd["error"] != "forbidden" || !strings.Contains(fmt.Sprint(bd["message"]), "owns") {
		t.Errorf("private chat on an admin's token: %d %v", st, bd)
	}

	// Bad entries are 400 and change nothing.
	many := []any{}
	for i := 0; i < 201; i++ {
		many = append(many, ent("group", -1-int64(i)))
	}
	for _, sc := range [][]any{
		{ent("chat", gnuweeb)},
		{map[string]any{"kind": "group"}},
		{ent("group", 5)},
		{ent("group", -1009999999999)},
		{ent("federation", -1)},
		{ent("federation", 999)},
		{ent("private", 1)},
		{ent("private", -5)},
		many,
	} {
		st, bd, _ := e.call("superadmin", "PATCH", fmt.Sprintf("/api/mcp/tokens/%d", id),
			map[string]any{"scope": sc, "name": "renamed"})
		if st != 400 || bd["error"] != "bad_request" {
			t.Errorf("scope %v: %d %v", sc[0], st, bd)
		}
	}
	if got := stored(id); got != "federation 0, group -1001000000001, private 9299000001" ||
		e.appOne("SELECT name FROM web_mcp_tokens WHERE id = ?", id) != "scoped" {
		t.Errorf("after refused writes: %q", got)
	}
	e.want(400, "superadmin", "PATCH", fmt.Sprintf("/api/mcp/tokens/%d", id), map[string]any{})
	// A token the viewer does not manage is 404, scope or not.
	e.want(404, "member", "PATCH", fmt.Sprintf("/api/mcp/tokens/%d", id),
		map[string]any{"scope": []any{}})

	// The scope alone, the same scope again, then name and scope.
	e.want(204, "superadmin", "PATCH", fmt.Sprintf("/api/mcp/tokens/%d", id),
		map[string]any{"scope": []any{ent("group", gnuweeb)}})
	e.want(204, "superadmin", "PATCH", fmt.Sprintf("/api/mcp/tokens/%d", id),
		map[string]any{"scope": []any{ent("group", gnuweeb)}})
	e.want(204, "superadmin", "PATCH", fmt.Sprintf("/api/mcp/tokens/%d", id),
		map[string]any{"name": "dm reader", "scope": []any{ent("private", dewi), ent("group", gnuweeb)}})
	if got := stored(id); got != "group -1001000000001, private 9299000001" ||
		e.appOne("SELECT name FROM web_mcp_tokens WHERE id = ?", id) != "dm reader" {
		t.Errorf("after name and scope: %q", got)
	}

	// Entries follow what they name: a withdrawn group, a deleted
	// federation, a private chat on a token passed to an admin.
	e.want(204, "superadmin", "DELETE", fmt.Sprintf("/api/mcp/groups/%d", gnuweeb), nil)
	if _, err := e.app.Exec("INSERT INTO web_federations (id, name) VALUES (50, 'Gone soon')"); err != nil {
		t.Fatal(err)
	}
	e.want(204, "superadmin", "PATCH", "/api/mcp/tokens/1",
		map[string]any{"scope": []any{ent("federation", 50)}})
	if _, err := e.app.Exec("DELETE FROM web_federations WHERE id = 50"); err != nil {
		t.Fatal(err)
	}
	if got := scopeOf("superadmin", 1); got != `federation 50 "" false` {
		t.Errorf("deleted federation %s", got)
	}
	e.want(204, "superadmin", "POST", fmt.Sprintf("/api/mcp/tokens/%d/transfer", id),
		map[string]any{"owner_id": 3})
	if got := scopeOf("superadmin", id); got != `group -1001000000001 "GNU/Weeb" false, `+
		`private 9299000001 "Dewi Kusuma" false` {
		t.Errorf("after withdrawing and transferring: %s", got)
	}
	// Its new owner must drop the private entry to save the scope.
	st, _, _ = e.call("admin", "PATCH", fmt.Sprintf("/api/mcp/tokens/%d", id),
		map[string]any{"scope": []any{ent("private", dewi), ent("group", kernel)}})
	if st != 403 {
		t.Errorf("admin keeping the private entry: %d", st)
	}
	e.want(204, "admin", "PATCH", fmt.Sprintf("/api/mcp/tokens/%d", id),
		map[string]any{"scope": []any{ent("group", kernel)}})
	// Emptied: every exposed group again.
	e.want(204, "admin", "PATCH", fmt.Sprintf("/api/mcp/tokens/%d", id), map[string]any{"scope": []any{}})
	if got := stored(id); got != "" {
		t.Errorf("emptied %q", got)
	}

	var scopes []string
	for _, a := range e.audits() {
		if strings.Contains(a, "mcp_token_scope") {
			scopes = append(scopes, a)
		}
	}
	want := []string{
		fmt.Sprintf("1 mcp_token_scope token %d: federation 0, group -1001000000001, private 9299000001", id),
		fmt.Sprintf("3 mcp_token_scope token %d: group -1001000000002", adm),
		fmt.Sprintf("1 mcp_token_scope token %d: group -1001000000001", id),
		fmt.Sprintf("1 mcp_token_scope token %d: group -1001000000001, private 9299000001", id),
		"1 mcp_token_scope token 1: federation 50",
		fmt.Sprintf("3 mcp_token_scope token %d: group -1001000000002", id),
		fmt.Sprintf("3 mcp_token_scope token %d: all exposed groups", id),
	}
	if strings.Join(scopes, "\n") != strings.Join(want, "\n") {
		t.Errorf("audit\n%s\nwant\n%s", strings.Join(scopes, "\n"), strings.Join(want, "\n"))
	}
}
