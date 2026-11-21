// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package mcp

import (
	"context"
	"encoding/base64"
	"sort"
	"strconv"
	"strings"
)

// The message rows of the message tools and the two-phase query behind
// them, as in the Drogon web (Tools.cpp: messageRow, runMessageQuery,
// listRecent, decorateIds).

const (
	defaultLimit = 50
	maxLimit     = 200
	// maxBatchIDs is the ids per batch call, the filter's maxInItems.
	maxBatchIDs = 100
)

// int32Of converts as C++ get<int>() does: the low 32 bits.
func int32Of(n int64) int { return int(int32(n)) }

// clampIntArg is an optional integer argument clamped to [lo, hi].
func clampIntArg(args map[string]any, name string, def, lo, hi int) int {
	n := def
	if v, ok := intValue(args[name]); ok {
		n = int32Of(v)
	}
	return min(max(n, lo), hi)
}

func clampLimit(args map[string]any) int {
	return clampIntArg(args, "limit", defaultLimit, 1, maxLimit)
}

// clampOffset refuses deep offsets, which are O(offset) in MySQL.
func clampOffset(args map[string]any) (int, error) {
	n := 0
	if v, ok := intValue(args["offset"]); ok {
		n = int32Of(v)
	}
	if n < 0 {
		n = 0
	}
	if n > 100000 {
		return 0, toolErr("offset is too large (max 100000); narrow the " +
			"filter instead of paging that deep")
	}
	return n, nil
}

// requireInt is args[name] as an integer, or the uniform error.
func requireInt(args map[string]any, name string) (int64, error) {
	v, ok := intValue(args[name])
	if !ok {
		return 0, toolErr(name + " is required and must be an integer")
	}
	return v, nil
}

// usernameSubq is a user's current public username, by the column that
// names the user.
func usernameSubq(userCol string) string {
	return "(SELECT x.username FROM telegram_user_usernames x " +
		"  WHERE x.user_id = " + userCol +
		" AND x.kind = 'active' " +
		"  ORDER BY x.position LIMIT 1)"
}

func propGroupID() map[string]any {
	return map[string]any{"type": "integer", "description": "The group's id (negative)."}
}

func propLimit(desc string) map[string]any {
	if desc == "" {
		desc = "1-200, default 50."
	}
	return map[string]any{"type": "integer", "description": desc}
}

// dateRange is an optional inclusive start/end on m.date.
type dateRange struct {
	sql              string
	binds            []any
	hasStart, hasEnd bool
	start, end       int64
}

// stamp echoes the range into a result, "all time" stated rather than
// left to two absent fields.
func (d *dateRange) stamp(out map[string]any) {
	if d.hasStart {
		out["start_date"] = d.start
	}
	if d.hasEnd {
		out["end_date"] = d.end
	}
	if !d.hasStart && !d.hasEnd {
		out["range"] = "all time"
	}
}

func parseDateRange(args map[string]any, defaultStart int64) (dateRange, error) {
	var d dateRange
	if v, ok := args["start_date"]; ok && v != nil {
		s, err := parseDate(v)
		if err != nil {
			return d, err
		}
		d.start, d.hasStart = s, true
	} else if defaultStart != 0 {
		d.start, d.hasStart = defaultStart, true
	}
	if v, ok := args["end_date"]; ok && v != nil {
		e, err := parseDate(v)
		if err != nil {
			return d, err
		}
		d.end, d.hasEnd = e, true
	}
	if d.hasStart && d.hasEnd && d.start > d.end {
		return d, toolErr("start_date is after end_date")
	}
	if d.hasStart {
		d.sql += " AND m.date >= ?"
		d.binds = append(d.binds, strconv.FormatInt(d.start, 10))
	}
	if d.hasEnd {
		d.sql += " AND m.date <= ?"
		d.binds = append(d.binds, strconv.FormatInt(d.end, 10))
	}
	return d, nil
}

