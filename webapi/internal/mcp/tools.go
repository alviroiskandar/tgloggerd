// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package mcp

import (
	"context"
	"database/sql"
	"sort"
	"strconv"
	"strings"
	"time"
)

// The curated Telegram tools, the port of web/src/mcp/telegram/
// Tools.cpp: the same names, descriptions, input schemas, statements
// and answers.

type telegram struct {
	db      *sql.DB
	fileURL func(id int64) string
	now     func() time.Time
	// self is the logging account's Telegram user id, 0 while unknown
	// (private.go).
	self    func() int64
	private privateSources
}

// Instructions is what initialize tells the client about the server.
const Instructions = "Read-only access to a Telegram message archive. Only " +
	"groups an administrator has explicitly exposed are " +
	"readable; private groups and direct messages are not " +
	"available through any tool. Call telegram_list_groups " +
	"first to see what can be queried."

// NewTelegramServer is the MCP server of the Telegram archive, reading
// the logger's schema through db. fileURL makes the absolute
// /files/<token> link of a stored file, "" when no public base URL is
// configured (the link is then left out). self is the logging
// account's Telegram user id, 0 while unknown, given as the sender of
// the private messages it sent; nil knows none.
func NewTelegramServer(db *sql.DB, fileURL func(id uint64) string, self func() int64) *Server {
	t := &telegram{db: db, now: time.Now, self: self}
	t.fileURL = func(id int64) string {
		if id == 0 || fileURL == nil {
			return ""
		}
		return fileURL(uint64(id))
	}
	return NewServer(ServerInfo{
		Name:         "tgloggerd",
		Title:        "tgloggerd archive",
		Version:      "0.1.0",
		Instructions: Instructions,
	}, t.tools())
}

func (t *telegram) tools() []Tool {
	return []Tool{
		t.listGroupsTool(),
		t.getGroupTool(),
		t.getGroupHistoryTool(),
		t.listGroupAdminsTool(),
		t.listRecentMessagesTool(),
		t.listGroupSendersTool(),
		t.countUserMessagesTool(),
		t.popularWordsTool(),
		t.searchMessagesTool(),
		t.getMessagesTool(),
		t.getMessageHistoryTool(),
		t.getUserTool(),
		t.getUserHistoryTool(),
		t.getUsersTool(),
	}
}

func objectSchema(props map[string]any, required ...string) map[string]any {
	s := map[string]any{"type": "object", "properties": props}
	if len(required) > 0 {
		r := make([]any, len(required))
		for i, k := range required {
			r[i] = k
		}
		s["required"] = r
	}
	return s
}

func strList(v ...string) []any {
	out := make([]any, len(v))
	for i, s := range v {
		out[i] = s
	}
	return out
}

// addRowShapeProps adds the size controls of every tool returning
// message rows.
func addRowShapeProps(props map[string]any) {
	props["fields"] = map[string]any{
		"type":  "array",
		"items": map[string]any{"type": "string"},
		"description": "Return only these keys per message, e.g. " +
			"[\"message_id\",\"sender_username\",\"date\",\"edit_date\"]. " +
			"Valid: message_id, group_id, group_title, sender_user_id, " +
			"sender_name, sender_username, date, sent_at, content_type, " +
			"text, is_forwarded, is_channel_post, reply_to_message_id, " +
			"is_edited, is_deleted, edit_date, deleted_at, " +
			"previous_version_count, history_available_via, media, " +
			"link_preview. " +
			"message_id is always included -- it is the handle every " +
			"follow-up call needs. Cuts response size several-fold on a " +
			"wide scan.",
	}
	props["truncate_text"] = map[string]any{
		"type": "integer",
		"description": "Cut each message's text to this many bytes; rows that were " +
			"cut carry text_truncated: true. Retrieve the full text of " +
			"the ones that matter with telegram_get_messages.",
	}
	props["include_media"] = map[string]any{
		"type": "boolean",
		"description": "Default true. Set false to drop the media object (url, " +
			"size, filename) -- the largest per-row cost, and rarely " +
			"needed while scanning.",
	}
	props["compact"] = map[string]any{
		"type": "boolean",
		"description": "Default false. Omits false and empty fields instead of " +
			"serialising them, and when group_id was passed, hoists " +
			"group_id/group_title out of every row into the envelope.",
	}
}

func addPagingProps(props map[string]any) {
	props["order_by"] = map[string]any{
		"type": "string",
		"enum": strList("date", "edit_date", "deleted_at"),
		"description": "Which timestamp to sort on. Default date (when sent). Use " +
			"edit_date to list what changed, deleted_at to list what was " +
			"removed -- both are index-backed. With a text match and no " +
			"order_by, results come back by relevance instead, and you " +
			"should usually leave it that way: pairing a BROAD text " +
			"match with an explicit order_by makes the database sort " +
			"every hit and can exceed the time limit. deleted_at " +
			"ordering considers only messages that were actually " +
			"deleted.",
	}
	props["order"] = map[string]any{
		"type":        "string",
		"enum":        strList("desc", "asc"),
		"description": "Default desc (newest first).",
	}
	props["cursor"] = map[string]any{
		"type": "string",
		"description": "Pass back the next_cursor from the previous response to get " +
			"the next page. PREFER THIS OVER offset: the archive is live, " +
			"so offsets shift as new messages arrive and rows get " +
			"duplicated or skipped mid-scan. A cursor resumes exactly " +
			"where the last page stopped. It is opaque -- pass it " +
			"verbatim, do not build one.",
	}
}

func inputSchemaForMessages(withFilter bool) map[string]any {
	props := map[string]any{}
	if withFilter {
		props["filter"] = map[string]any{
			"type": "object",
			"description": "A filter tree. A node is either a condition " +
				"{\"field\",\"op\",\"value\"} or a group " +
				"{\"and\":[...]}, {\"or\":[...]}, {\"not\":{...}}. " +
				"Groups nest, so (A OR B) AND NOT C is expressible.",
		}
	}
	props["group_id"] = map[string]any{
		"type":        "integer",
		"description": "Restrict to one group (negative id).",
	}
	props["limit"] = propLimit("0-200, default 50. Use limit: 0 with include_total to get " +
		"just the count, so a job can be sized before paging it.")
	props["offset"] = map[string]any{
		"type":        "integer",
		"description": "Rows to skip; default 0. Prefer cursor for paging.",
	}
	props["include_total"] = map[string]any{
		"type": "boolean",
		"description": "Also return the total match count. Off by default because " +
			"counting a broad filter is expensive. With a broad text " +
			"match the count can exceed the time limit; when it does the " +
			"messages are still returned and total_unavailable explains " +
			"why, so the call is never wasted.",
	}
	if withFilter {
		addPagingProps(props)
	}
	addRowShapeProps(props)
	return map[string]any{"type": "object", "properties": props}
}

// parseKinds is which of names "kinds" asks for: all when absent. An
// unknown name is an error, as a silently ignored kind would read as an
// empty history.
func parseKinds(args map[string]any, names []string) ([]bool, error) {
	want := make([]bool, len(names))
	v, ok := args["kinds"]
	if !ok {
		for i := range want {
			want[i] = true
		}
		return want, nil
	}
	a, ok := v.([]any)
	if !ok {
		return nil, toolErr(`"kinds" must be an array of strings`)
	}
	for _, k := range a {
		s, ok := k.(string)
		if !ok {
			return nil, toolErr(`"kinds" must contain strings`)
		}
		found := false
		for i, n := range names {
			if s == n {
				want[i], found = true, true
			}
		}
		if !found {
			return nil, toolErr(`unknown kind "` + s + `"; expected any of: ` +
				strings.Join(names, ", "))
		}
	}
	return want, nil
}

// histList is one simple history section: each row's columns as
// strings, plus observed_at from created_at.
func (t *telegram) histList(ctx context.Context, out map[string]any, key, stmt, bind string, fields [][2]string) error {
	rows, err := execTool(ctx, t.db, stmt, []any{bind}, queryFailed)
	if err != nil {
		return err
	}
	list := []any{}
	for _, r := range rows {
		j := map[string]any{}
		for _, f := range fields {
			j[f[0]] = r.str(f[1])
		}
		j["observed_at"] = r.str("created_at")
		list = append(list, j)
	}
	out[key] = list
	return nil
}

// ---- telegram_list_groups ----

func (t *telegram) listGroups(ctx context.Context, args map[string]any) (any, error) {
	limit := clampLimit(args)
	rd, err := t.readable(ctx)
	if err != nil {
		return nil, err
	}
	cond, binds := rd.groupCond("g.id")
	stmt := "SELECT g.id, g.title, g.type, g.msg_count, " +
		"g.description, " +
		"gf.id AS photo_file_id, gf.on_disk AS photo_on_disk, " +
		"(SELECT gu.username FROM telegram_group_usernames gu " +
		"  WHERE gu.group_id = g.id AND gu.kind='active' " +
		"  ORDER BY gu.position LIMIT 1) AS username " +
		"FROM `telegram_groups` g " +
		"LEFT JOIN telegram_files gf " +
		"  ON gf.id = g.photo_file_id WHERE 1=1" +
		cond +
		" ORDER BY g.msg_count DESC LIMIT " + strconv.Itoa(limit)
	rows, err := execTool(ctx, t.db, stmt, binds, queryFailed)
	if err != nil {
		return nil, err
	}
	groups := []any{}
	for _, r := range rows {
		j := map[string]any{
			"group_id":      r.i64("id"),
			"title":         r.str("title"),
			"type":          r.str("type"),
			"username":      r.str("username"),
			"message_count": r.i64("msg_count"),
		}
		if d := r.str("description"); d != "" {
			j["description"] = d
		}
		if !r.null("photo_file_id") {
			pid := r.i64("photo_file_id")
			j["photo_file_id"] = pid
			if r.i64("photo_on_disk") != 0 {
				if u := t.fileURL(pid); u != "" {
					j["photo_url"] = u
				}
			}
		}
		groups = append(groups, j)
	}
	out := map[string]any{"groups": groups, "count": len(groups)}
	// Only a token whose scope holds private chats sees the key, so
	// every other answer stays as it was.
	if len(rd.private) > 0 {
		pcs, err := t.privateChats(ctx, rd.private)
		if err != nil {
			return nil, err
		}
		out["private_chats"] = pcs
		out["private_chats_note"] = "This token may also read these private " +
			"chats: pass a chat_id as group_id to the message tools " +
			"(search, recent, get messages, message history, popular " +
			"words, senders, count user messages). Searching or listing " +
			"recent messages without a group_id covers groups only."
	}
	return out, nil
}

