// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package logdb

import (
	"context"
	"database/sql"
	"strconv"
	"sync"
	"time"

	"tgloggerd/webapi/internal/scope"
)

// The group page's Files and Polls tabs: a group's messages that carry
// a file, or a poll, newest first by Telegram message id, a page at a
// time. Each page is one query on the group's messages, walked down the
// (chat_id, message_id) index, and one batch for the senders it names.
// The caller has checked that the scope sees the group, or that the
// group is exposed (logdb.OpenChat).

// TabPageSize is how many messages a page of the tabs holds.
const TabPageSize = 50

// FileKinds are the content types the Files tab lists, in the order of
// its sub tabs: the kinds of message whose main file is the message's
// content.
var FileKinds = []string{"photo", "video", "animation", "audio", "voice", "document",
	"sticker", "video_note"}

// IsFileKind is whether k is one of FileKinds.
func IsFileKind(k string) bool {
	for _, f := range FileKinds {
		if f == k {
			return true
		}
	}
	return false
}

// TabItem is one message of a tab's page: the message as its sender
// and kind need it, and for the Files tab its file as a row of the
// files registry's columns (Files).
type TabItem struct {
	Message
	File Row
}

// TabPage is a page of a tab, newest first, with the users and groups
// that sent its messages (a chat posting in its own name) and the
// message id the next page starts below, 0 when this is the last one.
type TabPage struct {
	Items      []TabItem
	NextBefore int64
	Users      map[int64]User
	Groups     map[int64]Group
}

// tabSelect is a tab's message columns, before the tab's own.
const tabSelect = "SELECT m.id AS row_id, m.message_id, m.date, m.sender_user_id, " +
	"m.sender_chat_id, m.author_signature, m.content_type, m.content_extra"

// tabFileColumns are the files registry's columns (Files.sel) of the
// message's file, joined as f.
const tabFileColumns = ", f.id, f.file_type, f.file_ext, f.orig_file_name, f.file_size, " +
	"f.hit_count, f.on_disk, f.created_at, f.accessed_at, f.tg_file_id, " +
	"HEX(f.sha256) AS sha256_hex, f.generated_from_file_id IS NOT NULL AS is_generated"

// GroupFiles is a page of the group's messages that carry a file of
// one of FileKinds, or of kind alone ("" for every kind), below the
// message id before (0: from the newest), at most limit of them.
func GroupFiles(ctx context.Context, db *sql.DB, sc scope.Scope, chat int64, kind string,
	before int64, limit int) (TabPage, error) {
	kinds := FileKinds
	if kind != "" {
		kinds = []string{kind}
	}
	args := []any{chat}
	for _, k := range kinds {
		args = append(args, k)
	}
	q := tabSelect + tabFileColumns + " FROM telegram_group_messages m " +
		"JOIN telegram_files f ON f.id = m.file_id " +
		"WHERE m.chat_id = ? AND m.content_type IN (" + placeholders(len(kinds)) + ")"
	return tabPage(ctx, db, sc, chat, q, args, before, limit, true)
}

// GroupPolls is a page of the group's poll messages, as GroupFiles.
// The polls themselves are the caller's to load (PollsByID), from the
// messages' content_extra.
func GroupPolls(ctx context.Context, db *sql.DB, sc scope.Scope, chat int64,
	before int64, limit int) (TabPage, error) {
	q := tabSelect + " FROM telegram_group_messages m " +
		"WHERE m.chat_id = ? AND m.content_type = 'poll'"
	return tabPage(ctx, db, sc, chat, q, []any{chat}, before, limit, false)
}