// The message fields a filter may name. Three timestamps: date is when
// a message was SENT, edit_date when it was last edited, deleted_at
// when the deletion was observed.
var msgSchema = schema{
	{"text", "m.text", fFullText, "",
		"message body; word-based, so words shorter than 3 characters are " +
			"ignored by the index", ""},
	{"group_id", "m.chat_id", fInt, "", "the group's id (negative)", ""},
	{"sender_user_id", "m.sender_user_id", fInt, "", "author's Telegram user id", ""},
	{"message_id", "m.message_id", fInt, "", "per-group message id", ""},
	{"date", "m.date", fDateTs, "",
		"when it was SENT; accepts YYYY-MM-DD or a unix timestamp", ""},
	{"content_type", "m.content_type", fEnum,
		"text,photo,video,document,audio,voice,sticker,animation,service,unknown",
		"kind of message", ""},
	{"is_forwarded", "m.is_forwarded", fBool, "", "", ""},
	{"is_channel_post", "m.is_channel_post", fBool, "", "", ""},
	{"deleted", "m.deleted_at", fPresenceNull, "",
		"true for deleted messages, false for live ones " +
			"(is_null / is_not_null work too)", ""},
	{"edited", "m.edit_date", fPresence, "",
		"use op is_not_null for messages that were edited, is_null for " +
			"untouched ones -- the mirror of `deleted`", ""},
	{"edit_date", "m.edit_date", fDateTs, "",
		"when it was LAST EDITED. Use this, not `date`, to find edits in a " +
			"window; ordering by it is index-backed", ""},
	{"deleted_at", "m.deleted_at", fDateTs, "",
		"when the deletion was OBSERVED. Use this, not `date`, to find " +
			"deletions in a window", "FROM_UNIXTIME(?)"},
	{"sender_is_bot", "EXISTS(SELECT 1 FROM telegram_users bu " +
		"WHERE bu.id = m.sender_user_id AND bu.type = 'bot')",
		fBool, "",
		"true for messages sent by a bot -- excludes bot noise without " +
			"having to know any bot's id", ""},
	{"text_length", "CHAR_LENGTH(m.text)", fInt, "",
		"characters of text; use < to skip megaposts while scanning", ""},
}

// The user fields a filter may name.
var userSchema = schema{
	{"user_id", "u.id", fInt, "",
		"Telegram user id; with op \"in\" this is the batch path -- it is " +
			"the primary key, so a list of up to 100 ids is a single indexed " +
			"lookup", ""},
	{"username", "un.username", fText, "", "current public username, without the @", ""},
	{"first_name", "u.first_name", fText, "", "", ""},
	{"last_name", "u.last_name", fText, "", "", ""},
	{"phone_number", "COALESCE(e.phone_number,'')", fText, "", "only set for contacts", ""},
	{"bio", "COALESCE(e.bio,'')", fText, "", "", ""},
	{"type", "u.type", fEnum, "regular,deleted,bot,unknown", "", ""},
	{"is_bot", "(u.type = 'bot')", fBool, "", "", ""},
	{"msg_count", "u.msg_count", fInt, "", "messages seen from this user across the archive", ""},
}

// mediaJSON is the media object of a row with a file: metadata always,
// a URL only when the bytes are stored, since /files/<token> has to
// fetch anything else from Telegram, which may not deliver it.
func (t *telegram) mediaJSON(r row, withNames bool) map[string]any {
	fid := r.i64("file_id")
	md := map[string]any{
		"file_id": fid,
		"type":    r.str("file_type"),
		"size":    r.i64("file_size"),
	}
	if withNames {
		if n := r.str("orig_file_name"); n != "" {
			md["filename"] = n
		}
		if e := r.str("file_ext"); e != "" {
			md["extension"] = e
		}
	}
	stored := r.i64("on_disk") != 0
	md["stored"] = stored
	if stored {
		if u := t.fileURL(fid); u != "" {
			md["url"] = u
		}
	}
	return md
}

// rowFields is every key messageRow emits: what "fields" may name.
var rowFields = []string{
	"message_id", "group_id", "group_title", "sender_user_id",
	"sender_name", "sender_username", "date", "sent_at", "content_type",
	"text", "is_forwarded", "is_channel_post", "reply_to_message_id",
	"is_edited", "is_deleted", "edit_date", "deleted_at",
	"previous_version_count", "history_available_via", "media",
	"link_preview",
}

// outgoingField is a private message's "sent by the logging account"
// (private.go). "fields" may name it, but it is left out of rowFields
// and of what the tools describe, so every answer of a token without
// private chats, an error naming the fields included, stays as it was.
const outgoingField = "is_outgoing"

// rowOpts is how much of a row to emit; each control defaults to the
// full row.
type rowOpts struct {
	fields       []string // empty: every key
	truncateText int      // 0: never
	includeMedia bool
	compact      bool // drop false and empty fields, hoist the group
	hoistGroup   bool // the caller named one group
}

func (o *rowOpts) wants(k string) bool {
	if len(o.fields) == 0 {
		return true
	}
	for _, f := range o.fields {
		if f == k {
			return true
		}
	}
	return false
}