func (t *telegram) listGroupsTool() Tool {
	return Tool{
		Name:  "telegram_list_groups",
		Title: "List available Telegram groups",
		Description: "List the Telegram groups and channels this server may " +
			"read. Start here: the other tools take a group_id, and " +
			"only groups listed by this tool are queryable. Groups " +
			"not listed -- including every private group and every " +
			"direct message -- are invisible and cannot be reached " +
			"by any filter.",
		InputSchema: objectSchema(map[string]any{"limit": propLimit("")}),
		Handler:     t.listGroups,
	}
}

// ---- telegram_get_group ----

func (t *telegram) getGroup(ctx context.Context, args map[string]any) (any, error) {
	// The batch form keeps the single form's flat object for callers
	// that already parse it.
	if v, ok := args["group_ids"]; ok {
		a, ok := v.([]any)
		if !ok {
			return nil, toolErr("group_ids must be an array of integers")
		}
		if len(a) == 0 {
			return nil, toolErr("group_ids must not be empty")
		}
		if len(a) > maxBatchIDs {
			return nil, toolErr("group_ids accepts at most " + strconv.Itoa(maxBatchIDs) +
				" ids per call")
		}
		groups, missing := []any{}, []any{}
		for _, e := range a {
			g, ok := intValue(e)
			if !ok {
				return nil, toolErr("group_ids must contain integers")
			}
			// One unreadable id must not sink the batch.
			one, err := t.getGroupOne(ctx, g)
			if err != nil {
				if _, isTool := err.(*ToolError); isTool {
					missing = append(missing, g)
					continue
				}
				return nil, err
			}
			groups = append(groups, one)
		}
		return map[string]any{
			"groups":        groups,
			"missing_ids":   missing,
			"count":         len(groups),
			"missing_count": len(missing),
		}, nil
	}
	gid, err := requireInt(args, "group_id")
	if err != nil {
		return nil, err
	}
	return t.getGroupOne(ctx, gid)
}

func (t *telegram) getGroupOne(ctx context.Context, gid int64) (map[string]any, error) {
	bind := strconv.FormatInt(gid, 10)
	rd, err := t.readable(ctx)
	if err != nil {
		return nil, err
	}
	cond, cbinds := rd.groupCond("g.id")
	rows, err := execTool(ctx, t.db,
		"SELECT /*+ MAX_EXECUTION_TIME(5000) */ "+
			"g.id, g.title, g.type, g.description, "+
			"g.msg_count, g.created_at, g.updated_at, "+
			"gf.id AS photo_file_id, "+
			"gf.on_disk AS photo_on_disk "+
			"FROM `telegram_groups` g "+
			"LEFT JOIN telegram_files gf "+
			"  ON gf.id = g.photo_file_id "+
			"WHERE g.id = ?"+cond,
		append([]any{bind}, cbinds...), queryFailed)
	if err != nil {
		return nil, err
	}
	if len(rows) == 0 {
		return nil, toolErr("no readable group with id " + bind + "; see " +
			"telegram_list_groups for what is available")
	}
	r := rows[0]
	out := map[string]any{
		"group_id":      r.i64("id"),
		"title":         r.str("title"),
		"type":          r.str("type"),
		"message_count": r.i64("msg_count"),
		"first_seen":    r.str("created_at"),
		"last_updated":  r.str("updated_at"),
	}
	if d := r.str("description"); d != "" {
		out["description"] = d
	}
	if !r.null("photo_file_id") {
		pid := r.i64("photo_file_id")
		out["photo_file_id"] = pid
		if r.i64("photo_on_disk") != 0 {
			if u := t.fileURL(pid); u != "" {
				out["photo_url"] = u
			}
		}
	}

	urows, err := execTool(ctx, t.db,
		"SELECT username, is_collectible "+
			"FROM telegram_group_usernames "+
			"WHERE group_id = ? AND kind='active' "+
			"ORDER BY position",
		[]any{bind}, queryFailed)
	if err != nil {
		return nil, err
	}
	names := []any{}
	for _, u := range urows {
		j := map[string]any{"username": u.str("username")}
		if u.i64("is_collectible") != 0 {
			j["is_collectible"] = true
		}
		names = append(names, j)
	}
	out["usernames"] = names

	// Covered by (chat_id, sender_user_id), migration 000023.
	sr, err := execTool(ctx, t.db,
		"SELECT /*+ MAX_EXECUTION_TIME(5000) */ "+
			"COUNT(DISTINCT m.sender_user_id) AS n "+
			"FROM telegram_group_messages m "+
			"WHERE m.chat_id = ? "+
			"  AND m.sender_user_id IS NOT NULL",
		[]any{bind}, queryFailed)
	if err != nil {
		return nil, err
	}
	if len(sr) > 0 {
		out["distinct_senders"] = sr[0].i64("n")
	}

	// The span from the two ends of (chat_id, message_id), not an
	// aggregate over date, which no index serves.
	for end := 0; end < 2; end++ {
		dir, key := "ASC", "first_message_date"
		if end == 1 {
			dir, key = "DESC", "last_message_date"
		}
		dr, err := execTool(ctx, t.db,
			"SELECT m.date FROM "+
				"telegram_group_messages m "+
				"WHERE m.chat_id = ? "+
				"ORDER BY m.message_id "+dir+" LIMIT 1",
			[]any{bind}, queryFailed)
		if err != nil {
			return nil, err
		}
		// date 0 is a real stored value for some rows, not a bound.
		if len(dr) > 0 && dr[0].i64("date") != 0 {
			out[key] = dr[0].i64("date")
		}
	}

	ar, err := execTool(ctx, t.db,
		"SELECT COUNT(1) AS n FROM "+
			"telegram_group_admins WHERE group_id = ?",
		[]any{bind}, queryFailed)
	if err != nil {
		return nil, err
	}
	if len(ar) > 0 {
		out["admin_count"] = ar[0].i64("n")
	}
	return out, nil
}

func (t *telegram) getGroupTool() Tool {
	return Tool{
		Name:  "telegram_get_group",
		Title: "Get one group's full details",
		Description: "Everything the archive holds about one readable group: " +
			"title, description, type, every active username, photo, " +
			"message count, how many distinct people have posted, " +
			"how many admins it has, and the span of messages " +
			"recorded.\n\n" +
			"Only groups returned by telegram_list_groups can be " +
			"queried; anything else is reported as not found.\n\n" +
			"Pass group_ids: [...] instead of group_id to look up several " +
			"groups in one call.",
		InputSchema: objectSchema(map[string]any{
			"group_id": propGroupID(),
			"group_ids": map[string]any{
				"type":  "array",
				"items": map[string]any{"type": "integer"},
				"description": "Look up several groups in one call (at " +
					"most " + strconv.Itoa(maxBatchIDs) +
					"). Returns {groups, missing_ids} " +
					"instead of a single group object; " +
					"unreadable ids are reported rather than " +
					"failing the batch.",
			},
		}),
		Handler: t.getGroup,
	}
}

// ---- telegram_get_group_history ----

