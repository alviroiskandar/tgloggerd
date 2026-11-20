// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package logdb

import (
	"database/sql"
	"strings"
	"testing"

	"tgloggerd/webapi/internal/scope"
	"tgloggerd/webapi/internal/testdb"
)

// roleArchive fills db with 3000 files, every 30th the picture of a
// link preview of a text message in GNU/Weeb (one in thirty, a share
// like a real archive's), and every 97th plus 5 a video's still
// thumbnail.
func roleArchive(t *testing.T, db *sql.DB) {
	t.Helper()
	for _, q := range []string{
		"INSERT INTO telegram_files (id, tg_file_id, file_type, file_size, created_at) " +
			"WITH RECURSIVE d (x) AS (SELECT 0 UNION ALL SELECT x + 1 FROM d WHERE x < 99), " +
			"n (i) AS (SELECT a.x * 100 + b.x + 1 FROM d a, d b) " +
			"SELECT 7000000 + i, CONCAT('plan-', i), 'photo', i, " +
			"'2025-01-01' + INTERVAL i MINUTE FROM n WHERE i <= 3000",
		"INSERT INTO telegram_group_messages (id, chat_id, message_id, date, content_type, " +
			"text, file_id) " +
			"WITH RECURSIVE n (i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 100) " +
			"SELECT 7000000 + i, -1001000000001, (800000 + i) << 20, 1794000000 + i, 'text', " +
			"'https://example.org/', NULL FROM n",
		"INSERT INTO telegram_link_previews (group_message_id, url, display_url, photo_file_id) " +
			"SELECT id, 'https://example.org/', '', 7000000 + 30 * (id - 7000000) " +
			"FROM telegram_group_messages WHERE id BETWEEN 7000001 AND 7000100",
		"INSERT INTO telegram_message_files (group_message_id, role, position, file_id) " +
			"SELECT id, 'thumbnail', 0, 7000000 + 97 * (id - 7000000) + 5 " +
			"FROM telegram_group_messages WHERE id BETWEEN 7000001 AND 7000030",
		"ANALYZE TABLE telegram_files, telegram_group_messages, telegram_link_previews, " +
			"telegram_message_files",
	} {
		if _, err := db.Exec(q); err != nil {
			t.Fatalf("%s: %v", q, err)
		}
	}
}

// A link preview's picture is a few percent of the files: a page of
// them reads the files in the order asked for and tests each by its
// preview (a dependent subquery, the LIMIT ending the scan), rather
// than reading every preview, looking each file up and sorting them
// all. A count, an order no index gives and a deep page read every
// match anyway and keep the semijoin.
func TestFileRolePlans(t *testing.T) {
	t.Parallel()
	db := testdb.New(t, testdb.Logger, "sample").DB
	roleArchive(t, db)
	member := scope.For(scope.Viewer{ID: 5, Role: scope.Member,
		GrantedGroups: []int64{-1001000000001}})
	lp := []Cond{{C: "role", O: "=", V: "link_preview", HasV: true}}
	const hint = "NO_SEMIJOIN"

	for _, c := range []struct {
		name   string
		sc     scope.Scope
		req    SearchRequest
		driven bool
	}{
		{"default", scope.Everything(), SearchRequest{Conds: lp, Limit: 24}, true},
		{"created_at", scope.Everything(), SearchRequest{Conds: lp, Limit: 24, Sort: "created_at"}, true},
		{"id asc", scope.Everything(), SearchRequest{Conds: lp, Limit: 24, Sort: "id", Order: "asc"}, true},
		{"member", member, SearchRequest{Conds: lp, Limit: 24, Sort: "created_at"}, true},
		{"name", scope.Everything(), SearchRequest{Conds: lp, Limit: 24, Sort: "name"}, false},
		{"created_at asc", scope.Everything(),
			SearchRequest{Conds: lp, Limit: 24, Sort: "created_at", Order: "asc"}, false},
		{"deep", scope.Everything(), SearchRequest{Conds: lp, Limit: 24, Offset: 2000}, false},
	} {
		b, err := Files.build(c.sc, c.req)
		if err != nil {
			t.Fatalf("%s: %v", c.name, err)
		}
		if strings.Contains(b.countSQL, hint) {
			t.Errorf("%s: the count has the hint: %s", c.name, b.countSQL)
		}
		if strings.Contains(b.pageSQL, hint) != c.driven {
			t.Errorf("%s: hint in the page %v, want %v: %s", c.name, !c.driven, c.driven, b.pageSQL)
		}
		if !c.driven {
			continue
		}
		plan := planOf(t, db, b.pageSQL, b.pageArgs()...)
		t.Logf("%s page: %v", c.name, plan)
		var kind string
		for _, r := range plan {
			if r[0] == "x" {
				kind = r[2]
			}
		}
		if kind != "DEPENDENT SUBQUERY" {
			t.Errorf("%s: the preview is read as %q, not row by row: %v", c.name, kind, plan)
		}
		// By id, the primary key gives the order; a table this small
		// is sorted rather than read through idx_files_created_at.
		if k, _ := keyOf(plan, "f"); c.req.Sort != "created_at" && k != "PRIMARY" {
			t.Errorf("%s: files read by %q, not in the order asked for: %v", c.name, k, plan)
		}
	}
}