func parseRowOpts(args map[string]any) (rowOpts, error) {
	o := rowOpts{includeMedia: true}
	if v, ok := args["fields"]; ok {
		a, ok := v.([]any)
		if !ok {
			return o, toolErr(`"fields" must be an array of strings`)
		}
		for _, e := range a {
			k, ok := e.(string)
			if !ok {
				return o, toolErr(`"fields" must contain strings`)
			}
			known := k == outgoingField
			for _, f := range rowFields {
				if f == k {
					known = true
					break
				}
			}
			if !known {
				return o, toolErr(`unknown field "` + k + `" in "fields"; expected any of: ` +
					strings.Join(rowFields, ", "))
			}
			o.fields = append(o.fields, k)
		}
	}
	if n, ok := intValue(args["truncate_text"]); ok && n > 0 {
		o.truncateText = int(n)
	}
	if b, ok := boolArg(args, "include_media"); ok {
		o.includeMedia = b
	}
	if b, ok := boolArg(args, "compact"); ok {
		o.compact = b
	}
	o.hoistGroup = o.compact && isInt(args, "group_id")
	return o, nil
}

// truncateUTF8 cuts t to at most max bytes without splitting a UTF-8
// sequence; cut reports whether anything was removed.
func truncateUTF8(t string, max int) (string, bool) {
	if max <= 0 || len(t) <= max {
		return t, false
	}
	cut := max
	for cut > 0 && t[cut]&0xC0 == 0x80 {
		cut--
	}
	return t[:cut], true
}

// The paging cursor: base64url (unpadded) of "key|value|id", naming the
// last row's sort value and row id, so a page resumes where the last
// stopped while the archive grows. The key is checked on the way back:
// a cursor is only valid for the ordering that minted it.
type cursor struct {
	key, value string
	id         int64
	ok         bool
}

func cursorEncode(key, value string, id int64) string {
	return base64.RawURLEncoding.EncodeToString([]byte(key + "|" + value + "|" + strconv.FormatInt(id, 10)))
}

// b64uDecode decodes as the Drogon code does: the URL alphabet, no
// padding, trailing bits ignored, at most 512 characters.
func b64uDecode(in string) ([]byte, bool) {
	if in == "" || len(in)%4 == 1 || len(in) > 512 {
		return nil, false
	}
	var acc uint32
	bits := 0
	out := make([]byte, 0, len(in)*3/4)
	for i := 0; i < len(in); i++ {
		c := in[i]
		var d uint32
		switch {
		case c >= 'A' && c <= 'Z':
			d = uint32(c - 'A')
		case c >= 'a' && c <= 'z':
			d = uint32(c-'a') + 26
		case c >= '0' && c <= '9':
			d = uint32(c-'0') + 52
		case c == '-':
			d = 62
		case c == '_':
			d = 63
		default:
			return nil, false
		}
		acc = acc<<6 | d
		bits += 6
		if bits >= 8 {
			bits -= 8
			out = append(out, byte(acc>>bits))
		}
	}
	return out, true
}

func cursorDecode(s string) cursor {
	var c cursor
	raw, ok := b64uDecode(s)
	if !ok {
		return c
	}
	k, rest, ok := strings.Cut(string(raw), "|")
	if !ok {
		return c
	}
	v, idStr, ok := strings.Cut(rest, "|")
	if !ok {
		return c
	}
	c.key, c.value = k, v
	c.id = strtoll(idStr)
	// Every ordering mints numbers; anything else never reaches SQL.
	if c.value == "" || strings.Trim(c.value, "0123456789-") != "" {
		return c
	}
	c.ok = true
	return c
}

// strtoll reads a leading integer as strtoll does: blanks skipped, an
// optional sign, digits up to the first other byte, clamped on
// overflow.
func strtoll(s string) int64 {
	i := 0
	for i < len(s) && isSpace(s[i]) {
		i++
	}
	neg := false
	if i < len(s) && (s[i] == '+' || s[i] == '-') {
		neg = s[i] == '-'
		i++
	}
	var n int64
	for ; i < len(s) && s[i] >= '0' && s[i] <= '9'; i++ {
		d := int64(s[i] - '0')
		if !neg && n > (1<<63-1-d)/10 {
			return 1<<63 - 1
		}
		if neg && -n < (-1<<63+d)/10 {
			return -1 << 63
		}
		if neg {
			n = n*10 - d
		} else {
			n = n*10 + d
		}
	}
	return n
}