func (t *telegram) getGroupHistory(ctx context.Context, args map[string]any) (any, error) {
	gid, err := requireInt(args, "group_id")
	if err != nil {
		return nil, err
	}
	bind := strconv.FormatInt(gid, 10)
	lim := strconv.Itoa(clampLimit(args))
	want, err := parseKinds(args, []string{"titles", "descriptions", "usernames", "photos", "admins"})
	if err != nil {
		return nil, err
	}
	out := map[string]any{"group_id": gid}
	// The gate, once, before any history is read.
	rd, err := t.readable(ctx)
	if err != nil {
		return nil, err
	}
	if !rd.hasGroup(gid) {
		return nil, toolErr("no readable group with id " + bind +
			"; see telegram_list_groups for what is available")
	}
	if want[0] {
		if err := t.histList(ctx, out, "titles",
			"SELECT title, created_at FROM "+
				"telegram_group_hist_title WHERE group_id = ? "+
				"ORDER BY id DESC LIMIT "+lim,
			bind, [][2]string{{"title", "title"}}); err != nil {
			return nil, err
		}
	}
	if want[1] {
		if err := t.histList(ctx, out, "descriptions",
			"SELECT description, created_at "+
				"FROM telegram_group_hist_description "+
				"WHERE group_id = ? ORDER BY id DESC LIMIT "+lim,
			bind, [][2]string{{"description", "description"}}); err != nil {
			return nil, err
		}
	}
	if want[2] {
		if err := t.histList(ctx, out, "usernames",
			"SELECT username, action, kind, created_at FROM "+
				"telegram_group_hist_usernames_events "+
				"WHERE group_id = ? ORDER BY id DESC LIMIT "+lim,
			bind, [][2]string{{"username", "username"}, {"action", "action"}, {"kind", "kind"}}); err != nil {
			return nil, err
		}
	}
	if want[3] {
		rows, err := execTool(ctx, t.db,
			"SELECT h.file_id, h.created_at, f.on_disk "+
				"FROM telegram_group_hist_photo h "+
				"LEFT JOIN telegram_files f ON f.id = h.file_id "+
				"WHERE h.group_id = ? "+
				"ORDER BY h.id DESC LIMIT "+lim,
			[]any{bind}, queryFailed)
		if err != nil {
			return nil, err
		}
		out["photos"] = t.photoList(rows)
	}
	if want[4] {
		rows, err := execTool(ctx, t.db,
			"SELECT h.user_id, h.action, h.status, h.custom_title, "+
				"h.is_anonymous, h.created_at, "+
				"u.first_name, u.last_name, "+
				usernameSubq("h.user_id")+" AS username "+
				"FROM telegram_group_admin_hist h "+
				"LEFT JOIN telegram_users u ON u.id = h.user_id "+
				"WHERE h.group_id = ? "+
				"ORDER BY h.id DESC LIMIT "+lim,
			[]any{bind}, queryFailed)
		if err != nil {
			return nil, err
		}
		admins := []any{}
		for _, r := range rows {
			j := map[string]any{
				"user_id":     r.i64("user_id"),
				"username":    r.str("username"),
				"first_name":  r.str("first_name"),
				"last_name":   r.str("last_name"),
				"action":      r.str("action"),
				"status":      r.str("status"),
				"observed_at": r.str("created_at"),
			}
			if ct := r.str("custom_title"); ct != "" {
				j["custom_title"] = ct
			}
			if r.i64("is_anonymous") != 0 {
				j["is_anonymous"] = true
			}
			admins = append(admins, j)
		}
		out["admins"] = admins
	}
	return out, nil
}

// photoList is a photo history: the file, a URL when its bytes are
// stored, and when it was observed.
func (t *telegram) photoList(rows []row) []any {
	list := []any{}
	for _, r := range rows {
		fid := r.i64("file_id")
		j := map[string]any{"file_id": fid, "observed_at": r.str("created_at")}
		if r.i64("on_disk") != 0 {
			if u := t.fileURL(fid); u != "" {
				j["url"] = u
			}
		}
		list = append(list, j)
	}
	return list
}

func (t *telegram) getGroupHistoryTool() Tool {
	return Tool{
		Name:  "telegram_get_group_history",
		Title: "Get a group's change history",
		Description: "How a readable group has changed over time: titles, " +
			"descriptions, usernames, photos, and administrator " +
			"promotions, demotions and privilege changes -- newest " +
			"first.\n\n" +
			"As with user history, timestamps are when a change was " +
			"OBSERVED. Group metadata and the admin list are both " +
			"polled rather than pushed, so a change made and " +
			"reverted between polls leaves no trace, and the oldest " +
			"entry of each kind is usually the value at first sight " +
			"rather than a change.\n\n" +
			"Pass `kinds` to fetch only some categories; the default " +
			"is all of them.",
		InputSchema: objectSchema(map[string]any{
			"group_id": propGroupID(),
			"kinds": map[string]any{
				"type":  "array",
				"items": map[string]any{"type": "string"},
				"description": "Any of: titles, descriptions, " +
					"usernames, photos, admins. " +
					"Default all.",
			},
			"limit": map[string]any{
				"type":        "integer",
				"description": "Entries per category, 1-200, default 50.",
			},
		}, "group_id"),
		Handler: t.getGroupHistory,
	}
}

// ---- telegram_list_group_admins ----

var adminRights = []string{
	"can_manage_chat", "can_change_info",
	"can_post_messages", "can_edit_messages",
	"can_delete_messages", "can_invite_users",
	"can_restrict_members", "can_pin_messages",
	"can_manage_topics", "can_promote_members",
	"can_manage_video_chats", "can_post_stories",
	"can_edit_stories", "can_delete_stories",
	"can_manage_direct_messages", "can_manage_tags",
}

func (t *telegram) listGroupAdmins(ctx context.Context, args map[string]any) (any, error) {
	gid, err := requireInt(args, "group_id")
	if err != nil {
		return nil, err
	}
	// The gate as a WHERE condition: an unexposed group has no rows,
	// exactly as if it had no admins.
	rd, err := t.readable(ctx)
	if err != nil {
		return nil, err
	}
	cond, cbinds := rd.groupCond("a.group_id")
	stmt := "SELECT /*+ MAX_EXECUTION_TIME(5000) */ " +
		"a.user_id, a.status, a.custom_title, " +
		"a.joined_date, a.is_anonymous, " +
		"a.inviter_user_id, " +
		"a.can_manage_chat, a.can_change_info, " +
		"a.can_post_messages, a.can_edit_messages, " +
		"a.can_delete_messages, a.can_invite_users, " +
		"a.can_restrict_members, a.can_pin_messages, " +
		"a.can_manage_topics, a.can_promote_members, " +
		"a.can_manage_video_chats, a.can_post_stories, " +
		"a.can_edit_stories, a.can_delete_stories, " +
		"a.can_manage_direct_messages, a.can_manage_tags, " +
		"u.first_name, u.last_name, u.type, " +
		"u.is_verified, u.is_premium, " +
		usernameSubq("a.user_id") + " AS username " +
		"FROM telegram_group_admins a " +
		"LEFT JOIN telegram_users u ON u.id = a.user_id " +
		"WHERE a.group_id = ?" + cond + " " +
		"ORDER BY (a.status = 'creator') DESC, " +
		"         a.joined_date ASC " +
		"LIMIT " + strconv.Itoa(clampLimit(args))
	rows, err := execTool(ctx, t.db, stmt, append([]any{strconv.FormatInt(gid, 10)}, cbinds...), queryFailed)
	if err != nil {
		return nil, err
	}
	admins := []any{}
	for _, r := range rows {
		j := map[string]any{
			"user_id":      r.i64("user_id"),
			"username":     r.str("username"),
			"first_name":   r.str("first_name"),
			"last_name":    r.str("last_name"),
			"status":       r.str("status"),
			"custom_title": r.str("custom_title"),
			"is_anonymous": r.i64("is_anonymous") != 0,
			"is_bot":       r.str("type") == "bot",
			"is_verified":  r.i64("is_verified") != 0,
			"is_premium":   r.i64("is_premium") != 0,
		}
		if v := r.i64("joined_date"); v != 0 {
			j["joined_date"] = v
		}
		if v := r.i64("inviter_user_id"); v != 0 {
			j["promoted_by_user_id"] = v
		}
		// Only the rights actually held.
		perms := []any{}
		for _, k := range adminRights {
			if r.i64(k) != 0 {
				perms = append(perms, k)
			}
		}
		j["permissions"] = perms
		admins = append(admins, j)
	}
	return map[string]any{"group_id": gid, "admins": admins, "count": len(admins)}, nil
}

func (t *telegram) listGroupAdminsTool() Tool {
	return Tool{
		Name:  "telegram_list_group_admins",
		Title: "List a group's administrators",
		Description: "List the administrators of one readable group, with " +
			"their privileges. The owner is listed first, then " +
			"other admins oldest-promotion first.\n\n" +
			"Only groups returned by telegram_list_groups can be " +
			"queried; anything else yields an empty list rather " +
			"than an error.\n\n" +
			"Note this is a snapshot, not a live read. Telegram " +
			"does not push admin changes to a regular account, so " +
			"the list is refreshed by periodic polling and may lag " +
			"a very recent promotion or demotion.",
		InputSchema: objectSchema(map[string]any{
			"group_id": propGroupID(),
			"limit":    propLimit(""),
		}, "group_id"),
		Handler: t.listGroupAdmins,
	}
}

// ---- telegram_list_recent_messages ----

func (t *telegram) listRecentMessages(ctx context.Context, args map[string]any) (any, error) {
	g, err := t.msgGate(ctx, args)
	if err != nil {
		return nil, err
	}
	// The gate's own list: an unreadable group simply has no rows to
	// offer.
	var groups []int64
	for _, b := range g.binds {
		groups = append(groups, b.(int64))
	}
	o, err := parseRowOpts(args)
	if err != nil {
		return nil, err
	}
	limit := clampLimit(args)
	offset, err := clampOffset(args)
	if err != nil {
		return nil, err
	}
	out, err := t.listRecent(ctx, g.src, groups, limit, offset, &o)
	if err != nil {
		return nil, err
	}
	if o.hoistGroup {
		g, _ := intValue(args["group_id"])
		out["group_id"] = g
	}
	return out, nil
}

func (t *telegram) listRecentMessagesTool() Tool {
	return Tool{
		Name:  "telegram_list_recent_messages",
		Title: "Recent Telegram messages",
		Description: "The most recent messages, newest first, across every " +
			"readable group or within one group. Use this for " +
			"\"what has been said lately\"; use " +
			"telegram_search_messages when you need conditions.",
		InputSchema: inputSchemaForMessages(false),
		Handler:     t.listRecentMessages,
	}
}

// ---- telegram_list_group_senders ----

