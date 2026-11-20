// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package webdb

import (
	"context"
	"errors"
	"slices"
	"testing"

	"tgloggerd/webapi/internal/testdb"
)

// A token's scope is stored sorted and once per entry, replaced as a
// whole, and reports whether a write changed it; deleting a federation
// leaves its entry behind, so the scope never empties by itself.
func TestMCPTokenScope(t *testing.T) {
	t.Parallel()
	db := testdb.New(t, testdb.Web, "accounts", "mcp_tokens", "federations")
	ctx := context.Background()

	if s, err := MCPTokenScope(ctx, db.DB, 1); err != nil || s != nil {
		t.Fatalf("unscoped token: %v %v", s, err)
	}
	in := []MCPScopeEntry{{ScopePrivate, 9300000001}, {ScopeGroup, -1002000000002},
		{ScopeFederation, 2}, {ScopeGroup, -1002000000001}, {ScopeGroup, -1002000000002}}
	want := []MCPScopeEntry{{ScopeFederation, 2}, {ScopeGroup, -1002000000002},
		{ScopeGroup, -1002000000001}, {ScopePrivate, 9300000001}}
	if changed, err := SetMCPTokenScope(ctx, db.DB, 1, in); err != nil || !changed {
		t.Fatalf("set: %v %v", changed, err)
	}
	if s, err := MCPTokenScope(ctx, db.DB, 1); err != nil || !slices.Equal(s, want) {
		t.Fatalf("stored %v %v, want %v", s, err, want)
	}
	if changed, err := SetMCPTokenScope(ctx, db.DB, 1, want); err != nil || changed {
		t.Errorf("the same scope again: changed %v %v", changed, err)
	}
	if got := MCPScopeText(want); got != "federation 2, group -1002000000002, group -1002000000001, private 9300000001" {
		t.Errorf("text %q", got)
	}
	if _, err := SetMCPTokenScope(ctx, db.DB, 99, want); !errors.Is(err, ErrNoToken) {
		t.Errorf("unknown token: %v", err)
	}

	// Every token's at once, for the list.
	if _, err := SetMCPTokenScope(ctx, db.DB, 2, []MCPScopeEntry{{ScopeFederation, 0}}); err != nil {
		t.Fatal(err)
	}
	all, err := MCPTokenScopes(ctx, db.DB)
	if err != nil || len(all) != 2 || !slices.Equal(all[1], want) || len(all[2]) != 1 {
		t.Fatalf("all scopes %v %v", all, err)
	}

	if _, err := db.Exec("DELETE FROM web_federations WHERE id = 2"); err != nil {
		t.Fatal(err)
	}
	if s, _ := MCPTokenScope(ctx, db.DB, 1); !slices.Equal(s, want) {
		t.Errorf("after deleting the federation: %v", s)
	}

	// Emptied, it is every exposed group again.
	if changed, err := SetMCPTokenScope(ctx, db.DB, 1, nil); err != nil || !changed {
		t.Fatalf("clear: %v %v", changed, err)
	}
	if s, _ := MCPTokenScope(ctx, db.DB, 1); s != nil {
		t.Errorf("cleared: %v", s)
	}
	if MCPScopeText(nil) != "all exposed groups" {
		t.Error("empty text")
	}

	// Minting writes the scope with the token.
	id, err := CreateMCPToken(ctx, db.DB, 1, 1, "scoped", make([]byte, 32),
		[]MCPScopeEntry{{ScopeGroup, -1}, {ScopeGroup, -1}}, By(1, ""))
	if err != nil {
		t.Fatal(err)
	}
	if s, _ := MCPTokenScope(ctx, db.DB, id); !slices.Equal(s, []MCPScopeEntry{{ScopeGroup, -1}}) {
		t.Errorf("minted scope %v", s)
	}
}
