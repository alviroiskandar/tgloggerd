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

// A count by role alone reads the role tables, not every file; beside
// another condition it counts through the WHERE as before. Both give
// the same numbers, for every scope.
func TestFileRoleCount(t *testing.T) {
	t.Parallel()
	db := testdb.New(t, testdb.Logger, "sample").DB
	roleArchive(t, db)
	// Previews and thumbnails in a hidden group, a private chat's
	// preview, and a preview without a picture yet.
	for _, q := range []string{
		"INSERT INTO telegram_group_messages (id, chat_id, message_id, date, content_type, text) " +
			"VALUES (7000201, -1001000000002, 810001 << 20, 1794000000, 'text', 'x'), " +
			"(7000202, -1001000000002, 810002 << 20, 1794000000, 'video', NULL), " +
			"(7000203, -1001000000001, 810003 << 20, 1794000000, 'text', 'y')",
		"INSERT INTO telegram_link_previews (group_message_id, url, display_url, photo_file_id) " +
			"VALUES (7000201, 'https://example.org/h', '', 7000031), " +
			"(7000203, 'https://example.org/n', '', NULL)",
		"INSERT INTO telegram_message_files (group_message_id, role, position, file_id) " +
			"VALUES (7000202, 'thumbnail', 0, 7000033), (7000202, 'cover', 0, 7000030)",
		"INSERT INTO telegram_private_messages (id, chat_id, message_id, date, content_type, text) " +
			"VALUES (7000301, 9299000002, 820001 << 20, 1794000000, 'text', 'z')",
		"INSERT INTO telegram_link_previews (private_message_id, url, display_url, photo_file_id) " +
			"VALUES (7000301, 'https://example.org/p', '', 7000032)",
	} {
		if _, err := db.Exec(q); err != nil {
			t.Fatalf("%s: %v", q, err)
		}
	}
	member := scope.For(scope.Viewer{ID: 5, Role: scope.Member,
		GrantedGroups: []int64{-1001000000001}})
	admin := scope.For(scope.Viewer{ID: 6, Role: scope.Admin,
		HideGroups: []int64{-1001000000002}})
	role := func(vals ...string) Cond {
		if len(vals) == 1 {
			return Cond{C: "role", O: "=", V: vals[0], HasV: true, N: "AND"}
		}
		return Cond{C: "role", O: "in", L: vals, IsList: true, HasV: true, N: "AND"}
	}
	// Every file of the archive has an id above 0, so the second
	// condition changes no count, only the way it is counted.
	other := Cond{C: "id", O: ">", V: "0", HasV: true}
	count := func(b *built) int64 {
		t.Helper()
		n, err := countInt(t.Context(), db, b.countSQL, b.countArgs()...)
		if err != nil {
			t.Fatalf("%s: %v", b.countSQL, err)
		}
		return n
	}
	for _, s := range []struct {
		name string
		sc   scope.Scope
	}{{"everything", scope.Everything()}, {"admin", admin}, {"member", member}} {
		for _, r := range []Cond{
			role("link_preview"), role("thumbnail"), role("cover"),
			role("cover", "link_preview"), role("thumbnail", "cover", "link_preview"),
		} {
			name := s.name + " " + r.V + strings.Join(r.L, ",")
			b, err := Files.build(s.sc, SearchRequest{Conds: []Cond{r}, Limit: 24})
			if err != nil {
				t.Fatalf("%s: %v", name, err)
			}
			plan := planOf(t, db, b.countSQL, b.countArgs()...)
			if _, ok := keyOf(plan, "f"); ok {
				t.Errorf("%s: the count reads telegram_files: %v", name, plan)
			}
			with, err := Files.build(s.sc, SearchRequest{Conds: []Cond{r, other}, Limit: 24})
			if err != nil {
				t.Fatalf("%s: %v", name, err)
			}
			if _, ok := keyOf(planOf(t, db, with.countSQL, with.countArgs()...), "f"); !ok {
				t.Errorf("%s: with another condition the count does not read the files: %s",
					name, with.countSQL)
			}
			// The cover is in the hidden group only.
			if a, w := count(b), count(with); a != w || a == 0 && (s.name == "everything" ||
				r.V != "cover") {
				t.Errorf("%s: %d alone, %d with another condition", name, a, w)
			}
		}
	}
	// What each scope sees of the previews: 100 in GNU/Weeb, one in
	// the hidden group, one in a private chat.
	for _, c := range []struct {
		sc   scope.Scope
		want int64
	}{{scope.Everything(), 102}, {admin, 101}, {member, 100}} {
		b, err := Files.build(c.sc, SearchRequest{Conds: []Cond{role("link_preview")}, Limit: 24})
		if err != nil {
			t.Fatal(err)
		}
		if n := count(b); n != c.want {
			t.Errorf("%v: %d previews, want %d", c.sc.Role(), n, c.want)
		}
	}
	// != and an OR keep counting through the WHERE.
	for _, req := range []SearchRequest{
		{Conds: []Cond{{C: "role", O: "!=", V: "cover", HasV: true}}},
		{Conds: []Cond{{C: "role", O: "=", V: "cover", HasV: true, N: "OR"}, other}},
	} {
		b, err := Files.build(scope.Everything(), req)
		if err != nil {
			t.Fatal(err)
		}
		if b.count != nil || !strings.Contains(b.countSQL, "FROM telegram_files f") {
			t.Errorf("%v: count %s", req.Conds, b.countSQL)
		}
	}
}