func (t *telegram) listGroupSenders(ctx context.Context, args map[string]any) (any, error) {
	gid, err := requireInt(args, "group_id")
	if err != nil {
		return nil, err
	}
	limit := clampLimit(args)
	offset, err := clampOffset(args)
	if err != nil {
		return nil, err
	}
	dr, err := parseDateRange(args, 0)
	if err != nil {
		return nil, err
	}
	// Aggregate first, decorate second: the inner statement is covered
	// by (chat_id, sender_user_id), or (chat_id, date, sender_user_id)
	// with a date range; the joins run over at most limit rows. A NULL
	// sender is the chat itself (a channel post, an anonymous admin);
	// in a private chat the logging account's messages count as its
	// own once its id is known (private.go), before that as NULL. No
	// hint inside: MySQL ignores it in a derived table.
	g, err := t.chatGate(ctx, gid, true)
	if err != nil {
		return nil, err
	}
	src := g.src
	inner := "SELECT " + src.sender + " AS uid, COUNT(1) AS n " +
		"FROM " + src.table + " m " +
		"WHERE m.chat_id = ? " +
		"  AND " + src.sender + " IS NOT NULL" +
		g.sql +
		dr.sql +
		" GROUP BY " + src.sender + " " +
		"ORDER BY n DESC LIMIT " + strconv.Itoa(limit) +
		" OFFSET " + strconv.Itoa(offset)
	stmt := "SELECT /*+ MAX_EXECUTION_TIME(5000) */ " +
		"s.uid, s.n, u.first_name, u.last_name, " +
		"u.type, u.is_verified, u.is_premium, " +
		"u.msg_count AS total_msgs, " +
		usernameSubq("s.uid") + " AS username " +
		"FROM (" + inner + ") s " +
		"LEFT JOIN telegram_users u ON u.id = s.uid " +
		"ORDER BY s.n DESC"
	binds := append([]any{strconv.FormatInt(gid, 10)}, g.binds...)
	binds = append(binds, dr.binds...)
	rows, err := execTool(ctx, t.db, stmt, binds, queryFailed)
	if err != nil {
		return nil, err
	}
	senders := []any{}
	for _, r := range rows {
		senders = append(senders, map[string]any{
			"user_id":           r.i64("uid"),
			"username":          r.str("username"),
			"first_name":        r.str("first_name"),
			"last_name":         r.str("last_name"),
			"is_bot":            r.str("type") == "bot",
			"is_verified":       r.i64("is_verified") != 0,
			"is_premium":        r.i64("is_premium") != 0,
			"messages_in_group": r.i64("n"),
			"messages_total":    r.i64("total_msgs"),
		})
	}
	out := map[string]any{
		"group_id": gid,
		"senders":  senders,
		"count":    len(senders),
		"limit":    limit,
		"offset":   offset,
	}
	dr.stamp(out)
	if b, ok := boolArg(args, "include_total"); ok && b {
		csql := "SELECT /*+ MAX_EXECUTION_TIME(5000) */ " +
			"COUNT(DISTINCT " + src.sender + ") AS n " +
			"FROM " + src.table + " m " +
			"WHERE m.chat_id = ? " +
			"  AND " + src.sender + " IS NOT NULL" +
			g.sql +
			dr.sql
		cr, err := execTool(ctx, t.db, csql, binds, "count failed")
		if err != nil {
			return nil, err
		}
		if len(cr) == 0 {
			out["total_senders"] = 0
		} else {
			out["total_senders"] = cr[0].i64("n")
		}
	}
	return out, nil
}

var dateBoundProps = map[string]string{
	"start_date": "Only count messages on or after this " +
		"date. YYYY-MM-DD or a unix " +
		"timestamp. Omit for all time.",
	"end_date": "Only count messages on or before " +
		"this date. Omit for all time.",
}

func (t *telegram) listGroupSendersTool() Tool {
	return Tool{
		Name:  "telegram_list_group_senders",
		Title: "Message-count leaderboard for a group",
		Description: "A leaderboard of who posts most in one readable group: " +
			"every user who has sent a message, ordered by message " +
			"count descending, with the count for each.\n\n" +
			"Pass start_date and/or end_date to count only part of " +
			"the history; with neither, it counts all time.\n\n" +
			"This is participation, not membership: it can only " +
			"see people who have posted, so lurkers and members who " +
			"joined without speaking do not appear. Counts include " +
			"messages later deleted, since the archive keeps them.\n\n" +
			"Channel posts and messages from anonymous admins are " +
			"excluded, as those are sent by the channel rather than " +
			"by a user.\n\n" +
			"Only groups returned by telegram_list_groups can be " +
			"queried.",
		InputSchema: objectSchema(map[string]any{
			"group_id":   propGroupID(),
			"limit":      propLimit(""),
			"offset":     map[string]any{"type": "integer"},
			"start_date": map[string]any{"description": dateBoundProps["start_date"]},
			"end_date":   map[string]any{"description": dateBoundProps["end_date"]},
			"include_total": map[string]any{
				"type": "boolean",
				"description": "Also return how many distinct users " +
					"have posted. Costs a second pass.",
			},
		}, "group_id"),
		Handler: t.listGroupSenders,
	}
}

// ---- telegram_count_user_messages ----

func (t *telegram) countUserMessages(ctx context.Context, args map[string]any) (any, error) {
	gid, err := requireInt(args, "group_id")
	if err != nil {
		return nil, err
	}
	uid, err := requireInt(args, "user_id")
	if err != nil {
		return nil, err
	}
	dr, err := parseDateRange(args, 0)
	if err != nil {
		return nil, err
	}
	g, err := t.chatGate(ctx, gid, true)
	if err != nil {
		return nil, err
	}
	stmt := "SELECT /*+ MAX_EXECUTION_TIME(5000) */ " +
		"COUNT(1) AS n, MIN(m.date) AS first_date, " +
		"MAX(m.date) AS last_date " +
		"FROM " + g.src.table + " m " +
		"WHERE m.chat_id = ? AND " + g.src.sender + " = ?" +
		g.sql +
		dr.sql
	binds := append([]any{strconv.FormatInt(gid, 10), strconv.FormatInt(uid, 10)}, g.binds...)
	binds = append(binds, dr.binds...)
	out := map[string]any{"group_id": gid, "user_id": uid}
	rows, err := execTool(ctx, t.db, stmt, binds, queryFailed)
	if err != nil {
		return nil, err
	}
	var n int64
	if len(rows) > 0 {
		n = rows[0].i64("n")
	}
	out["message_count"] = n
	if n != 0 {
		out["first_message_date"] = rows[0].i64("first_date")
		out["last_message_date"] = rows[0].i64("last_date")
	}
	dr.stamp(out)

	// A zero alone is ambiguous: say when the group is not readable.
	if n == 0 && g.nothing {
		rd, err := t.readable(ctx)
		if err != nil {
			return nil, err
		}
		if rd.exposed[gid] {
			out["note"] = "That group is outside this " +
				"token's scope, so the count is " +
				"zero regardless of what the " +
				"user posted. See " +
				"telegram_list_groups."
		} else {
			out["note"] = "That group is not exposed to " +
				"MCP, so the count is zero " +
				"regardless of what the user " +
				"posted. See " +
				"telegram_list_groups."
		}
	}

	// Decoration only, never a failure of the count.
	if u, err := queryRows(ctx, t.db,
		"SELECT u.first_name, u.last_name, "+
			"(SELECT x.username FROM "+
			"  telegram_user_usernames x "+
			"  WHERE x.user_id = u.id AND "+
			"        x.kind = 'active' "+
			"  ORDER BY x.position LIMIT 1) AS username "+
			"FROM telegram_users u WHERE u.id = ?",
		[]any{strconv.FormatInt(uid, 10)}); err == nil && len(u) > 0 {
		out["username"] = u[0].str("username")
		out["first_name"] = u[0].str("first_name")
		out["last_name"] = u[0].str("last_name")
	}
	return out, nil
}

func (t *telegram) countUserMessagesTool() Tool {
	return Tool{
		Name:  "telegram_count_user_messages",
		Title: "Count one user's messages in one group",
		Description: "How many messages a single user has sent to a single " +
			"readable group.\n\n" +
			"Use this when the question is about one person -- " +
			"\"how much has @alice posted in GNU/Weeb?\" -- rather " +
			"than about the group as a whole; for a ranking of " +
			"everyone use telegram_list_group_senders.\n\n" +
			"Pass start_date and/or end_date to count only part of " +
			"the history; with neither, it counts all time.\n\n" +
			"Counts include messages later deleted, since the " +
			"archive keeps them. Messages the user sent " +
			"anonymously (as the group) are not attributed to " +
			"them and so are not counted.\n\n" +
			"Only groups returned by telegram_list_groups can be " +
			"queried.",
		InputSchema: objectSchema(map[string]any{
			"group_id": propGroupID(),
			"user_id": map[string]any{
				"type": "integer",
				"description": "The user's id. Resolve a username " +
					"with telegram_get_users first.",
			},
			"start_date": map[string]any{"description": dateBoundProps["start_date"]},
			"end_date":   map[string]any{"description": dateBoundProps["end_date"]},
		}, "group_id", "user_id"),
		Handler: t.countUserMessages,
	}
}

// ---- telegram_popular_words ----

func wordFilterFromArgs(args map[string]any) (*wordFilter, error) {
	f := &wordFilter{
		minLen:       clampIntArg(args, "min_length", 3, 1, 32),
		useStopwords: true,
		extra:        map[string]struct{}{},
	}
	if b, ok := boolArg(args, "include_stopwords"); ok {
		f.useStopwords = !b
	}
	if v, ok := args["exclude"]; ok {
		a, ok := v.([]any)
		if !ok {
			return nil, toolErr("exclude must be an array of strings")
		}
		if len(a) > 200 {
			return nil, toolErr("exclude holds at most 200 words")
		}
		for _, e := range a {
			s, ok := e.(string)
			if !ok {
				return nil, toolErr("exclude must be an array of strings")
			}
			b := []byte(s)
			for i, c := range b {
				if c >= 'A' && c <= 'Z' {
					b[i] = c + 'a' - 'A'
				}
			}
			f.extra[string(b)] = struct{}{}
		}
	}
	return f, nil
}

