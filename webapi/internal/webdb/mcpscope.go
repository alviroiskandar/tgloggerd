// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package webdb

import (
	"context"
	"database/sql"
	"slices"
	"strconv"
	"strings"
)

// The scope of each MCP token (web_mcp_token_scopes, web migration
// 000019; decisions/2026-11-20-mcp-token-scope.md): the federations,
// groups and private chats it may read. No rows is every exposed
// group. What an entry reaches is decided where it is applied
// (mcp.ResolveScope); these only store the entries.

// MCPScopeKind is what an entry's id names.
type MCPScopeKind string

const (
	ScopeFederation MCPScopeKind = "federation" // web_federations.id, 0 for Default
	ScopeGroup      MCPScopeKind = "group"      // a group's negative chat id
	ScopePrivate    MCPScopeKind = "private"    // a one-to-one chat: the peer user's id
)

// MaxMCPScopeEntries bounds a scope: every entry ends up a bound value
// of the statements the tools run.
const MaxMCPScopeEntries = 200

// MCPScopeEntry is one entry of a token's scope.
type MCPScopeEntry struct {
	Kind MCPScopeKind
	ID   int64
}

// kindRank is the stored order: the ENUM's, federations first.
func kindRank(k MCPScopeKind) int {
	switch k {
	case ScopeFederation:
		return 0
	case ScopeGroup:
		return 1
	}
	return 2
}

// SortMCPScope puts entries in the stored order (by kind, then id) and
// drops duplicates, so two scopes compare by ==.
func SortMCPScope(es []MCPScopeEntry) []MCPScopeEntry {
	out := slices.Clone(es)
	slices.SortFunc(out, func(a, b MCPScopeEntry) int {
		if d := kindRank(a.Kind) - kindRank(b.Kind); d != 0 {
			return d
		}
		switch {
		case a.ID < b.ID:
			return -1
		case a.ID > b.ID:
			return 1
		}
		return 0
	})
	return slices.Compact(out)
}

// MCPScopeText is a scope for an audit row: "federation 1, group
// -100..., private 9..." in the stored order, "all exposed groups"
// for none.
func MCPScopeText(es []MCPScopeEntry) string {
	if len(es) == 0 {
		return "all exposed groups"
	}
	parts := make([]string, len(es))
	for i, e := range es {
		parts[i] = string(e.Kind) + " " + strconv.FormatInt(e.ID, 10)
	}
	return strings.Join(parts, ", ")
}

func scanScope(rows *sql.Rows, add func(uint64, MCPScopeEntry)) error {
	defer rows.Close()
	for rows.Next() {
		var (
			tok uint64
			e   MCPScopeEntry
		)
		if err := rows.Scan(&tok, &e.Kind, &e.ID); err != nil {
			return err
		}
		add(tok, e)
	}
	return rows.Err()
}

// MCPTokenScope is one token's scope in the stored order; nil for
// none.
func MCPTokenScope(ctx context.Context, db Querier, token uint64) ([]MCPScopeEntry, error) {
	rows, err := db.QueryContext(ctx, "SELECT token_id, kind, target_id FROM web_mcp_token_scopes "+
		"WHERE token_id = ? ORDER BY kind, target_id", token)
	if err != nil {
		return nil, err
	}
	var out []MCPScopeEntry
	err = scanScope(rows, func(_ uint64, e MCPScopeEntry) { out = append(out, e) })
	return out, err
}

// MCPTokenScopes is every token's scope, by token id, for the token
// list.
func MCPTokenScopes(ctx context.Context, db Querier) (map[uint64][]MCPScopeEntry, error) {
	rows, err := db.QueryContext(ctx, "SELECT token_id, kind, target_id FROM web_mcp_token_scopes "+
		"ORDER BY token_id, kind, target_id")
	if err != nil {
		return nil, err
	}
	out := map[uint64][]MCPScopeEntry{}
	err = scanScope(rows, func(tok uint64, e MCPScopeEntry) { out[tok] = append(out[tok], e) })
	return out, err
}

// SetMCPTokenScope replaces a token's scope with es (checked by the
// caller) and reports whether it changed. It locks the token's row, so
// two writers of the same token's scope take turns.
func SetMCPTokenScope(ctx context.Context, db *sql.DB, token uint64, es []MCPScopeEntry) (bool, error) {
	es = SortMCPScope(es)
	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return false, err
	}
	defer tx.Rollback()
	var id uint64
	err = tx.QueryRowContext(ctx, "SELECT id FROM web_mcp_tokens WHERE id = ? FOR UPDATE", token).Scan(&id)
	if err == sql.ErrNoRows {
		return false, ErrNoToken
	}
	if err != nil {
		return false, err
	}
	old, err := MCPTokenScope(ctx, tx, token)
	if err != nil {
		return false, err
	}
	if slices.Equal(old, es) {
		return false, nil
	}
	if _, err := tx.ExecContext(ctx, "DELETE FROM web_mcp_token_scopes WHERE token_id = ?", token); err != nil {
		return false, err
	}
	if err := insertScope(ctx, tx, token, es); err != nil {
		return false, err
	}
	return true, tx.Commit()
}

// insertScope writes es, sorted, as the scope of a token that has
// none.
func insertScope(ctx context.Context, tx *sql.Tx, token uint64, es []MCPScopeEntry) error {
	if len(es) == 0 {
		return nil
	}
	q := "INSERT INTO web_mcp_token_scopes (token_id, kind, target_id) VALUES " +
		strings.TrimSuffix(strings.Repeat("(?, ?, ?), ", len(es)), ", ")
	args := make([]any, 0, 3*len(es))
	for _, e := range es {
		args = append(args, token, string(e.Kind), e.ID)
	}
	_, err := tx.ExecContext(ctx, q, args...)
	return err
}
