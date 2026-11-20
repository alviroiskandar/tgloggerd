// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package logdb

import (
	"context"
	"database/sql"
	"strings"

	"tgloggerd/webapi/internal/scope"
)

// The MCP exposure allowlist, telegram_public_groups (web/docs/mcp.md,
// migration 000022), as the Drogon dao::mcp keeps it. The web's account
// is granted INSERT and DELETE on it, and UPDATE on the note alone: a
// row is a decision to expose a group, added or withdrawn; only the
// reason written down for it may be reworded later, never who made the
// decision or when. Every read takes the scope, so a group hidden from the viewer is neither
// listed nor counted nor changed (web-accounts.md section 4).

// ExposedGroup is an exposed group the scope sees.
type ExposedGroup struct {
	Group Group
	Note  string
	Added int64 // unix seconds
	// PublicNow: the group owns an active public username. A hint
	// for the admin, never the gate.
	PublicNow bool
}

// ExposedGroups lists the exposed groups the scope sees, newest first.
func ExposedGroups(ctx context.Context, db *sql.DB, sc scope.Scope) ([]ExposedGroup, error) {
	gf, ga := sc.GroupFilter("p.group_id")
	rows, err := db.QueryContext(ctx, "SELECT p.group_id, p.note, UNIX_TIMESTAMP(p.created_at), "+
		"EXISTS (SELECT 1 FROM telegram_group_usernames gu WHERE gu.group_id = p.group_id "+
		"AND gu.kind = 'active') FROM telegram_public_groups p "+
		"JOIN telegram_groups g ON g.id = p.group_id WHERE "+gf+
		" ORDER BY p.created_at DESC, p.group_id", ga...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var (
		out []ExposedGroup
		ids []int64
	)
	for rows.Next() {
		var e ExposedGroup
		if err := rows.Scan(&e.Group.ID, &e.Note, &e.Added, &e.PublicNow); err != nil {
			return nil, err
		}
		out = append(out, e)
		ids = append(ids, e.Group.ID)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	groups, err := groupsByID(ctx, db, ids)
	if err != nil {
		return nil, err
	}
	for i := range out {
		if g, ok := groups[out[i].Group.ID]; ok {
			out[i].Group = g
		}
	}
	if out == nil {
		out = []ExposedGroup{}
	}
	return out, nil
}

// ExposedStats counts the exposed groups the scope sees and their
// messages.
func ExposedStats(ctx context.Context, db *sql.DB, sc scope.Scope) (groups int, messages int64, err error) {
	gf, ga := sc.GroupFilter("p.group_id")
	err = db.QueryRowContext(ctx, "SELECT COUNT(*), COALESCE(SUM(g.msg_count), 0) "+
		"FROM telegram_public_groups p JOIN telegram_groups g ON g.id = p.group_id WHERE "+gf,
		ga...).Scan(&groups, &messages)
	return groups, messages, err
}

// IsExposed is whether a group is on the allowlist.
func IsExposed(ctx context.Context, db *sql.DB, id int64) (bool, error) {
	var n int
	err := db.QueryRowContext(ctx, "SELECT COUNT(*) FROM telegram_public_groups "+
		"WHERE group_id = ?", id).Scan(&n)
	return n > 0, err
}

// ExposeGroup puts a group the caller has checked on the allowlist;
// false when it was there already (a race between two admins, in
// which doing nothing is right). note is at most 255 characters.
func ExposeGroup(ctx context.Context, db *sql.DB, id int64, note string, by uint64) (bool, error) {
	res, err := db.ExecContext(ctx, "INSERT IGNORE INTO telegram_public_groups "+
		"(group_id, note, added_by) VALUES (?, ?, ?)", id, note, by)
	if err != nil {
		return false, err
	}
	n, err := res.RowsAffected()
	return n > 0, err
}

// UnexposeGroup takes a group off the allowlist; false when it was not
// on it.
func UnexposeGroup(ctx context.Context, db *sql.DB, id int64) (bool, error) {
	res, err := db.ExecContext(ctx, "DELETE FROM telegram_public_groups WHERE group_id = ?", id)
	if err != nil {
		return false, err
	}
	n, err := res.RowsAffected()
	return n > 0, err
}

// SetExposedNote replaces the note of an exposed group the caller has
// checked and answers the note it had; false when the group is not
// exposed. Who exposed the group and when stay as they are. note is at
// most 255 characters.
func SetExposedNote(ctx context.Context, db *sql.DB, id int64, note string) (string, bool, error) {
	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return "", false, err
	}
	defer tx.Rollback()
	var old string
	err = tx.QueryRowContext(ctx, "SELECT note FROM telegram_public_groups "+
		"WHERE group_id = ? FOR UPDATE", id).Scan(&old)
	if err == sql.ErrNoRows {
		return "", false, nil
	}
	if err != nil {
		return "", false, err
	}
	if old != note {
		if _, err := tx.ExecContext(ctx, "UPDATE telegram_public_groups SET note = ? "+
			"WHERE group_id = ?", note, id); err != nil {
			return "", false, err
		}
	}
	return old, true, tx.Commit()
}

// ExposedGroupIDs is every exposed group, whoever views it: whether a
// token's scope entry still reaches its group (decisions/2026-11-20-
// mcp-token-scope.md) does not depend on the viewer.
func ExposedGroupIDs(ctx context.Context, db *sql.DB) (map[int64]bool, error) {
	rows, err := db.QueryContext(ctx, "SELECT group_id FROM telegram_public_groups")
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[int64]bool{}
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		out[id] = true
	}
	return out, rows.Err()
}

// UserNames is the name of each user of ids the archive knows, first
// and last name, else its username, else "": the private chats of MCP
// token scopes, whose chat id is the peer's user id.
func UserNames(ctx context.Context, db *sql.DB, ids []int64) (map[int64]string, error) {
	out := map[int64]string{}
	if len(ids) == 0 {
		return out, nil
	}
	args := make([]any, len(ids))
	for i, id := range ids {
		args[i] = id
	}
	rows, err := db.QueryContext(ctx, "SELECT u.id, TRIM(CONCAT(COALESCE(u.first_name, ''), ' ', "+
		"COALESCE(u.last_name, ''))), COALESCE((SELECT x.username FROM telegram_user_usernames x "+
		"WHERE x.user_id = u.id AND x.kind = 'active' ORDER BY x.position LIMIT 1), '') "+
		"FROM telegram_users u WHERE u.id IN (?"+strings.Repeat(", ?", len(ids)-1)+")", args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	for rows.Next() {
		var (
			id         int64
			name, user string
		)
		if err := rows.Scan(&id, &name, &user); err != nil {
			return nil, err
		}
		if name == "" {
			name = user
		}
		out[id] = name
	}
	return out, rows.Err()
}