func (t *telegram) popularWords(ctx context.Context, args map[string]any) (any, error) {
	gid, err := requireInt(args, "group_id")
	if err != nil {
		return nil, err
	}
	limit := clampLimit(args)
	// The one tool with a default window: "popular words lately" is
	// the useful and affordable question, "ever" neither.
	dr, err := parseDateRange(args, t.now().Unix()-30*86400)
	if err != nil {
		return nil, err
	}
	g, err := t.msgGate(ctx, args)
	if err != nil {
		return nil, err
	}
	wf, err := wordFilterFromArgs(args)
	if err != nil {
		return nil, err
	}
	scanCap := clampIntArg(args, "max_messages", 50000, 1, 200000)

	// Service messages are Telegram's text, not the conversation. By
	// date, not message_id: the range is on date, so
	// (chat_id, date, sender_user_id) supplies the order too.
	stmt := "SELECT /*+ MAX_EXECUTION_TIME(5000) */ m.text " +
		"FROM " + g.src.table + " m " +
		"WHERE m.chat_id = ? AND m.text IS NOT NULL " +
		"  AND m.content_type <> 'service'" +
		g.sql + dr.sql +
		" ORDER BY m.date DESC LIMIT " + strconv.Itoa(scanCap)
	binds := append([]any{strconv.FormatInt(gid, 10)}, g.binds...)
	binds = append(binds, dr.binds...)
	rows, err := execTool(ctx, t.db, stmt, binds, queryFailed)
	if err != nil {
		return nil, err
	}
	freq := map[string]int64{}
	var scanned, words int64
	for _, r := range rows {
		scanned++
		countWords(r.str("text"), wf, freq, &words)
	}
	type wc struct {
		w string
		n int64
	}
	top := make([]wc, 0, len(freq))
	for w, n := range freq {
		top = append(top, wc{w, n})
	}
	sort.Slice(top, func(i, j int) bool {
		if top[i].n != top[j].n {
			return top[i].n > top[j].n
		}
		return top[i].w < top[j].w
	})
	keep := min(limit, len(top))
	list := []any{}
	for _, e := range top[:keep] {
		pct := 0.0
		if words != 0 {
			// Share of all counted words, not of messages.
			pct = float64(e.n) * 100.0 / float64(words)
		}
		list = append(list, map[string]any{"word": e.w, "count": e.n, "percent": pct})
	}
	out := map[string]any{
		"group_id":           gid,
		"start_date":         dr.start,
		"end_date":           nil,
		"words":              list,
		"count":              len(list),
		"messages_scanned":   scanned,
		"words_counted":      words,
		"distinct_words":     int64(len(freq)),
		"truncated":          scanned >= int64(scanCap),
		"stopwords_filtered": wf.useStopwords,
	}
	if dr.hasEnd {
		out["end_date"] = dr.end
	}
	if scanned == 0 {
		out["note"] = "No messages with text in that window. " +
			"Check the group is exposed (see " +
			"telegram_list_groups) and widen the " +
			"date range."
	}
	return out, nil
}

func (t *telegram) popularWordsTool() Tool {
	return Tool{
		Name:  "telegram_popular_words",
		Title: "Most-used words in a group",
		Description: "What a readable group actually talks about: the words " +
			"used most often in its messages, commonest first.\n\n" +
			"Noise is filtered so the result is topical rather than " +
			"grammatical. Excluded: Indonesian and English " +
			"stopwords (\"yang\", \"the\", \"untuk\", \"and\"); chat " +
			"slang and shorthand in both languages (\"gak\", " +
			"\"aja\", \"wkwk\", \"lol\", \"btw\"); laughter and filler " +
			"of any length (\"wkwkwkwk\", \"hahahaha\"); quoted-mail " +
			"and calendar scaffolding (\"subject\", \"wrote\", " +
			"\"jul\"); URLs, e-mail addresses and pure numbers. " +
			"Elongation is collapsed, so \"yesss\" counts as " +
			"\"yes\".\n\n" +
			"Pass include_stopwords: true to switch all of that off " +
			"and get raw frequencies, or exclude: [...] to drop " +
			"further words specific to your question.\n\n" +
			"With no dates this covers the last 30 days, not all " +
			"time -- word frequency is a snapshot of what is being " +
			"discussed now. Pass start_date and/or end_date for any " +
			"other window.\n\n" +
			"Counting reads message text, so it scans at most " +
			"max_messages (newest first) and reports " +
			"messages_scanned and truncated; when truncated is " +
			"true the counts describe that newest slice, not the " +
			"whole window.\n\n" +
			"Only groups returned by telegram_list_groups can be " +
			"queried.",
		InputSchema: objectSchema(map[string]any{
			"group_id": propGroupID(),
			"limit":    propLimit("How many words to return. 1-200, default 50."),
			"start_date": map[string]any{"description": "Window start. YYYY-MM-DD or a unix " +
				"timestamp. Default: 30 days ago."},
			"end_date": map[string]any{"description": "Window end. Default: now."},
			"min_length": map[string]any{
				"type": "integer",
				"description": "Ignore words shorter than this. " +
					"1-32, default 3.",
			},
			"max_messages": map[string]any{
				"type": "integer",
				"description": "Cap on messages scanned, newest " +
					"first. 1-200000, default 50000.",
			},
			"exclude": map[string]any{
				"type":  "array",
				"items": map[string]any{"type": "string"},
				"description": "Further words to drop, on top of " +
					"the built-in lists. Up to 200.",
			},
			"include_stopwords": map[string]any{
				"type": "boolean",
				"description": "Count every word, filtering nothing " +
					"but `exclude`, min_length and pure " +
					"numbers. Default false.",
			},
		}, "group_id"),
		Handler: t.popularWords,
	}
}

// ---- telegram_search_messages ----

func (t *telegram) searchMessages(ctx context.Context, args map[string]any) (any, error) {
	// group_id is folded into the gate, one constant list for the
	// optimiser instead of two conditions; the gate also picks the
	// table, whose columns the filter is written in.
	g, err := t.msgGate(ctx, args)
	if err != nil {
		return nil, err
	}
	var c compiled
	if v, ok := args["filter"]; ok {
		if c, err = compile(g.src.schema, v); err != nil {
			return nil, err
		}
	}
	q := &queryReq{whereExtra: c.sql, binds: c.binds, gate: g}
	if q.order, err = parseOrder(args, c.ftExpr); err != nil {
		return nil, err
	}
	if q.order.key == "relevance" {
		q.orderBind = c.ftValue
	}
	q.limit = clampIntArg(args, "limit", defaultLimit, 0, maxLimit)
	if q.offset, err = clampOffset(args); err != nil {
		return nil, err
	}
	if b, ok := boolArg(args, "include_total"); ok {
		q.includeTotal = b
	}
	if q.row, err = parseRowOpts(args); err != nil {
		return nil, err
	}
	if q.row.hoistGroup {
		q.hoistedGroup, _ = intValue(args["group_id"])
	}
	if s, ok := stringArg(args, "cursor"); ok {
		q.cursor = cursorDecode(s)
		if !q.cursor.ok {
			return nil, toolErr("cursor is not one this server issued; " +
				"pass back next_cursor verbatim, or omit " +
				"it to start over")
		}
		if q.cursor.key != q.order.key {
			return nil, toolErr(`this cursor was issued for order_by "` +
				q.cursor.key + `" but the call asks for "` + q.order.key +
				`"; a cursor is only valid for the ordering that produced it`)
		}
		if !q.order.cursorable {
			return nil, toolErr("relevance ordering has no cursor; add " +
				"an explicit order_by to page")
		}
	}
	return t.runMessageQuery(ctx, q)
}

func (t *telegram) searchMessagesTool() Tool {
	return Tool{
		Name:  "telegram_search_messages",
		Title: "Search Telegram messages",
		Description: "Search messages with a filter tree supporting AND, OR " +
			"and NOT.\n\nFields:\n" +
			describeFields(msgSchema) +
			"\nOperators: =, !=, <, >, <=, >=, contains, " +
			"not_contains, starts_with, in, between, is_null, " +
			"is_not_null, match.\n\n" +
			"Example -- messages mentioning \"kernel\" in either of " +
			"two groups, excluding photos:\n" +
			"{\"and\":[{\"field\":\"text\",\"op\":\"match\"," +
			"\"value\":\"kernel\"}," +
			"{\"or\":[{\"field\":\"group_id\",\"op\":\"=\"," +
			"\"value\":-1001},{\"field\":\"group_id\",\"op\":\"=\"," +
			"\"value\":-1002}]}," +
			"{\"not\":{\"field\":\"content_type\",\"op\":\"=\"," +
			"\"value\":\"photo\"}}]}\n\n" +
			"Only readable groups are searched; see " +
			"telegram_list_groups.\n\n" +
			"WORKING WITH EDITS AND DELETIONS. `date` is when a message " +
			"was SENT, so a message sent in January and edited in August " +
			"appears in no window on `date` -- filter on `edit_date` or " +
			"`deleted_at` instead, and order by them. Every edited row " +
			"carries previous_version_count, so you can tell which edits " +
			"actually have a recoverable revision WITHOUT a follow-up " +
			"call, then fetch just those with telegram_get_messages " +
			"(include_history: true).\n\n" +
			"Example -- everything edited in one week, cheapest form:\n" +
			"{\"filter\":{\"field\":\"edit_date\",\"op\":\"between\"," +
			"\"value\":[\"2026-01-01\",\"2026-01-08\"]}," +
			"\"order_by\":\"edit_date\",\"limit\":200," +
			"\"fields\":[\"message_id\",\"sender_username\"," +
			"\"edit_date\",\"previous_version_count\"]," +
			"\"include_media\":false,\"compact\":true}\n\n" +
			"To exclude bot noise without knowing any bot's id, add " +
			"{\"not\":{\"field\":\"sender_is_bot\",\"op\":\"=\"," +
			"\"value\":true}}.\n\n" +
			"SIZE AND PAGING. Use fields/truncate_text/include_media/" +
			"compact to keep a wide scan small, cursor (not offset) to " +
			"page a live archive, and limit: 0 with include_total when you " +
			"only need to know how big the job is.",
		InputSchema: inputSchemaForMessages(true),
		Handler:     t.searchMessages,
	}
}