func (t *telegram) messageRow(r row, o *rowOpts) map[string]any {
	// message_id whatever the projection: the handle of every
	// follow-up call.
	j := map[string]any{"message_id": r.i64("message_id")}
	if !o.hoistGroup {
		if o.wants("group_id") {
			j["group_id"] = r.i64("chat_id")
		}
		if o.wants("group_title") {
			j["group_title"] = r.str("group_title")
		}
	}
	if o.wants("sender_user_id") {
		j["sender_user_id"] = r.i64("sender_user_id")
	}
	if o.wants("sender_name") {
		if v := r.str("sender_name"); !o.compact || v != "" {
			j["sender_name"] = v
		}
	}
	if o.wants("sender_username") {
		if v := r.str("sender_username"); !o.compact || v != "" {
			j["sender_username"] = v
		}
	}
	if o.wants("date") {
		j["date"] = r.i64("date")
	}
	if o.wants("sent_at") {
		j["sent_at"] = r.str("sent_at")
	}
	if o.wants("content_type") {
		j["content_type"] = r.str("content_type")
	}
	if o.wants("text") {
		// Raw text: it goes to a model, not a browser.
		text, cut := truncateUTF8(r.str("text"), o.truncateText)
		if cut {
			j["text_truncated"] = true
		}
		if !o.compact || text != "" {
			j["text"] = text
		}
	}
	fwd := r.i64("is_forwarded") != 0
	ch := r.i64("is_channel_post") != 0
	if o.wants("is_forwarded") && (!o.compact || fwd) {
		j["is_forwarded"] = fwd
	}
	if o.wants("is_channel_post") && (!o.compact || ch) {
		j["is_channel_post"] = ch
	}
	if _, private := r["is_outgoing"]; private && o.wants(outgoingField) {
		if out := r.i64("is_outgoing") != 0; !o.compact || out {
			j[outgoingField] = out
		}
	}
	if o.wants("reply_to_message_id") && !r.null("reply_to_msg_id") {
		j["reply_to_message_id"] = r.i64("reply_to_msg_id")
	}
	edited := r.i64("edit_date") != 0
	deleted := !r.null("deleted_at")
	if o.wants("is_edited") && (!o.compact || edited) {
		j["is_edited"] = edited
	}
	if o.wants("is_deleted") && (!o.compact || deleted) {
		j["is_deleted"] = deleted
	}
	if edited && o.wants("edit_date") {
		j["edit_date"] = r.i64("edit_date")
	}
	if deleted && o.wants("deleted_at") {
		j["deleted_at"] = r.str("deleted_at")
	}
	if edited && o.wants("previous_version_count") && !r.null("prev_versions") {
		j["previous_version_count"] = r.i64("prev_versions")
	}
	if (edited || deleted) && o.wants("history_available_via") && !o.compact {
		j["history_available_via"] = "telegram_get_messages (include_history) or " +
			"telegram_get_message_history"
	}
	if o.includeMedia && o.wants("media") && !r.null("file_id") {
		j["media"] = t.mediaJSON(r, true)
	}
	if o.wants("link_preview") && !r.null("lp_url") {
		lp := map[string]any{"url": r.str("lp_url")}
		for _, k := range [][2]string{
			{"display_url", "lp_display_url"}, {"site_name", "lp_site_name"},
			{"title", "lp_title"}, {"description", "lp_description"},
			{"author", "lp_author"},
		} {
			if v := r.str(k[1]); v != "" {
				lp[k[0]] = v
			}
		}
		lp["type"] = r.str("lp_type")
		if iv := r.i64("lp_iv"); iv != 0 {
			lp["instant_view_version"] = iv
		}
		if o.includeMedia && !r.null("lp_photo_id") {
			fid := r.i64("lp_photo_id")
			md := map[string]any{
				"file_id": fid,
				"type":    r.str("lp_photo_type"),
				"size":    r.i64("lp_photo_size"),
			}
			stored := r.i64("lp_photo_on_disk") != 0
			md["stored"] = stored
			if stored {
				if u := t.fileURL(fid); u != "" {
					md["url"] = u
				}
			}
			lp["image"] = md
		}
		j["link_preview"] = lp
	}
	return j
}

// orderSpec is how a scan is ordered. Only date, edit_date and
// deleted_at have a (chat_id, <column>) index; relevance, the default
// with a text match, has no cursor, a score not being a position.
type orderSpec struct {
	key, col, keyExpr, valExpr, sql, extraWhere string
	asc, cursorable                             bool
}

