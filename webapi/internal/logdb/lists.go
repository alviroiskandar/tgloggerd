// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package logdb

import (
	"context"
	"database/sql"
	"strconv"
	"strings"

	"tgloggerd/webapi/internal/scope"
)

// What the list pages read besides the search itself: the archive's
// totals, the message a file came in, the origin of forwarded
// messages, and the chat search of the pickers. Each takes the scope.

// ArchiveTotal is the number of rows of the entity, for a scope that
// sees all of them; ok is false for one that does not, which gets no
// archive total at all. It is exact when counting fits the statement
// time limit, the table's row estimate otherwise (always for entities
// counted by estimate).
func ArchiveTotal(ctx context.Context, db *sql.DB, sc scope.Scope, e *Entity) (int64, bool, error) {
	if !e.visible(sc) {
		return 0, false, nil
	}
	if cond, _ := e.filter(sc); cond != "" {
		return 0, false, nil
	}
	if !e.estimateBrowse {
		n, err := countInt(ctx, db, "SELECT /*+ MAX_EXECUTION_TIME("+
			strconv.Itoa(statementMS)+") */ COUNT(*) FROM "+e.countFromFor(""))
		if err == nil {
			return n, true, nil
		}
		if ctx.Err() != nil {
			return 0, false, ctx.Err()
		}
	}
	var est sql.NullInt64
	err := db.QueryRowContext(ctx, "SELECT table_rows FROM information_schema.tables "+
		"WHERE table_schema = DATABASE() AND table_name = ?", e.countTable()).Scan(&est)
	if err != nil {
		return 0, false, err
	}
	return est.Int64, true, nil
}

// StoredBytes is the size of the files whose bytes are in the store,
// for a scope that sees every file; ok is false otherwise, or when the
// sum does not fit the statement time limit.
func StoredBytes(ctx context.Context, db *sql.DB, sc scope.Scope) (int64, bool) {
	if cond, _ := Files.filter(sc); cond != "" || !Files.visible(sc) {
		return 0, false
	}
	n, err := countInt(ctx, db, "SELECT /*+ MAX_EXECUTION_TIME("+
		strconv.Itoa(statementMS)+") */ SUM(file_size) FROM telegram_files "+
		"WHERE on_disk = 1")
	if err != nil {
		return 0, false
	}
	return n, true
}

// FileOrigin is the first message a file was sent in, among those the
// scope sees.
type FileOrigin struct {
	Scope     string // "group" or "private"
	MessageID uint64 // the message's row id
	ChatID    int64
	ChatTitle string
	ChatKind  string // "group" or "user"
	SenderID  int64  // 0: the logging account, or a chat
	Sender    string // the sender's name, "" with SenderID 0
	// Extra is the message's content_extra (docs/design/message-types.md
	// section 4.2.1): a video's or audio file's length, among others.
	// Only for the message's own file: it describes that file.
	Extra []byte
	// Role is "" when the file is the message's own file, else what it
	// is to the message (FileRoles): a video's cover, a link preview's
	// picture. Kind is then the message's content_type.
	Role, Kind string
}