// ---- telegram_get_messages ----

// editsForRows is every edit snapshot of a set of already authorised
// message rows, by row, in one statement for the whole batch.
func (t *telegram) editsForRows(ctx context.Context, src *msgSource, rowIDs []int64) (map[int64][]any, error) {
	out := map[int64][]any{}
	if len(rowIDs) == 0 {
		return out, nil
	}
	binds := make([]any, len(rowIDs))
	for i, id := range rowIDs {
		binds[i] = strconv.FormatInt(id, 10)
	}
	stmt := "SELECT /*+ MAX_EXECUTION_TIME(5000) */ " +
		"e." + src.editKey + " AS owner_id, e.content_type, e.text, e.edit_date, " +
		"e.created_at, " +
		"f.id AS file_id, f.file_type, f.file_size, f.on_disk " +
		"FROM " + src.edits + " e " +
		"LEFT JOIN telegram_files f ON f.id = e.file_id " +
		"WHERE e." + src.editKey + " IN (" + placeholders(len(rowIDs), ",") + ") ORDER BY e.id ASC"
	rows, err := execTool(ctx, t.db, stmt, binds, queryFailed)
	if err != nil {
		return nil, err
	}
	for _, e := range rows {
		owner := e.i64("owner_id")
		v := map[string]any{
			"content_type": e.str("content_type"),
			"text":         e.str("text"),
			"observed_at":  e.str("created_at"),
		}
		if d := e.i64("edit_date"); d != 0 {
			v["edit_date"] = d
		}
		if !e.null("file_id") {
			v["media"] = t.mediaJSON(e, false)
		}
		out[owner] = append(out[owner], v)
	}
	return out, nil
}

func (t *telegram) getMessages(ctx context.Context, args map[string]any) (any, error) {
	gid, err := requireInt(args, "group_id")
	if err != nil {
		return nil, err
	}
	in, ok := args["message_ids"].([]any)
	if !ok {
		return nil, toolErr("message_ids is required and must be an array of integers")
	}
	if len(in) == 0 {
		return nil, toolErr("message_ids must not be empty")
	}
	if len(in) > maxBatchIDs {
		return nil, toolErr("message_ids accepts at most " + strconv.Itoa(maxBatchIDs) +
			" ids per call; split the batch")
	}
	var want []int64
	binds := []any{strconv.FormatInt(gid, 10)}
	for _, v := range in {
		id, ok := intValue(v)
		if !ok {
			return nil, toolErr("message_ids must contain integers")
		}
		want = append(want, id)
		binds = append(binds, strconv.FormatInt(id, 10))
	}
	withHistory := false
	if b, ok := boolArg(args, "include_history"); ok {
		withHistory = b
	}
	g, err := t.msgGate(ctx, args)
	if err != nil {
		return nil, err
	}
	o, err := parseRowOpts(args)
	if err != nil {
		return nil, err
	}
	// The group is a call parameter by definition here.
	o.hoistGroup = o.compact

	out := map[string]any{"group_id": gid}
	found := []any{}
	var rowIDs []int64
	seen := map[int64]bool{}
	if !g.nothing {
		stmt := g.src.selectCols +
			"FROM " + g.src.table + " m " + g.src.joins +
			"WHERE m.chat_id = ? AND m.message_id IN (" + placeholders(len(want), ",") + ")" +
			g.sql + " ORDER BY m.message_id ASC"
		rows, err := execTool(ctx, t.db, stmt, append(binds, g.binds...), queryFailed)
		if err != nil {
			return nil, err
		}
		for _, r := range rows {
			seen[r.i64("message_id")] = true
			j := t.messageRow(r, &o)
			if o.hoistGroup && !has(out, "group_title") {
				out["group_title"] = r.str("group_title")
			}
			if withHistory {
				rowIDs = append(rowIDs, r.i64("id_internal"))
			}
			found = append(found, j)
		}
	}
	// An unknown id is reported, never fatal; an id in an unexposed
	// group is missing too, as saying otherwise would confirm it.
	missing := []any{}
	for _, id := range want {
		if !seen[id] {
			missing = append(missing, id)
		}
	}
	if withHistory {
		byOwner, err := t.editsForRows(ctx, g.src, rowIDs)
		if err != nil {
			return nil, err
		}
		for i, rid := range rowIDs {
			j := found[i].(map[string]any)
			pv := byOwner[rid]
			if pv == nil {
				pv = []any{}
			}
			j["previous_versions"] = pv
			j["previous_version_count"] = len(pv)
			// Edited and recoverable are not the same thing.
			if ed, _ := j["is_edited"].(bool); ed && len(pv) == 0 {
				j["note"] = "marked edited, but no earlier version " +
					"was captured -- the edit predates " +
					"archiving or happened while the " +
					"daemon was down"
			}
		}
	}
	out["found"] = found
	out["missing_ids"] = missing
	out["count"] = len(found)
	out["missing_count"] = len(missing)
	return out, nil
}

func (t *telegram) getMessagesTool() Tool {
	props := map[string]any{
		"group_id": propGroupID(),
		"message_ids": map[string]any{
			"type":        "array",
			"items":       map[string]any{"type": "integer"},
			"description": "The message ids within that group; at most " + strconv.Itoa(maxBatchIDs) + ".",
		},
		"include_history": map[string]any{
			"type": "boolean",
			"description": "Default false. When true each message also carries " +
				"previous_versions (oldest first) and " +
				"previous_version_count -- collapsing N history calls into " +
				"this one.",
		},
	}
	addRowShapeProps(props)
	return Tool{
		Name:  "telegram_get_messages",
		Title: "Fetch many messages by id, optionally with their revisions",
		Description: "Fetch up to " + strconv.Itoa(maxBatchIDs) +
			" messages of one group by id, in a single call.\n\n" +
			"USE THIS INSTEAD OF LOOPING. With include_history: true it " +
			"also inlines every earlier version and the deletion record " +
			"for each message, which is what telegram_get_message_history " +
			"returns for ONE message -- so a set of fifty edited messages " +
			"costs one call here rather than fifty there.\n\n" +
			"Unknown ids never fail the batch: they come back in " +
			"missing_ids while everything found comes back in found. An id " +
			"in a group that is not readable is reported missing.\n\n" +
			"Typical two-call workflow for \"what was edited this week, " +
			"and what changed\":\n" +
			"  1. telegram_search_messages with filter {\"field\":" +
			"\"edit_date\",\"op\":\"between\",\"value\":[\"2026-01-01\"," +
			"\"2026-01-08\"]}, order_by edit_date, " +
			"fields [\"message_id\",\"previous_version_count\"]\n" +
			"  2. telegram_get_messages with those message_ids and " +
			"include_history: true\n\n" +
			"The size controls (fields, truncate_text, include_media, " +
			"compact) work here too.",
		InputSchema: objectSchema(props, "group_id", "message_ids"),
		Handler:     t.getMessages,
	}
}

// ---- telegram_get_message_history ----