func parseOrder(args map[string]any, ftExpr string) (orderSpec, error) {
	o := orderSpec{cursorable: true, key: "date"}
	if v, ok := stringArg(args, "order"); ok {
		if v != "asc" && v != "desc" {
			return o, toolErr(`"order" must be "asc" or "desc"`)
		}
		o.asc = v == "asc"
	}
	explicit := false
	if v, ok := args["order_by"]; ok && v != nil {
		s, ok := v.(string)
		if !ok {
			return o, toolErr(`"order_by" must be a string: date, edit_date or deleted_at`)
		}
		o.key, explicit = s, true
	}
	switch o.key {
	case "date":
		o.col, o.keyExpr, o.valExpr = "m.date", "m.date", "?"
	case "edit_date":
		o.col, o.keyExpr, o.valExpr = "m.edit_date", "m.edit_date", "?"
	case "deleted_at":
		// A DATETIME, while a cursor carries unix seconds: out through
		// UNIX_TIMESTAMP, back in through FROM_UNIXTIME, the column
		// bare. Rows never deleted have no position to resume from.
		o.col, o.keyExpr, o.valExpr = "m.deleted_at", "UNIX_TIMESTAMP(m.deleted_at)", "FROM_UNIXTIME(?)"
		o.extraWhere = " AND m.deleted_at IS NOT NULL"
	default:
		return o, toolErr(`"order_by" must be one of: date, edit_date, deleted_at`)
	}
	dir := "DESC"
	if o.asc {
		dir = "ASC"
	}
	// The row id breaks ties, so a cursor neither repeats nor skips.
	o.sql = o.col + " " + dir + ", m.id " + dir
	// A text match without an explicit order goes by relevance, which
	// the full-text index serves in order so LIMIT stops early.
	if ftExpr != "" && !explicit {
		o.key = "relevance"
		o.sql = "MATCH(" + ftExpr + ") AGAINST(? IN BOOLEAN MODE) DESC"
		o.cursorable = false
	}
	return o, nil
}

type queryReq struct {
	gate         gate
	whereExtra   string
	binds        []any // whereExtra's
	order        orderSpec
	orderBind    string // the relevance term, bound twice
	cursor       cursor
	limit        int
	offset       int
	includeTotal bool
	row          rowOpts
	hoistedGroup int64
}

// runMessageQuery runs every message scan, in TWO PHASES: the ids are
// chosen from the message table alone, so a full-text index can supply
// them in relevance order and LIMIT stops early, and only then are the
// display joins run over at most limit rows. With the joins in the
// same statement MySQL materialises every hit and sorts it.
func (t *telegram) runMessageQuery(ctx context.Context, q *queryReq) (map[string]any, error) {
	messages := []any{}
	out := map[string]any{}

	where := q.gate.sql + q.order.extraWhere
	binds := append([]any{}, q.gate.binds...)
	if q.whereExtra != "" {
		where += " AND " + q.whereExtra
		binds = append(binds, q.binds...)
	}
	pageWhere := where
	pageBinds := append([]any{}, binds...)
	if q.cursor.ok {
		cmp := "<"
		if q.order.asc {
			cmp = ">"
		}
		pageWhere += " AND (" + q.order.col + " " + cmp + " " + q.order.valExpr +
			" OR (" + q.order.col + " = " + q.order.valExpr + " AND m.id " + cmp + " ?))"
		pageBinds = append(pageBinds, q.cursor.value, q.cursor.value, strconv.FormatInt(q.cursor.id, 10))
	}

	src := q.gate.src
	if q.limit > 0 && !q.gate.nothing {
		inner := "SELECT m.id, " + q.order.keyExpr + " AS ckey " +
			"FROM " + src.table + " m WHERE 1=1" + pageWhere +
			" ORDER BY " + q.order.sql +
			" LIMIT " + strconv.Itoa(q.limit) +
			" OFFSET " + strconv.Itoa(q.offset)
		stmt := src.selectCols + ", sel.ckey " +
			"FROM (" + inner + ") sel " +
			"JOIN " + src.table + " m ON m.id = sel.id " +
			src.joins + "ORDER BY " + q.order.sql
		all := append([]any{}, pageBinds...)
		if q.orderBind != "" {
			all = append(all, q.orderBind, q.orderBind)
		}
		rows, err := execTool(ctx, t.db, stmt, all, queryFailed)
		if err != nil {
			return nil, err
		}
		var lastID int64
		var lastKey string
		for _, r := range rows {
			messages = append(messages, t.messageRow(r, &q.row))
			if q.row.hoistGroup && !has(out, "group_title") {
				out["group_title"] = r.str("group_title")
			}
			lastID = r.i64("id_internal")
			lastKey = r.str("ckey")
		}
		// A cursor only after a full page: a short one is the end.
		if q.order.cursorable && lastID != 0 && len(messages) == q.limit {
			if lastKey == "" {
				lastKey = "0"
			}
			out["next_cursor"] = cursorEncode(q.order.key, lastKey, lastID)
		}
	}
	out["messages"] = messages
	out["count"] = len(messages)
	out["limit"] = q.limit
	if !q.cursor.ok {
		out["offset"] = q.offset
	}
	out["order_by"] = q.order.key
	if q.order.asc {
		out["order"] = "asc"
	} else {
		out["order"] = "desc"
	}
	if q.row.hoistGroup {
		out["group_id"] = q.hoistedGroup
	}

	if q.includeTotal {
		if q.gate.nothing {
			out["total"] = 0
		} else {
			// No ORDER BY and no cursor: the total is of the whole
			// match. A count that times out keeps the page.
			csql := "SELECT /*+ MAX_EXECUTION_TIME(5000) */ " +
				"COUNT(1) AS n FROM " + src.table + " m " +
				"WHERE 1=1" + where
			cr, err := execTool(ctx, t.db, csql, binds, "count failed")
			switch {
			case err != nil:
				out["total_unavailable"] = "the total could not be counted within " +
					"the time limit -- this happens with a " +
					"broad text match, whose count scans " +
					"every match in the archive before the " +
					"group filter applies. Narrow the " +
					"match, or drop include_total; the " +
					"messages above are unaffected."
			case len(cr) == 0:
				out["total"] = 0
			default:
				out["total"] = cr[0].i64("n")
			}
		}
	}
	return out, nil
}