// FileOrigins are the first visible messages of the files ids, by file
// id: a group message when there is one, else a private message, that
// carries it as its own file; for a file that is none's, the first
// that carries it in a role (fileRoleOrigins). A file in no visible
// message has none.
func FileOrigins(ctx context.Context, db *sql.DB, sc scope.Scope, ids []uint64) (map[uint64]FileOrigin, error) {
	out := map[uint64]FileOrigin{}
	if len(ids) == 0 {
		return out, nil
	}
	args := make([]any, len(ids))
	for i, id := range ids {
		args[i] = id
	}
	gf, ga := sc.GroupFilter("fm.chat_id")
	q := "SELECT gm.file_id, gm.id, gm.chat_id, g.title, gm.sender_user_id, " +
		"su.first_name, su.last_name, gm.content_extra " +
		"FROM telegram_group_messages gm " +
		"JOIN (SELECT fm.file_id, MIN(fm.id) AS id FROM telegram_group_messages fm " +
		"WHERE fm.file_id IN (" + placeholders(len(ids)) + ") AND " + gf +
		" GROUP BY fm.file_id) x ON x.id = gm.id " +
		"LEFT JOIN `telegram_groups` g ON g.id = gm.chat_id " +
		"LEFT JOIN telegram_users su ON su.id = gm.sender_user_id"
	rs, err := db.QueryContext(ctx, q, both(args, ga)...)
	if err != nil {
		return nil, err
	}
	rows, err := scanRows(rs)
	if err != nil {
		return nil, err
	}
	for _, r := range rows {
		o := FileOrigin{Scope: "group", MessageID: r.Uint("id"), ChatID: r.Int("chat_id"),
			ChatTitle: GroupTitle(r.Str("title")), ChatKind: "group",
			Extra: []byte(r.Str("content_extra"))}
		if !r.Null("sender_user_id") {
			o.SenderID = r.Int("sender_user_id")
			o.Sender = DisplayName(r.Str("first_name"), r.Str("last_name"))
		}
		out[r.Uint("file_id")] = o
	}
	if !sc.SeesPrivate() || len(out) == len(ids) {
		return out, fileRoleOrigins(ctx, db, sc, ids, out)
	}
	q = "SELECT pm.file_id, pm.id, pm.chat_id, pu.first_name AS peer_first, " +
		"pu.last_name AS peer_last, pm.sender_id, pm.is_outgoing, pm.content_extra " +
		"FROM telegram_private_messages pm " +
		"JOIN (SELECT fp.file_id, MIN(fp.id) AS id FROM telegram_private_messages fp " +
		"WHERE fp.file_id IN (" + placeholders(len(ids)) + ") GROUP BY fp.file_id) x " +
		"ON x.id = pm.id LEFT JOIN telegram_users pu ON pu.id = pm.chat_id"
	if rs, err = db.QueryContext(ctx, q, args...); err != nil {
		return nil, err
	}
	if rows, err = scanRows(rs); err != nil {
		return nil, err
	}
	for _, r := range rows {
		id := r.Uint("file_id")
		if _, ok := out[id]; ok {
			continue
		}
		peer := DisplayName(r.Str("peer_first"), r.Str("peer_last"))
		o := FileOrigin{Scope: "private", MessageID: r.Uint("id"), ChatID: r.Int("chat_id"),
			ChatTitle: peer, ChatKind: "user", Extra: []byte(r.Str("content_extra"))}
		if !r.Bool("is_outgoing") && !r.Null("sender_id") {
			// Incoming private messages come from the peer.
			o.SenderID, o.Sender = r.Int("sender_id"), peer
		}
		out[id] = o
	}
	return out, fileRoleOrigins(ctx, db, sc, ids, out)
}

