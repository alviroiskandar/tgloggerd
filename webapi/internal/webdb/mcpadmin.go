// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package webdb

import (
	"context"
	"database/sql"
	"errors"
	"time"
)

// The MCP bearer tokens (web_mcp_tokens, migration 000003; only their
// SHA-256 is stored), as the Drogon web's dao::mcp keeps them.

// MCPToken is a web_mcp_tokens row, without its digest, with its
// owner (web_resource_relations).
type MCPToken struct {
	ID   uint64
	Name string
	// OwnerID is the owning account, 0 for a token without an owner
	// record (the superadmins'). MinterID is the account that minted
	// it (web_mcp_tokens.web_user_id), which its requests are
	// attributed to.
	OwnerID  uint64
	MinterID uint64
	Created  time.Time
	LastUsed *time.Time
	Revoked  *time.Time
}

func unixPtr(v sql.NullFloat64) *time.Time {
	if !v.Valid {
		return nil
	}
	t := time.Unix(int64(v.Float64), 0).UTC()
	return &t
}

const mcpTokenColumns = "SELECT t.id, t.name, COALESCE(o.user_id, 0), t.web_user_id, " +
	"UNIX_TIMESTAMP(t.created_at), UNIX_TIMESTAMP(t.last_used_at), " +
	"UNIX_TIMESTAMP(t.revoked_at) FROM web_mcp_tokens t " +
	"LEFT JOIN web_resource_relations o ON o.resource_type = ? AND o.resource_id = t.id " +
	"AND o.relation = ? "

func scanMCPToken(row scanner) (MCPToken, error) {
	var (
		t             MCPToken
		created       float64
		used, revoked sql.NullFloat64
	)
	if err := row.Scan(&t.ID, &t.Name, &t.OwnerID, &t.MinterID, &created, &used, &revoked); err != nil {
		return t, err
	}
	t.Created = time.Unix(int64(created), 0).UTC()
	t.LastUsed, t.Revoked = unixPtr(used), unixPtr(revoked)
	return t, nil
}

// ListMCPTokens is every token, newest first; the caller keeps those
// the viewer manages (scope.ManagesMCPToken).
func ListMCPTokens(ctx context.Context, db *sql.DB) ([]MCPToken, error) {
	rows, err := db.QueryContext(ctx, mcpTokenColumns+"ORDER BY t.id DESC",
		string(ResMCPToken), string(Owner))
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []MCPToken{}
	for rows.Next() {
		t, err := scanMCPToken(rows)
		if err != nil {
			return nil, err
		}
		out = append(out, t)
	}
	return out, rows.Err()
}

// GetMCPToken is one token, or ErrNoToken.
func GetMCPToken(ctx context.Context, db *sql.DB, id uint64) (MCPToken, error) {
	t, err := scanMCPToken(db.QueryRowContext(ctx, mcpTokenColumns+"WHERE t.id = ?",
		string(ResMCPToken), string(Owner), id))
	if errors.Is(err, sql.ErrNoRows) {
		return t, ErrNoToken
	}
	return t, err
}

// CreateMCPToken stores a token by its digest, minted by account
// minter and owned by account owner, with the owner record, its
// owner_set audit row and its scope (checked by the caller; none for
// every exposed group) in the same transaction, so a token never lacks
// an owner and never works, even once, wider than it was made. The
// token itself is never passed here, so it cannot be stored by
// accident.
func CreateMCPToken(ctx context.Context, db *sql.DB, minter, owner uint64, name string,
	sha256 []byte, scope []MCPScopeEntry, by Actor) (uint64, error) {
	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return 0, err
	}
	defer tx.Rollback()
	res, err := tx.ExecContext(ctx, "INSERT INTO web_mcp_tokens (web_user_id, name, token_sha256) "+
		"VALUES (?, ?, ?)", minter, name, sha256)
	if err != nil {
		return 0, err
	}
	id, err := res.LastInsertId()
	if err != nil {
		return 0, err
	}
	if err := setOwner(ctx, tx, Resource{ResMCPToken, id}, owner, by, ""); err != nil {
		return 0, err
	}
	if err := insertScope(ctx, tx, uint64(id), SortMCPScope(scope)); err != nil {
		return 0, err
	}
	return uint64(id), tx.Commit()
}

// RenameMCPToken sets a token's name and returns the one it had, or
// ErrNoToken.
func RenameMCPToken(ctx context.Context, db *sql.DB, id uint64, name string) (string, error) {
	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return "", err
	}
	defer tx.Rollback()
	var old string
	err = tx.QueryRowContext(ctx, "SELECT name FROM web_mcp_tokens WHERE id = ? FOR UPDATE", id).Scan(&old)
	if errors.Is(err, sql.ErrNoRows) {
		return "", ErrNoToken
	}
	if err != nil {
		return "", err
	}
	if old != name {
		if _, err := tx.ExecContext(ctx, "UPDATE web_mcp_tokens SET name = ? WHERE id = ?", name, id); err != nil {
			return "", err
		}
	}
	return old, tx.Commit()
}

// ErrNoToken is returned when no token has the id.
var ErrNoToken = errors.New("no such token")

// RevokeMCPToken stamps revoked_at, so the row stays auditable; true
// when it was not revoked before.
func RevokeMCPToken(ctx context.Context, db *sql.DB, id uint64) (bool, error) {
	res, err := db.ExecContext(ctx, "UPDATE web_mcp_tokens SET revoked_at = NOW() "+
		"WHERE id = ? AND revoked_at IS NULL", id)
	if err != nil {
		return false, err
	}
	n, err := res.RowsAffected()
	return n > 0, err
}

// AllMCPTokenIDs is every token, revoked ones too, for the
// assign-owner command.
func AllMCPTokenIDs(ctx context.Context, db *sql.DB) ([]int64, error) {
	rows, err := db.QueryContext(ctx, "SELECT id FROM web_mcp_tokens ORDER BY id")
	if err != nil {
		return nil, err
	}
	return scanIDs(rows)
}