func (t *telegram) getMessageHistory(ctx context.Context, args map[string]any) (any, error) {
	gid, err := requireInt(args, "group_id")
	if err != nil {
		return nil, err
	}
	mid, err := requireInt(args, "message_id")
	if err != nil {
		return nil, err
	}
	g, err := t.msgGate(ctx, args)
	if err != nil {
		return nil, err
	}
	binds := append([]any{strconv.FormatInt(gid, 10), strconv.FormatInt(mid, 10)}, g.binds...)
	// The pair is covered by uq_group_messages_chat_msg; the gate is
	// in the same statement.
	rows, err := execTool(ctx, t.db,
		"SELECT /*+ MAX_EXECUTION_TIME(5000) */ "+
			"m.id, m.content_type, m.text, m.date, "+
			"m.edit_date, m.deleted_at, "+g.src.sender+" AS sender_user_id, "+
			"FROM_UNIXTIME(m.date) AS sent_at"+g.src.outgoingCol()+", "+
			"f.id AS file_id, f.file_type, f.file_size, f.on_disk "+
			"FROM "+g.src.table+" m "+
			"LEFT JOIN telegram_files f ON f.id = m.file_id "+
			"WHERE m.chat_id = ? AND m.message_id = ?"+g.sql,
		binds, queryFailed)
	if err != nil {
		return nil, err
	}
	if len(rows) == 0 {
		return nil, toolErr("no readable message " + strconv.FormatInt(mid, 10) +
			" in group " + strconv.FormatInt(gid, 10))
	}
	r := rows[0]
	cur := map[string]any{
		"content_type": r.str("content_type"),
		"text":         r.str("text"),
		"sent_at":      r.str("sent_at"),
		"date":         r.i64("date"),
	}
	if !r.null("file_id") {
		cur["media"] = t.mediaJSON(r, false)
	}
	edited := r.i64("edit_date") != 0
	deleted := !r.null("deleted_at")
	out := map[string]any{
		"group_id":       gid,
		"message_id":     mid,
		"sender_user_id": r.i64("sender_user_id"),
		"is_edited":      edited,
		"is_deleted":     deleted,
		"current":        cur,
	}
	if _, ok := r["is_outgoing"]; ok {
		out["is_outgoing"] = r.i64("is_outgoing") != 0
	}
	if edited {
		out["last_edit_date"] = r.i64("edit_date")
	}
	if deleted {
		// When the deletion was NOTICED; the content is still here.
		out["deleted_observed_at"] = r.str("deleted_at")
	}

	format := "full"
	if s, ok := stringArg(args, "format"); ok {
		format = s
		if format != "full" && format != "diff" {
			return nil, toolErr(`"format" must be "full" or "diff"`)
		}
	}
	n, err := t.msgHistVersions(ctx, g.src, out, r.i64("id"), format == "diff", r.str("text"))
	if err != nil {
		return nil, err
	}
	out["format"] = format
	out["previous_version_count"] = n
	// Say plainly when the archive knows a message changed but cannot
	// show how: silence would read as "nothing was edited".
	if edited && n == 0 {
		out["note"] = "This message is marked edited, " +
			"but no earlier version was " +
			"captured -- the edit happened " +
			"before the message was archived " +
			"or while the logger was not " +
			"running."
	}
	return out, nil
}

// msgHistVersions adds one message's earlier versions, oldest first, in
// full or as diffs between consecutive versions ending at the current
// text, and returns how many there are.
func (t *telegram) msgHistVersions(ctx context.Context, src *msgSource, out map[string]any, rowID int64,
	asDiff bool, current string) (int, error) {
	rows, err := execTool(ctx, t.db,
		"SELECT e.content_type, e.text, e.edit_date, "+
			"e.created_at, "+
			"f.id AS file_id, f.file_type, f.file_size, f.on_disk "+
			"FROM "+src.edits+" e "+
			"LEFT JOIN telegram_files f ON f.id = e.file_id "+
			"WHERE e."+src.editKey+" = ? ORDER BY e.id ASC",
		[]any{strconv.FormatInt(rowID, 10)}, queryFailed)
	if err != nil {
		return 0, err
	}
	type ver struct {
		meta map[string]any
		text string
	}
	vers := make([]ver, 0, len(rows))
	for _, e := range rows {
		v := ver{text: e.str("text"), meta: map[string]any{
			"content_type": e.str("content_type"),
			"observed_at":  e.str("created_at"),
		}}
		if d := e.i64("edit_date"); d != 0 {
			v.meta["edit_date"] = d
		}
		if !e.null("file_id") {
			v.meta["media"] = t.mediaJSON(e, false)
		}
		vers = append(vers, v)
	}
	if !asDiff {
		list := []any{}
		for _, v := range vers {
			v.meta["text"] = v.text
			list = append(list, v.meta)
		}
		out["previous_versions"] = list
		return len(list), nil
	}
	// One array: each entry carries the change and the metadata of the
	// version it produced, so nothing is stated twice.
	diffs := []any{}
	for i, v := range vers {
		d := map[string]any{"from_version": i}
		to := current
		if i+1 < len(vers) {
			next := vers[i+1]
			to = next.text
			d["to_version"] = i + 1
			if ed, ok := next.meta["edit_date"]; ok {
				d["edit_date"] = ed
			}
			d["observed_at"] = next.meta["observed_at"]
		} else {
			// The last diff lands on the live message.
			d["to_version"] = "current"
		}
		u := unifiedDiff(v.text, to)
		d["diff"] = u
		if u == "" {
			d["unchanged_text"] = true
		}
		diffs = append(diffs, d)
	}
	out["diffs"] = diffs
	return len(diffs), nil
}

func (t *telegram) getMessageHistoryTool() Tool {
	return Tool{
		Name:  "telegram_get_message_history",
		Title: "Get one message's edit and deletion history",
		Description: "What a single message used to say, and whether it was " +
			"deleted. Returns the current version, every earlier " +
			"version the archive captured (oldest first), and the " +
			"deletion record if there is one.\n\n" +
			"IMPORTANT -- edited and recoverable are not the same. " +
			"Telegram marks a message as edited, but an earlier " +
			"version exists only if the daemon was running and saw " +
			"the edit happen. A message edited before it was first " +
			"archived, or while the daemon was down, is flagged " +
			"is_edited with no previous version to show. The " +
			"response says so explicitly rather than looking " +
			"like nothing changed: check `previous_version_count` " +
			"against `is_edited`.\n\n" +
			"Deleted messages keep their content here: a deletion " +
			"records WHEN it was observed, it does not erase what " +
			"was said.\n\n" +
			"Identify the message by group_id plus message_id, as " +
			"returned by the message tools. Only readable groups.\n\n" +
			"FOR MORE THAN ONE MESSAGE, use telegram_get_messages with " +
			"include_history: true -- it returns the same information for " +
			"up to 100 messages in a single call. This tool is the " +
			"single-message form.\n\n" +
			"format: \"diff\" returns unified diffs between consecutive " +
			"versions rather than the full text of each, which is much " +
			"smaller when a long message got a small edit.",
		InputSchema: objectSchema(map[string]any{
			"group_id": propGroupID(),
			"message_id": map[string]any{
				"type":        "integer",
				"description": "The message's id within that group.",
			},
			"format": map[string]any{
				"type": "string",
				"enum": strList("full", "diff"),
				"description": "full (default) returns the complete text " +
					"of every version. diff returns a unified " +
					"diff between consecutive versions instead " +
					"-- far smaller when a long message was " +
					"edited slightly, and it shows what " +
					"actually changed.",
			},
		}, "group_id", "message_id"),
		Handler: t.getMessageHistory,
	}
}

// ---- telegram_get_user ----

func (t *telegram) getUser(ctx context.Context, args map[string]any) (any, error) {
	uid, err := requireInt(args, "user_id")
	if err != nil {
		return nil, err
	}
	bind := strconv.FormatInt(uid, 10)
	rows, err := execTool(ctx, t.db,
		"SELECT /*+ MAX_EXECUTION_TIME(5000) */ "+
			"u.id, u.first_name, u.last_name, u.type, "+
			"u.profile_photo_file_id, u.accent_color_id, "+
			"u.is_verified, u.is_scam, u.is_fake, "+
			"u.is_premium, u.is_support, "+
			"u.birthday_day, u.birthday_month, "+
			"u.birthday_year, u.msg_count, "+
			"u.created_at, u.updated_at, "+
			"e.bio, e.phone_number, e.language_code, "+
			"e.restriction_reason, e.has_sensitive_content, "+
			"e.restricts_new_chats, e.paid_message_star_count, "+
			"e.personal_chat_id, e.emoji_status_custom_emoji_id, "+
			"e.profile_accent_color_id, "+
			"pf.on_disk AS photo_on_disk "+
			"FROM telegram_users u "+
			"LEFT JOIN telegram_user_extra_info e "+
			"  ON e.user_id = u.id "+
			"LEFT JOIN telegram_files pf "+
			"  ON pf.id = u.profile_photo_file_id "+
			"WHERE u.id = ?",
		[]any{bind}, queryFailed)
	if err != nil {
		return nil, err
	}
	if len(rows) == 0 {
		return nil, toolErr("no user with id " + bind + " in the archive")
	}
	r := rows[0]
	typ := r.str("type")
	out := map[string]any{
		"user_id":       r.i64("id"),
		"first_name":    r.str("first_name"),
		"last_name":     r.str("last_name"),
		"type":          typ,
		"is_bot":        typ == "bot",
		"is_deleted":    typ == "deleted",
		"is_verified":   r.i64("is_verified") != 0,
		"is_premium":    r.i64("is_premium") != 0,
		"is_scam":       r.i64("is_scam") != 0,
		"is_fake":       r.i64("is_fake") != 0,
		"is_support":    r.i64("is_support") != 0,
		"message_count": r.i64("msg_count"),
		"first_seen":    r.str("created_at"),
		"last_updated":  r.str("updated_at"),
	}
	// Omitted when never seen: "no bio" and "never learned one" differ.
	for _, k := range []string{"bio", "phone_number", "language_code", "restriction_reason"} {
		if v := r.str(k); v != "" {
			out[k] = v
		}
	}
	if r.i64("has_sensitive_content") != 0 {
		out["has_sensitive_content"] = true
	}
	if r.i64("restricts_new_chats") != 0 {
		out["restricts_new_chats"] = true
	}
	if v := r.i64("paid_message_star_count"); v != 0 {
		out["paid_message_star_count"] = v
	}
	if v := r.i64("personal_chat_id"); v != 0 {
		out["personal_chat_id"] = v
	}
	if r.i64("emoji_status_custom_emoji_id") != 0 {
		out["has_emoji_status"] = true
	}
	// A birthday may come without a year: the parts as given.
	bd, bm, by := r.i64("birthday_day"), r.i64("birthday_month"), r.i64("birthday_year")
	if bd != 0 || bm != 0 || by != 0 {
		b := map[string]any{}
		if bd != 0 {
			b["day"] = bd
		}
		if bm != 0 {
			b["month"] = bm
		}
		if by != 0 {
			b["year"] = by
		}
		out["birthday"] = b
	}
	if photo := r.i64("profile_photo_file_id"); photo != 0 {
		out["profile_photo_file_id"] = photo
		if r.i64("photo_on_disk") != 0 {
			if u := t.fileURL(photo); u != "" {
				out["profile_photo_url"] = u
			}
		}
	}
	urows, err := execTool(ctx, t.db,
		"SELECT username, is_collectible "+
			"FROM telegram_user_usernames "+
			"WHERE user_id = ? AND kind = 'active' "+
			"ORDER BY position",
		[]any{bind}, "username lookup failed")
	if err != nil {
		return nil, err
	}
	names := []any{}
	for _, u := range urows {
		j := map[string]any{"username": u.str("username")}
		if u.i64("is_collectible") != 0 {
			j["is_collectible"] = true
		}
		names = append(names, j)
	}
	out["usernames"] = names
	return out, nil
}

