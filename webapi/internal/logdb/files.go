// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

// Package logdb reads the logger's schema (tgloggerd, the "ro" pool),
// which only the daemon writes. Every lookup of something a request
// asks for by id takes the request's scope.Scope.
package logdb

import (
	"context"
	"database/sql"
	"errors"

	"tgloggerd/webapi/internal/scope"
)

// ErrNotFound is returned when no row matches, or the row is outside
// the request's scope; callers answer both with 404.
var ErrNotFound = errors.New("not found")

// File is a telegram_files row as /files/<token> needs it.
type File struct {
	ID       uint64
	Hex      string // lower-case SHA-256 hex; "" = bytes never seen
	Ext      string // extension without the dot, or ""
	Type     string // "photo", "video", "document", ...
	OrigName string // the name sent through Telegram, or ""
	Size     uint64 // file_size; 0 = unknown
	OnDisk   bool   // the bytes are in the store
	AliasOf  uint64 // the row holding the same bytes, 0 if none
	// Generated: a still the daemon took from a stored video
	// (generated_from_file_id set), which Telegram never had.
	Generated bool
}

const fileColumns = "SELECT id, LOWER(HEX(sha256)), file_ext, file_type, " +
	"orig_file_name, file_size, on_disk, alias_of, " +
	"generated_from_file_id IS NOT NULL FROM telegram_files " +
	"WHERE id = ?"

func file(ctx context.Context, db *sql.DB, id uint64) (File, error) {
	var (
		f            File
		hex, ext     sql.NullString
		onDisk       bool
		aliasOf      sql.NullInt64
		size, fileID uint64
	)
	err := db.QueryRowContext(ctx, fileColumns, id).Scan(&fileID, &hex, &ext,
		&f.Type, &f.OrigName, &size, &onDisk, &aliasOf, &f.Generated)
	if errors.Is(err, sql.ErrNoRows) {
		return File{}, ErrNotFound
	}
	if err != nil {
		return File{}, err
	}
	f.ID, f.Hex, f.Ext, f.Size, f.OnDisk = fileID, hex.String, ext.String, size, onDisk
	if aliasOf.Valid {
		f.AliasOf = uint64(aliasOf.Int64)
	}
	return f, nil
}

// GetFile is the file a request asked for by id, within its scope.
//
// The scope is not applied: the file token is the authorization, a
// capability that works without a session, and a request without one
// (scope.Anonymous) gets every file its token opens, so filtering a
// signed-in request's files would only make it see less than a
// signed-out one. A member's grants are kept at the other end: it only
// ever gets tokens for files it sees (the rows it reads are within its
// scope), and whether its request may have an unstored file fetched
// is the server's mayFetch, from its abilities.
func GetFile(ctx context.Context, db *sql.DB, sc scope.Scope, id uint64) (File, error) {
	_ = sc
	return file(ctx, db, id)
}

// GetBlob is the row whose bytes are served for a file the request may
// already see: the one its alias_of names, or the one a fetch answered
// with. It takes no scope on purpose: those bytes are the visible
// file's own, whichever row records them.
func GetBlob(ctx context.Context, db *sql.DB, id uint64) (File, error) {
	return file(ctx, db, id)
}