// fileRoleOrigins adds to out the origins of the ids out lacks from the
// messages that carry them in a role: the first visible group message
// with the file in telegram_message_files, then in a link preview,
// then the same in private messages. Each query reads its table by
// its file index for the few ids left, and none runs when no id is.
func fileRoleOrigins(ctx context.Context, db *sql.DB, sc scope.Scope, ids []uint64,
	out map[uint64]FileOrigin) error {
	type source struct {
		table, fileCol, role string
	}
	sources := []source{
		{"telegram_message_files", "file_id", "x.role"},
		{"telegram_link_previews", "photo_file_id", "'link_preview'"},
	}
	gf, ga := sc.GroupFilter("fm.chat_id")
	for _, private := range []bool{false, true} {
		if private && !sc.SeesPrivate() {
			break
		}
		for _, src := range sources {
			var left []any
			for _, id := range ids {
				if _, ok := out[id]; !ok {
					left = append(left, id)
				}
			}
			if len(left) == 0 {
				return nil
			}
			var q string
			var args []any
			if !private {
				q = "SELECT x." + src.fileCol + " AS file_id, " + src.role + " AS role, " +
					"gm.id, gm.chat_id, gm.content_type, g.title, gm.sender_user_id, " +
					"su.first_name, su.last_name " +
					"FROM (SELECT y." + src.fileCol + " AS fid, MIN(y.id) AS rid FROM " +
					src.table + " y JOIN telegram_group_messages fm ON fm.id = y.group_message_id " +
					"WHERE y." + src.fileCol + " IN (" + placeholders(len(left)) + ") AND " + gf +
					" GROUP BY y." + src.fileCol + ") z JOIN " + src.table + " x ON x.id = z.rid " +
					"JOIN telegram_group_messages gm ON gm.id = x.group_message_id " +
					"LEFT JOIN `telegram_groups` g ON g.id = gm.chat_id " +
					"LEFT JOIN telegram_users su ON su.id = gm.sender_user_id"
				args = both(left, ga)
			} else {
				q = "SELECT x." + src.fileCol + " AS file_id, " + src.role + " AS role, " +
					"pm.id, pm.chat_id, pm.content_type, pu.first_name AS peer_first, " +
					"pu.last_name AS peer_last, pm.sender_id, pm.is_outgoing " +
					"FROM (SELECT y." + src.fileCol + " AS fid, MIN(y.id) AS rid FROM " +
					src.table + " y WHERE y." + src.fileCol + " IN (" + placeholders(len(left)) +
					") AND y.private_message_id IS NOT NULL GROUP BY y." + src.fileCol + ") z " +
					"JOIN " + src.table + " x ON x.id = z.rid " +
					"JOIN telegram_private_messages pm ON pm.id = x.private_message_id " +
					"LEFT JOIN telegram_users pu ON pu.id = pm.chat_id"
				args = left
			}
			rs, err := db.QueryContext(ctx, q, args...)
			if err != nil {
				return err
			}
			rows, err := scanRows(rs)
			if err != nil {
				return err
			}
			for _, r := range rows {
				o := FileOrigin{MessageID: r.Uint("id"), ChatID: r.Int("chat_id"),
					Role: r.Str("role"), Kind: r.Str("content_type")}
				if !private {
					o.Scope, o.ChatTitle, o.ChatKind = "group", GroupTitle(r.Str("title")), "group"
					if !r.Null("sender_user_id") {
						o.SenderID = r.Int("sender_user_id")
						o.Sender = DisplayName(r.Str("first_name"), r.Str("last_name"))
					}
				} else {
					peer := DisplayName(r.Str("peer_first"), r.Str("peer_last"))
					o.Scope, o.ChatTitle, o.ChatKind = "private", peer, "user"
					if !r.Bool("is_outgoing") && !r.Null("sender_id") {
						o.SenderID, o.Sender = r.Int("sender_id"), peer
					}
				}
				out[r.Uint("file_id")] = o
			}
		}
	}
	return nil
}

// FileStill is what a video file (an animation, a video note, a video
// sticker) can show before it plays, from the messages that carried
// it: the stored cover and the stored Telegram thumbnail of one of
// them (telegram_message_files), and the base64 of one's minithumbnail
// (content_extra). An id is 0, and Minithumb "", when none has it.
type FileStill struct {
	Cover, Thumbnail uint64
	Minithumb        string
}

// FileStills are the stills of the video files ids, by file id, from
// the messages the scope sees, in one query per message table: the
// same video sent again carries the same pictures, and a message
// logged before the daemon stored thumbnails has none, so any visible
// message that has one will do. A video that has none from a message
// of its own gets the picture a message it plays a role in shows it
// with (roleStills). A file with none is left out.
func FileStills(ctx context.Context, db *sql.DB, sc scope.Scope, ids []uint64) (map[uint64]FileStill, error) {
	gf, ga := sc.GroupFilter("m.chat_id")
	return fileStills(ctx, db, ids, gf, ga, sc.SeesPrivate())
}

// ChatFileStills are FileStills from the messages of one group alone,
// for a viewer who sees that group only as it is exposed (its public
// chat and tabs), whose scope sees no message.
func ChatFileStills(ctx context.Context, db *sql.DB, chat int64, ids []uint64) (map[uint64]FileStill, error) {
	return fileStills(ctx, db, ids, "m.chat_id = ?", []any{chat}, false)
}