func (t *telegram) getUserTool() Tool {
	return Tool{
		Name:  "telegram_get_user",
		Title: "Get one user's full profile",
		Description: "Everything the archive holds about one user, by id: " +
			"names, every active username, bio, phone number, " +
			"language, birthday, account flags and counts.\n\n" +
			"Use telegram_get_users to FIND a user by username, " +
			"name or phone; use this once you have the id and want " +
			"the whole record. Fields the archive never saw are " +
			"omitted rather than returned empty, so what comes back " +
			"is what is actually known.\n\n" +
			"Not restricted to exposed groups: this describes an " +
			"account, not a conversation.",
		InputSchema: objectSchema(map[string]any{
			"user_id": map[string]any{
				"type":        "integer",
				"description": "Telegram user id (positive).",
			},
		}, "user_id"),
		Handler: t.getUser,
	}
}

// ---- telegram_get_user_history ----

func (t *telegram) getUserHistory(ctx context.Context, args map[string]any) (any, error) {
	uid, err := requireInt(args, "user_id")
	if err != nil {
		return nil, err
	}
	bind := strconv.FormatInt(uid, 10)
	lim := strconv.Itoa(clampLimit(args))
	want, err := parseKinds(args, []string{"names", "usernames", "bios", "phone_numbers", "photos"})
	if err != nil {
		return nil, err
	}
	out := map[string]any{"user_id": uid}
	if want[0] {
		if err := t.histList(ctx, out, "names",
			"SELECT first_name, last_name, created_at FROM "+
				"telegram_user_hist_name WHERE user_id = ? "+
				"ORDER BY id DESC LIMIT "+lim,
			bind, [][2]string{{"first_name", "first_name"}, {"last_name", "last_name"}}); err != nil {
			return nil, err
		}
	}
	if want[1] {
		rows, err := execTool(ctx, t.db,
			"SELECT username, action, kind, is_collectible, "+
				"created_at FROM telegram_user_hist_usernames_events "+
				"WHERE user_id = ? ORDER BY id DESC LIMIT "+lim,
			[]any{bind}, queryFailed)
		if err != nil {
			return nil, err
		}
		list := []any{}
		for _, r := range rows {
			j := map[string]any{
				"username":    r.str("username"),
				"action":      r.str("action"),
				"kind":        r.str("kind"),
				"observed_at": r.str("created_at"),
			}
			if r.i64("is_collectible") != 0 {
				j["is_collectible"] = true
			}
			list = append(list, j)
		}
		out["usernames"] = list
	}
	if want[2] {
		if err := t.histList(ctx, out, "bios",
			"SELECT bio, created_at FROM telegram_user_hist_bio "+
				"WHERE user_id = ? ORDER BY id DESC LIMIT "+lim,
			bind, [][2]string{{"bio", "bio"}}); err != nil {
			return nil, err
		}
	}
	if want[3] {
		if err := t.histList(ctx, out, "phone_numbers",
			"SELECT phone_number, created_at "+
				"FROM telegram_user_hist_phone_num "+
				"WHERE user_id = ? ORDER BY id DESC LIMIT "+lim,
			bind, [][2]string{{"phone_number", "phone_number"}}); err != nil {
			return nil, err
		}
	}
	if want[4] {
		rows, err := execTool(ctx, t.db,
			"SELECT h.file_id, h.created_at, f.on_disk "+
				"FROM telegram_user_hist_profile_photo h "+
				"LEFT JOIN telegram_files f ON f.id = h.file_id "+
				"WHERE h.user_id = ? "+
				"ORDER BY h.id DESC LIMIT "+lim,
			[]any{bind}, queryFailed)
		if err != nil {
			return nil, err
		}
		out["photos"] = t.photoList(rows)
	}
	return out, nil
}

func (t *telegram) getUserHistoryTool() Tool {
	return Tool{
		Name:  "telegram_get_user_history",
		Title: "Get a user's profile change history",
		Description: "How a user's profile has changed over time: names, " +
			"usernames, bios, phone numbers and profile photos, " +
			"newest first.\n\n" +
			"The archive records a row when a change is OBSERVED, " +
			"so a timestamp is when the daemon noticed, not " +
			"necessarily when the user made the change, and a " +
			"change made and reverted between observations is " +
			"invisible. The oldest entry of each kind is usually " +
			"the value at first sight rather than a change.\n\n" +
			"Pass `kinds` to fetch only some categories; the " +
			"default is all of them.\n\n" +
			"Not restricted to exposed groups: this describes an " +
			"account, not a conversation.",
		InputSchema: objectSchema(map[string]any{
			"user_id": map[string]any{
				"type":        "integer",
				"description": "Telegram user id (positive).",
			},
			"kinds": map[string]any{
				"type":  "array",
				"items": map[string]any{"type": "string"},
				"description": "Any of: names, usernames, bios, " +
					"phone_numbers, photos. Default all.",
			},
			"limit": map[string]any{
				"type":        "integer",
				"description": "Entries per category, 1-200, default 50.",
			},
		}, "user_id"),
		Handler: t.getUserHistory,
	}
}

// ---- telegram_get_users ----

func (t *telegram) getUsers(ctx context.Context, args map[string]any) (any, error) {
	var c compiled
	if v, ok := args["filter"]; ok {
		var err error
		if c, err = compile(userSchema, v); err != nil {
			return nil, err
		}
	}
	if c.sql == "" {
		return nil, toolErr("a filter is required: give at least " +
			"one of user_id, username, " +
			"phone_number or a name")
	}
	offset, err := clampOffset(args)
	if err != nil {
		return nil, err
	}
	stmt := "SELECT /*+ MAX_EXECUTION_TIME(5000) */ " +
		"u.id, u.first_name, u.last_name, u.type, " +
		"u.msg_count, u.is_verified, u.is_premium, " +
		"u.is_scam, u.is_fake, u.created_at, " +
		"COALESCE(e.bio,'') AS bio, " +
		"COALESCE(e.phone_number,'') AS phone_number, " +
		"COALESCE(e.language_code,'') AS language_code, " +
		"un.username " +
		"FROM telegram_users u " +
		"LEFT JOIN telegram_user_extra_info e " +
		"  ON e.user_id = u.id " +
		"LEFT JOIN telegram_user_usernames un " +
		"  ON un.user_id = u.id AND un.kind = 'active' " +
		"  AND un.position = 0 " +
		"WHERE " + c.sql +
		" ORDER BY u.msg_count DESC LIMIT " +
		strconv.Itoa(clampLimit(args)) + " OFFSET " +
		strconv.Itoa(offset)
	rows, err := execTool(ctx, t.db, stmt, c.binds, queryFailed)
	if err != nil {
		return nil, err
	}
	users := []any{}
	for _, r := range rows {
		users = append(users, map[string]any{
			"user_id":       r.i64("id"),
			"first_name":    r.str("first_name"),
			"last_name":     r.str("last_name"),
			"username":      r.str("username"),
			"type":          r.str("type"),
			"bio":           r.str("bio"),
			"phone_number":  r.str("phone_number"),
			"language_code": r.str("language_code"),
			"message_count": r.i64("msg_count"),
			"is_verified":   r.i64("is_verified") != 0,
			"is_premium":    r.i64("is_premium") != 0,
			"is_scam":       r.i64("is_scam") != 0,
			"is_fake":       r.i64("is_fake") != 0,
			"first_seen":    r.str("created_at"),
		})
	}
	return map[string]any{"users": users, "count": len(users)}, nil
}

func (t *telegram) getUsersTool() Tool {
	return Tool{
		Name:  "telegram_get_users",
		Title: "Look up Telegram users",
		Description: "Find users by id, username, phone number or name, " +
			"using the same filter tree as message search.\n\n" +
			"Fields:\n" +
			describeFields(userSchema) +
			"\nNote names are not indexed, so a name search scans " +
			"the user table -- prefer user_id or username when you " +
			"have one.\n\n" +
			"BATCH LOOKUP. To resolve many users at once, use the \"in\" " +
			"operator on user_id -- it is the primary key, so up to 100 " +
			"ids resolve in a single indexed call:\n" +
			"{\"field\":\"user_id\",\"op\":\"in\",\"value\":" +
			"[123,456,789]}\n" +
			"Do not loop one call per user.\n\n" +
			"This tool is not restricted to readable groups: it " +
			"describes accounts, not conversations.",
		InputSchema: objectSchema(map[string]any{
			"filter": map[string]any{
				"type":        "object",
				"description": "Filter tree; same grammar as telegram_search_messages.",
			},
			"limit":  propLimit(""),
			"offset": map[string]any{"type": "integer"},
		}),
		Handler: t.getUsers,
	}
}
