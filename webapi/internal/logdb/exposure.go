// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package logdb

import (
	"context"
	"database/sql"
)

// ExposedGroupsAmong is the groups among ids that are exposed (in
// telegram_public_groups), whose public chat anyone may read
// (OpenChat). Ids that are not groups' are never in it. It is the
// check of a few ids a live stream asks for, by the table's key.
func ExposedGroupsAmong(ctx context.Context, db *sql.DB, ids []int64) (map[int64]bool, error) {
	var groups []int64
	for _, id := range uniq(ids) {
		if id < 0 {
			groups = append(groups, id)
		}
	}
	out := map[int64]bool{}
	if len(groups) == 0 {
		return out, nil
	}
	rows, err := db.QueryContext(ctx, "SELECT group_id FROM telegram_public_groups "+
		"WHERE group_id IN ("+placeholders(len(groups))+")", int64Args(groups)...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		out[id] = true
	}
	return out, rows.Err()
}