// fileStills runs FileStills' queries on the group messages filter
// allows (with fargs), and on the private messages too if private.
func fileStills(ctx context.Context, db *sql.DB, ids []uint64, gf string, ga []any,
	private bool) (map[uint64]FileStill, error) {
	out := map[uint64]FileStill{}
	if len(ids) == 0 {
		return out, nil
	}
	args := make([]any, len(ids))
	for i, id := range ids {
		args[i] = id
	}
	query := func(table, fk, filter string, fargs []any) error {
		rs, err := db.QueryContext(ctx, "SELECT m.file_id, "+
			"MIN(CASE WHEN mf.role = 'cover' THEN mf.file_id END) AS cover, "+
			"MIN(CASE WHEN mf.role = 'thumbnail' THEN mf.file_id END) AS thumb, "+
			"MAX(JSON_UNQUOTE(JSON_EXTRACT(m.content_extra, '$.minithumbnail.data'))) AS mini "+
			"FROM "+table+" m LEFT JOIN (telegram_message_files mf "+
			"JOIN telegram_files sf ON sf.id = mf.file_id AND sf.on_disk = 1) "+
			"ON mf."+fk+" = m.id AND mf.role IN ('cover', 'thumbnail') AND mf.position = 0 "+
			"WHERE m.file_id IN ("+placeholders(len(ids))+") AND "+filter+
			" GROUP BY m.file_id", both(args, fargs)...)
		if err != nil {
			return err
		}
		rows, err := scanRows(rs)
		if err != nil {
			return err
		}
		for _, r := range rows {
			id := r.Uint("file_id")
			st := out[id]
			if st.Cover == 0 {
				st.Cover = r.Uint("cover")
			}
			if st.Thumbnail == 0 {
				st.Thumbnail = r.Uint("thumb")
			}
			if st.Minithumb == "" {
				st.Minithumb = r.Str("mini")
			}
			if st != (FileStill{}) {
				out[id] = st
			}
		}
		return nil
	}
	if err := query("telegram_group_messages", "group_message_id", gf, ga); err != nil {
		return nil, err
	}
	if private {
		if err := query("telegram_private_messages", "private_message_id", "1", nil); err != nil {
			return nil, err
		}
	}
	if err := roleStills(ctx, db, args, gf, ga, private, out); err != nil {
		return nil, err
	}
	return out, nil
}

// roleStillCandidates are the stored pictures a video gets from a
// message it plays a role in (rather than being its own file), as the
// chat shows it there, with their rank (0 first): a rich block's still
// thumbnail at its position (rich.go), an animated chat photo's still
// photo (photoAnimation), a video link preview's cover, else the
// preview's photo (previewMedium). A paid item, a game's animation
// and a live photo's video have none. FK is the message table's key
// in telegram_message_files and telegram_link_previews; the ids are
// bound twice.
const roleStillCandidates = "SELECT v.file_id, v.FK AS msg, 0 AS prio, st.file_id AS still " +
	"FROM telegram_message_files v JOIN telegram_message_files st ON st.FK = v.FK AND (" +
	"(v.role = 'rich' AND st.role = 'thumbnail' AND st.position = v.position) OR " +
	"(v.role = 'chat_photo' AND v.position = 1 AND st.role = 'chat_photo' AND st.position = 0) OR " +
	"(v.role = 'preview_media' AND v.position = 0 AND st.role = 'preview_media' AND st.position = 1 " +
	"AND EXISTS (SELECT 1 FROM telegram_link_previews vp WHERE vp.FK = v.FK AND vp.type = 'video'))) " +
	"WHERE v.file_id IN (IDS) AND v.role IN ('rich', 'chat_photo', 'preview_media') " +
	"UNION ALL SELECT v.file_id, v.FK, 1, p.photo_file_id " +
	"FROM telegram_message_files v JOIN telegram_link_previews p ON p.FK = v.FK " +
	"WHERE v.file_id IN (IDS) AND v.role = 'preview_media' AND v.position = 0"

