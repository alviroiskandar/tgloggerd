// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"encoding/json"
	"strconv"
	"strings"
	"testing"
)

// The user tools of a token owned by a member or an admin describe only
// the users its owner sees on the web
// (decisions/2026-11-22-mcp-token-owner-view.md), on the "mcp" fixture.
// Rust Belajar (mcpG2) has messages of Rina (9300000001), Tono
// (9300000002) and Maya (9300000005); CI Bot (9300000003) writes only in
// Kernel Hackers ID (mcpG1). No user has the id mcpNoUser.

const mcpNoUser = int64(9399999999)

// userAnswers is what the three user tools answer for user id, each
// with the id replaced by "<id>", so a hidden user and an unknown one
// compare equal.
func (e *mcpEnv) userAnswers(id int64) map[string]string {
	e.t.Helper()
	out := map[string]string{}
	for tool, args := range map[string]map[string]any{
		"telegram_get_user":         {"user_id": id},
		"telegram_get_user_history": {"user_id": id},
		"telegram_get_users": {"filter": map[string]any{"field": "user_id", "op": "in",
			"value": []any{id}}},
	} {
		b, _ := json.Marshal(e.call(tool, args))
		out[tool] = strings.ReplaceAll(string(b), strconv.FormatInt(id, 10), "<id>")
	}
	return out
}

// usersListed is the user ids telegram_get_users answers for ids.
func (e *mcpEnv) usersListed(ids ...int64) []int64 {
	e.t.Helper()
	vs := []any{}
	for _, id := range ids {
		vs = append(vs, id)
	}
	r := sc(e.call("telegram_get_users", map[string]any{"filter": map[string]any{
		"field": "user_id", "op": "in", "value": vs}, "limit": 200}))
	var out []int64
	us, _ := r["users"].([]any)
	for _, u := range us {
		out = append(out, int64(u.(map[string]any)["user_id"].(float64)))
	}
	return out
}

func TestMCPMemberTokenUserTools(t *testing.T) {
	t.Parallel()
	e := newMCPEnv(t, MCPConfig{})
	// A history row for the hidden user, so its empty answer means
	// something.
	e.lgExec("INSERT INTO telegram_user_hist_bio (user_id, bio, created_at) VALUES " +
		"(9300000003, 'beep', '2025-02-01 00:00:00')")
	super := e.userAnswers(9300000003)
	if !strings.Contains(super["telegram_get_user"], "CI Bot") ||
		!strings.Contains(super["telegram_get_user_history"], "beep") ||
		!strings.Contains(super["telegram_get_users"], "CI Bot") {
		t.Fatalf("superadmin's token does not describe CI Bot: %v", super)
	}

	e.memberToken(mcpG2)
	unknown := e.userAnswers(mcpNoUser)
	hidden := e.userAnswers(9300000003)
	for tool, a := range unknown {
		if hidden[tool] != a {
			t.Errorf("%s: hidden user %s, unknown user %s", tool, hidden[tool], a)
		}
	}
	if v := e.userAnswers(9300000001)["telegram_get_user_history"]; !strings.Contains(v, "Kernel tinkerer") {
		t.Errorf("visible user's history: %s", v)
	}
	if got := e.usersListed(9300000001, 9300000002, 9300000003, 9300000005, mcpNoUser); len(got) != 3 ||
		got[0] != 9300000001 || got[1] != 9300000002 || got[2] != 9300000005 {
		t.Errorf("get_users within the grants: %v", got)
	}
	// A grant withdrawn counts at once: no user is seen.
	e.webExec("DELETE FROM web_member_grant_groups WHERE web_user_id = 5")
	if got := e.usersListed(9300000001, 9300000003); len(got) != 0 {
		t.Errorf("get_users without grants: %v", got)
	}
	if a := e.userAnswers(9300000001)["telegram_get_user"]; a != unknown["telegram_get_user"] {
		t.Errorf("get_user without grants: %s", a)
	}
	// Back with a superadmin, every user again, byte for byte.
	e.webExec("UPDATE web_resource_relations SET user_id = 1 WHERE resource_type = 'mcp_token' " +
		"AND resource_id = 1")
	if a := e.userAnswers(9300000003); a["telegram_get_user"] != super["telegram_get_user"] ||
		a["telegram_get_user_history"] != super["telegram_get_user_history"] ||
		a["telegram_get_users"] != super["telegram_get_users"] {
		t.Errorf("superadmin's token after a member's: %v", a)
	}
}

// An admin sees every user on the web, hidden groups or not, so its
// token's user tools answer as a superadmin's.
func TestMCPAdminTokenUserTools(t *testing.T) {
	t.Parallel()
	e := newMCPEnv(t, MCPConfig{})
	super := e.userAnswers(9300000003)
	e.adminToken(mcpG1, mcpG2)
	if a := e.userAnswers(9300000003); a["telegram_get_user"] != super["telegram_get_user"] ||
		a["telegram_get_user_history"] != super["telegram_get_user_history"] ||
		a["telegram_get_users"] != super["telegram_get_users"] {
		t.Errorf("admin's token: %v\nsuperadmin's %v", a, super)
	}
}