// tabPage runs a tab's query q (its WHERE begun, with args) for the
// page below before, newest first, reading one row more than limit to
// learn whether another page follows, and names the senders.
func tabPage(ctx context.Context, db *sql.DB, sc scope.Scope, chat int64, q string, args []any,
	before int64, limit int, withFile bool) (TabPage, error) {
	if before > 0 {
		q += " AND m.message_id < ?"
		args = append(args, before)
	}
	q += " ORDER BY m.message_id DESC LIMIT ?"
	args = append(args, limit+1)
	rs, err := db.QueryContext(ctx, q, args...)
	if err != nil {
		return TabPage{}, err
	}
	rows, err := scanRows(rs)
	if err != nil {
		return TabPage{}, err
	}
	var p TabPage
	if len(rows) > limit {
		rows = rows[:limit]
		p.NextBefore = rows[limit-1].Int("message_id")
	}
	var users, groups []int64
	for _, r := range rows {
		it := TabItem{Message: Message{RowID: r.Uint("row_id"), ChatID: chat,
			MessageID: r.Int("message_id"), Date: r.Int("date"),
			SenderUser: r.Int("sender_user_id"), SenderChat: r.Int("sender_chat_id"),
			AuthorSignature: r.Str("author_signature"), ContentType: r.Str("content_type")}}
		if !r.Null("content_extra") {
			it.Extra = []byte(r.Str("content_extra"))
		}
		if withFile {
			it.File = r
		}
		users = append(users, it.SenderUser)
		groups = append(groups, it.SenderChat)
		p.Items = append(p.Items, it)
	}
	if p.Users, err = usersByID(ctx, db, sc, users); err != nil {
		return TabPage{}, err
	}
	// Only the chats the scope sees, and the group itself, are named
	// (as Refs does); the others are not even read.
	var want []int64
	for _, g := range uniq(groups) {
		if g == chat || sc.SeesChat(g) {
			want = append(want, g)
		}
	}
	if p.Groups, err = groupsByID(ctx, db, want); err != nil {
		return TabPage{}, err
	}
	return p, nil
}

// KindCount is a kind of the Files tab and how many messages of the
// group carry one; Count is nil when counting took too long.
type KindCount struct {
	Kind  string
	Count *int64
}

// kindsTimeLimit bounds the count of a group's files by kind: it reads
// every message of the group, which for a large one is not cheap. Past
// it the kinds come without counts.
const kindsTimeLimit = 1500 * time.Millisecond

// kindsTTL is how long a group's counts are kept (kindsCache).
const kindsTTL = 2 * time.Minute

// kindsCache keeps each group's counts by kind for kindsTTL, so paging
// through a group's files, or switching its sub tabs, counts them once.
// Kept by database handle and group, as CountCache is.
var kindsCache = struct {
	sync.Mutex
	m map[kindsKey]kindsEntry
}{m: map[kindsKey]kindsEntry{}}

type kindsKey struct {
	db   *sql.DB
	chat int64
}

type kindsEntry struct {
	at    time.Time
	kinds []KindCount
}

// kindsCacheMax bounds the groups kept; the expired ones go when it is
// reached, every one when none has expired.
const kindsCacheMax = 1024

// GroupFileKinds are the kinds among FileKinds that the group's
// messages carry, in that order, with their counts: one query over the
// group's messages, bounded by kindsTimeLimit. When it runs out of time
// every kind is listed, without a count, as which are present is not
// known then.
func GroupFileKinds(ctx context.Context, db *sql.DB, chat int64) ([]KindCount, error) {
	key := kindsKey{db, chat}
	kindsCache.Lock()
	if e, ok := kindsCache.m[key]; ok && time.Since(e.at) < kindsTTL {
		kindsCache.Unlock()
		return e.kinds, nil
	}
	kindsCache.Unlock()

	args := []any{chat}
	for _, k := range FileKinds {
		args = append(args, k)
	}
	rows, err := db.QueryContext(ctx, "SELECT /*+ MAX_EXECUTION_TIME("+
		strconv.FormatInt(kindsTimeLimit.Milliseconds(), 10)+") */ m.content_type, COUNT(*) "+
		"FROM telegram_group_messages m WHERE m.chat_id = ? AND m.file_id IS NOT NULL "+
		"AND m.content_type IN ("+placeholders(len(FileKinds))+") GROUP BY m.content_type", args...)
	counts := map[string]int64{}
	if err == nil {
		for rows.Next() {
			var (
				k string
				n int64
			)
			if err = rows.Scan(&k, &n); err != nil {
				break
			}
			counts[k] = n
		}
		if err == nil {
			err = rows.Err()
		}
		rows.Close()
	}
	out := []KindCount{}
	switch {
	case isTimeout(err):
		for _, k := range FileKinds {
			out = append(out, KindCount{Kind: k})
		}
		// Not kept: the next request tries again.
		return out, nil
	case err != nil:
		return nil, err
	}
	for _, k := range FileKinds {
		if n, ok := counts[k]; ok {
			out = append(out, KindCount{Kind: k, Count: &n})
		}
	}
	kindsCache.Lock()
	if len(kindsCache.m) >= kindsCacheMax {
		pruned := false
		for k, e := range kindsCache.m {
			if time.Since(e.at) >= kindsTTL {
				delete(kindsCache.m, k)
				pruned = true
			}
		}
		if !pruned {
			clear(kindsCache.m)
		}
	}
	kindsCache.m[key] = kindsEntry{at: time.Now(), kinds: out}
	kindsCache.Unlock()
	return out, nil
}