// decorateIds joins the display columns onto an explicit, already
// authorised set of row ids, newest first.
func (t *telegram) decorateIDs(ctx context.Context, src *msgSource, ids []int64, o *rowOpts,
	envelope map[string]any) ([]any, error) {
	arr := []any{}
	if len(ids) == 0 {
		return arr, nil
	}
	binds := make([]any, len(ids))
	for i, id := range ids {
		binds[i] = strconv.FormatInt(id, 10)
	}
	stmt := src.selectCols + "FROM " + src.table + " m " + src.joins +
		"WHERE m.id IN (" + placeholders(len(ids), ",") + ") ORDER BY m.date DESC"
	rows, err := execTool(ctx, t.db, stmt, binds, queryFailed)
	if err != nil {
		return nil, err
	}
	for _, r := range rows {
		arr = append(arr, t.messageRow(r, o))
		if envelope != nil && o.hoistGroup && !has(envelope, "group_title") {
			envelope["group_title"] = r.str("group_title")
		}
	}
	return arr, nil
}

// listRecent is "recent messages" fanned out per group and merged.
// Ordering by date across the gated set materialises every message of
// every exposed group; within one group message_id is monotonic and
// covered by uq_group_messages_chat_msg, so a reverse walk stops at
// the limit. No single statement does that across groups, so the
// fan-out is the plan, affordable because the list is curated.
func (t *telegram) listRecent(ctx context.Context, src *msgSource, groups []int64, limit, offset int,
	o *rowOpts) (map[string]any, error) {
	type hit struct{ id, date int64 }
	var hits []hit
	per := limit + offset
	for _, g := range groups {
		stmt := "SELECT /*+ MAX_EXECUTION_TIME(5000) */ m.id, m.date " +
			"FROM " + src.table + " m WHERE m.chat_id = ? " +
			"ORDER BY m.message_id DESC LIMIT " + strconv.Itoa(per)
		rows, err := execTool(ctx, t.db, stmt, []any{strconv.FormatInt(g, 10)}, queryFailed)
		if err != nil {
			return nil, err
		}
		for _, r := range rows {
			hits = append(hits, hit{r.i64("id"), r.i64("date")})
		}
	}
	sort.SliceStable(hits, func(i, j int) bool { return hits[i].date > hits[j].date })
	var ids []int64
	for i := offset; i < len(hits) && len(ids) < limit; i++ {
		ids = append(ids, hits[i].id)
	}
	out := map[string]any{}
	msgs, err := t.decorateIDs(ctx, src, ids, o, out)
	if err != nil {
		return nil, err
	}
	out["messages"] = msgs
	out["count"] = len(msgs)
	out["limit"] = limit
	out["offset"] = offset
	return out, nil
}