// roleStills fills in the Thumbnail of the videos of out that have no
// picture from a message of their own (fileStills), from the messages
// filter allows that they play a role in (roleStillCandidates), the
// private ones too if private: the best rank, then the smallest id.
// args are the video ids. It costs one query per message table, as
// many videos as there are.
func roleStills(ctx context.Context, db *sql.DB, args []any, gf string, ga []any, private bool,
	out map[uint64]FileStill) error {
	ph := placeholders(len(args))
	type pick struct{ prio, still uint64 }
	best := map[uint64]pick{}
	query := func(table, fk, filter string, fargs []any) error {
		c := strings.NewReplacer("FK", fk, "IDS", ph).Replace(roleStillCandidates)
		rs, err := db.QueryContext(ctx, "SELECT c.file_id, c.prio, MIN(c.still) AS still "+
			"FROM ("+c+") c JOIN telegram_files sf ON sf.id = c.still AND sf.on_disk = 1 "+
			"JOIN "+table+" m ON m.id = c.msg WHERE "+filter+
			" GROUP BY c.file_id, c.prio", both(both(args, args), fargs)...)
		if err != nil {
			return err
		}
		rows, err := scanRows(rs)
		if err != nil {
			return err
		}
		for _, r := range rows {
			id, p := r.Uint("file_id"), pick{r.Uint("prio"), r.Uint("still")}
			if b, ok := best[id]; !ok || p.prio < b.prio || p.prio == b.prio && p.still < b.still {
				best[id] = p
			}
		}
		return nil
	}
	if err := query("telegram_group_messages", "group_message_id", gf, ga); err != nil {
		return err
	}
	if private {
		if err := query("telegram_private_messages", "private_message_id", "1", nil); err != nil {
			return err
		}
	}
	for id, b := range best {
		if st := out[id]; st.Cover == 0 && st.Thumbnail == 0 {
			st.Thumbnail = b.still
			out[id] = st
		}
	}
	return nil
}

// ForwardOrigins names where forwarded messages came from, by message
// row id, for the messages of one scope ("group" or "private") the
// caller already read within the request scope. A chat the scope does
// not see is not named.
func ForwardOrigins(ctx context.Context, db *sql.DB, sc scope.Scope, msgScope string, ids []uint64) (map[uint64]string, error) {
	out := map[uint64]string{}
	if len(ids) == 0 {
		return out, nil
	}
	table, fk := "telegram_group_message_fwd_info", "group_message_id"
	if msgScope == "private" {
		table, fk = "telegram_private_message_fwd_info", "private_message_id"
	}
	args := make([]any, len(ids))
	for i, id := range ids {
		args[i] = id
	}
	rs, err := db.QueryContext(ctx, "SELECT fi."+fk+" AS mid, fi.origin_sender_name, "+
		"fi.origin_chat_id, ou.first_name, ou.last_name, oc.title "+
		"FROM "+table+" fi LEFT JOIN telegram_users ou ON ou.id = fi.origin_sender_user_id "+
		"LEFT JOIN `telegram_groups` oc ON oc.id = fi.origin_chat_id "+
		"WHERE fi."+fk+" IN ("+placeholders(len(ids))+")", args...)
	if err != nil {
		return nil, err
	}
	rows, err := scanRows(rs)
	if err != nil {
		return nil, err
	}
	for _, r := range rows {
		name := r.Str("origin_sender_name")
		switch {
		case name != "":
		case r.Str("first_name") != "" || r.Str("last_name") != "":
			name = DisplayName(r.Str("first_name"), r.Str("last_name"))
		case !r.Null("origin_chat_id") && r.Str("title") != "" &&
			sc.SeesChat(r.Int("origin_chat_id")):
			name = r.Str("title")
		default:
			name = "unknown"
		}
		out[r.Uint("mid")] = name
	}
	return out, nil
}
