// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"cmp"
	"context"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net/url"
	"regexp"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"tgloggerd/webapi/internal/logdb"
	"tgloggerd/webapi/internal/scope"
)

// The list pages (users, groups, files, group_messages,
// private_messages): their loaders read the page's list state from
// the query string (q, f, sort, dir, page, per, as web/app's
// useListNav writes it), run it through the advanced search
// (logdb.Search) within the request scope, and answer the rows in the
// shapes of the React pages (UsersProps, GroupsProps, FilesProps,
// MessagesProps in webapi/openapi.yaml).

// listCond is a filter of a list page (Cond). N is the connector to
// the next filter, AND (the default) or OR, as the search's n.
type listCond struct {
	Field string    `json:"field"`
	Op    string    `json:"op"`
	Value condValue `json:"value"`
	N     string    `json:"n,omitempty"`
}

// condValue is a filter's value: a string, or for the operator "in" a
// list of strings (List).
type condValue struct {
	S    string
	L    []string
	List bool
}

// strValue is a single value.
func strValue(s string) condValue { return condValue{S: s} }

func (v condValue) MarshalJSON() ([]byte, error) {
	if v.List {
		if v.L == nil {
			return []byte("[]"), nil
		}
		return json.Marshal(v.L)
	}
	return json.Marshal(v.S)
}

// UnmarshalJSON takes a string or a list of strings; null is "", as
// for a string field.
func (v *condValue) UnmarshalJSON(b []byte) error {
	*v = condValue{}
	if len(b) > 0 && b[0] == '[' {
		v.List = true
		return json.Unmarshal(b, &v.L)
	}
	return json.Unmarshal(b, &v.S)
}

// tooLong is whether the value, or one of the list's, is longer than
// the search takes.
func (v condValue) tooLong() bool {
	if len(v.S) > logdb.MaxValueLen {
		return true
	}
	for _, s := range v.L {
		if len(s) > logdb.MaxValueLen {
			return true
		}
	}
	return false
}

// listError is why a list shows its unfiltered rows (ListError): the
// filters or the search text were refused, or the search ran out of
// time.
type listError struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

// listState is the list state the rows were read with (ListState).
type listState struct {
	Total int64      `json:"total"`
	Page  int        `json:"page"`
	Pages int        `json:"pages"`
	Per   int        `json:"per"`
	Sort  string     `json:"sort"`
	Dir   string     `json:"dir"`
	Q     string     `json:"q"`
	Conds []listCond `json:"conds"`
	// ArchiveTotal is everything in the archive, for viewers who see
	// all of it only.
	ArchiveTotal *int64 `json:"archive_total,omitempty"`
	// TotalExact is false when Total is an estimate (a count that ran
	// out of time, or a table's row estimate).
	TotalExact bool `json:"total_exact"`
	// Error, when set, says why the rows are the whole list instead
	// of the search asked for.
	Error *listError `json:"error,omitempty"`
	// Fields are the search fields the viewer may filter by, with
	// their operators, for the lists that take every field of the
	// search (listSpec.registry).
	Fields []logdb.FieldInfo `json:"fields,omitempty"`
}

// maxListPer is the most rows a list page shows, the search's own
// limit (Drogon's lists took any page size up to it).
const maxListPer = logdb.MaxLimit

// relevanceSort is the sort a message list reports when its search
// text orders it by relevance.
const relevanceSort = "relevance"

// pageFilter turns one filter of a page into search conditions; ok is
// false for one the field does not take, which is dropped.
type pageFilter func(sc scope.Scope, op, value string) ([]logdb.Cond, bool)

// pageListFilter is a page filter of a list value (the operator in).
type pageListFilter func(sc scope.Scope, op string, values []string) ([]logdb.Cond, bool)

// listSpec is one list page.
type listSpec struct {
	entity     *logdb.Entity
	defSort    func(sc scope.Scope) string
	defPer     int
	sorts      map[string]string // the page's sort keys: field keys
	filters    map[string]pageFilter
	lists      map[string]pageListFilter // the filters of list values
	search     func(sc scope.Scope, q string) []logdb.Cond
	limitedUse map[string]bool // filters and sorts of a viewer who sees in part
	// relevance: the search text is a fulltext match, which orders
	// the rows by relevance unless a sort is asked for.
	relevance bool
	// registry: besides the page's own filters and sorts, the list
	// takes every field of the entity's search registry with the
	// search's operators, and its sortable fields, as the Drogon list
	// pages did; the scope narrows them as it does for the API.
	registry bool
}

var dateRE = regexp.MustCompile(`^\d{4}-\d{2}-\d{2}$`)

func cond(c, o, v string) logdb.Cond {
	return logdb.Cond{C: c, O: o, V: v, N: "AND", HasV: true}
}

func isInt(v string) bool {
	_, err := strconv.ParseInt(v, 10, 64)
	return err == nil
}

func numberFilter(key string) pageFilter {
	return func(_ scope.Scope, op, v string) ([]logdb.Cond, bool) {
		switch op {
		case "=", "!=", ">", "<", ">=", "<=":
			if isInt(v) {
				return []logdb.Cond{cond(key, op, v)}, true
			}
		}
		return nil, false
	}
}

// textFilter is a text field: contains, equals, or empty.
func textFilter(key string) pageFilter {
	return func(_ scope.Scope, op, v string) ([]logdb.Cond, bool) {
		switch op {
		case "contains":
			return []logdb.Cond{cond(key, "%LIKE%", v)}, true
		case "=":
			return []logdb.Cond{cond(key, "=", v)}, true
		case "empty":
			return []logdb.Cond{cond(key, "=", "")}, true
		}
		return nil, false
	}
}

// existsTextFilter is a text field held in another table (a current
// username): empty is none at all.
func existsTextFilter(key string) pageFilter {
	return func(_ scope.Scope, op, v string) ([]logdb.Cond, bool) {
		switch op {
		case "contains":
			return []logdb.Cond{cond(key, "%LIKE%", v)}, true
		case "=":
			return []logdb.Cond{cond(key, "=", v)}, true
		case "empty":
			return []logdb.Cond{cond(key, "NOT LIKE", "%")}, true
		}
		return nil, false
	}
}

func enumFilter(key, values string) pageFilter {
	return func(_ scope.Scope, op, v string) ([]logdb.Cond, bool) {
		if (op == "=" || op == "!=") && strings.Contains(","+values+",", ","+v+",") {
			return []logdb.Cond{cond(key, op, v)}, true
		}
		return nil, false
	}
}

// enumInFilter is an enum field's "in": one of a list of its values.
func enumInFilter(key, values string) pageListFilter {
	return func(_ scope.Scope, op string, vs []string) ([]logdb.Cond, bool) {
		if op != "in" || len(vs) == 0 || len(vs) > logdb.MaxInValues {
			return nil, false
		}
		for _, v := range vs {
			if !strings.Contains(","+values+",", ","+v+",") {
				return nil, false
			}
		}
		return []logdb.Cond{{C: key, O: "in", L: vs, IsList: true, HasV: true, N: "AND"}}, true
	}
}

func yesNo(v string) (bool, bool) {
	switch v {
	case "yes", "true", "1":
		return true, true
	case "no", "false", "0":
		return false, true
	}
	return false, false
}

func boolFilter(key string) pageFilter {
	return func(_ scope.Scope, op, v string) ([]logdb.Cond, bool) {
		b, ok := yesNo(v)
		if op != "is" || !ok {
			return nil, false
		}
		if b {
			return []logdb.Cond{cond(key, "=", "1")}, true
		}
		return []logdb.Cond{cond(key, "=", "0")}, true
	}
}

// nullFilter is a flag held as a nullable column (deleted_at).
func nullFilter(key string) pageFilter {
	return func(_ scope.Scope, op, v string) ([]logdb.Cond, bool) {
		b, ok := yesNo(v)
		if op != "is" || !ok {
			return nil, false
		}
		o := "IS NULL"
		if b {
			o = "IS NOT NULL"
		}
		return []logdb.Cond{{C: key, O: o, N: "AND"}}, true
	}
}

// orderingOp is a comparison a day can take.
func orderingOp(op string) bool {
	return op == ">" || op == "<" || op == ">=" || op == "<="
}

// dateFilter compares a DATETIME column with a day, YYYY-MM-DD.
func dateFilter(key string) pageFilter {
	return func(_ scope.Scope, op, v string) ([]logdb.Cond, bool) {
		if orderingOp(op) && dateRE.MatchString(v) {
			if _, err := time.Parse(time.DateOnly, v); err == nil {
				return []logdb.Cond{cond(key, op, v)}, true
			}
		}
		return nil, false
	}
}

// unixDateFilter compares a unix time column with the start of a day
// (UTC), YYYY-MM-DD, as dateFilter does a DATETIME one.
func unixDateFilter(key string) pageFilter {
	return func(_ scope.Scope, op, v string) ([]logdb.Cond, bool) {
		if orderingOp(op) && dateRE.MatchString(v) {
			if t, err := time.Parse(time.DateOnly, v); err == nil {
				return []logdb.Cond{cond(key, op, strconv.FormatInt(t.Unix(), 10))}, true
			}
		}
		return nil, false
	}
}

// fulltextFilter is message text: a word search over the fulltext
// index for contains and equals.
func fulltextFilter(key string) pageFilter {
	return func(_ scope.Scope, op, v string) ([]logdb.Cond, bool) {
		if (op == "contains" || op == "=") && strings.TrimSpace(v) != "" {
			return []logdb.Cond{cond(key, "matches", v)}, true
		}
		return nil, false
	}
}

// sha256Filter is a SHA-256 digest, equal or not, as 64 hex digits of
// either case; the search matches it through the unique index.
func sha256Filter(key string) pageFilter {
	return func(_ scope.Scope, op, v string) ([]logdb.Cond, bool) {
		if (op == "=" || op == "!=") && len(v) == 64 {
			if _, err := hex.DecodeString(v); err == nil {
				return []logdb.Cond{cond(key, op, v)}, true
			}
		}
		return nil, false
	}
}

// or joins conditions into one chain of alternatives.
func or(cs ...logdb.Cond) []logdb.Cond {
	for i := range cs {
		cs[i].N = "OR"
	}
	return cs
}

// nameSearch matches a user's name: own or contact for a viewer who
// sees users in full, only the own one otherwise.
func nameKey(sc scope.Scope) string {
	if logdb.Users.Full(sc) {
		return "any_name"
	}
	return "full_name"
}

var usersList = &listSpec{
	registry: true,
	entity:   logdb.Users,
	defSort: func(sc scope.Scope) string {
		if logdb.Users.Full(sc) {
			return "msg_count"
		}
		return "name"
	},
	defPer: 25,
	sorts: map[string]string{"name": "first_name", "id": "id", "type": "type",
		"msg_count": "msg_count", "language": "lang", "created_at": "created_at"},
	filters: map[string]pageFilter{
		"id": numberFilter("id"),
		"name": func(sc scope.Scope, op, v string) ([]logdb.Cond, bool) {
			if op == "contains" {
				return []logdb.Cond{cond(nameKey(sc), "%LIKE%", v)}, true
			}
			return textFilter("full_name")(sc, op, v)
		},
		"username":    existsTextFilter("username"),
		"type":        enumFilter("type", "regular,deleted,bot,unknown"),
		"msg_count":   numberFilter("msg_count"),
		"is_premium":  boolFilter("is_premium"),
		"is_verified": boolFilter("is_verified"),
		"is_scam":     boolFilter("is_scam"),
		"is_fake":     boolFilter("is_fake"),
		"language":    textFilter("lang"),
		"bio":         textFilter("bio"),
		"created_at":  dateFilter("created_at"),
	},
	search: func(sc scope.Scope, q string) []logdb.Cond {
		q = strings.TrimPrefix(q, "@")
		cs := []logdb.Cond{cond(nameKey(sc), "%LIKE%", q), cond("username", "%LIKE%", q)}
		if isInt(q) {
			cs = append(cs, cond("id", "=", q))
		}
		return or(cs...)
	},
	limitedUse: map[string]bool{"id": true, "name": true, "username": true, "type": true},
}

var groupsList = &listSpec{
	registry: true,
	entity:   logdb.Groups,
	defSort:  func(scope.Scope) string { return "msg_count" },
	defPer:   25,
	sorts: map[string]string{"title": "title", "id": "id", "type": "type",
		"msg_count": "msg_count", "admin_count": "admin_count", "created_at": "created_at"},
	filters: map[string]pageFilter{
		"id":          numberFilter("id"),
		"title":       textFilter("title"),
		"username":    existsTextFilter("username"),
		"type":        enumFilter("type", "basic_group,supergroup,channel"),
		"msg_count":   numberFilter("msg_count"),
		"description": textFilter("description"),
		"exposed":     boolFilter("exposed"),
		"created_at":  dateFilter("created_at"),
	},
	search: func(_ scope.Scope, q string) []logdb.Cond {
		q = strings.TrimPrefix(q, "@")
		cs := []logdb.Cond{cond("title", "%LIKE%", q), cond("username", "%LIKE%", q)}
		if isInt(q) {
			cs = append(cs, cond("id", "=", q))
		}
		return or(cs...)
	},
}

// fileTypes are the files list's types, the search's file_type values.
const fileTypes = "photo,video,document,audio,voice,sticker,animation,video_note,thumbnail,unknown"

var filesList = &listSpec{
	entity:  logdb.Files,
	defSort: func(scope.Scope) string { return "created_at" },
	defPer:  50,
	sorts: map[string]string{"id": "id", "name": "name", "file_type": "file_type",
		"ext": "ext", "size": "size", "hits": "hits", "created_at": "created_at",
		"accessed_at": "accessed_at"},
	filters: map[string]pageFilter{
		"id":          numberFilter("id"),
		"file_type":   enumFilter("file_type", fileTypes),
		"name":        textFilter("name"),
		"ext":         textFilter("ext"),
		"size":        numberFilter("size"),
		"hits":        numberFilter("hits"),
		"stored":      boolFilter("stored"),
		"tg_file_id":  textFilter("tg_file_id"),
		"sha256":      sha256Filter("sha256"),
		"created_at":  dateFilter("created_at"),
		"updated_at":  dateFilter("updated_at"),
		"accessed_at": dateFilter("accessed_at"),
		"role":        enumFilter("role", logdb.FileRoles),
	},
	lists: map[string]pageListFilter{"file_type": enumInFilter("file_type", fileTypes),
		"role": enumInFilter("role", logdb.FileRoles)},
	search: func(_ scope.Scope, q string) []logdb.Cond {
		cs := []logdb.Cond{cond("name", "%LIKE%", q)}
		if isInt(q) {
			cs = append(cs, cond("id", "=", q))
		}
		return or(cs...)
	},
}

func messageSearch(_ scope.Scope, q string) []logdb.Cond {
	return []logdb.Cond{cond("text", "matches", q)}
}

var groupMessagesList = &listSpec{
	registry:  true,
	entity:    logdb.GroupMessages,
	defSort:   func(scope.Scope) string { return "date" },
	defPer:    50,
	sorts:     map[string]string{"date": "date"},
	relevance: true,
	filters: map[string]pageFilter{
		"text":         fulltextFilter("text"),
		"content_type": enumFilter("content_type", logdb.MessageTypes),
		"chat_id":      numberFilter("chat_id"),
		"sender_id":    numberFilter("sender_user_id"),
		"is_forwarded": boolFilter("is_forwarded"),
		"is_edited":    boolFilter("is_edited"),
		"is_deleted":   nullFilter("deleted"),
		"date":         unixDateFilter("date"),
	},
	search: messageSearch,
}

var privateMessagesList = &listSpec{
	registry:  true,
	entity:    logdb.PrivateMessages,
	defSort:   func(scope.Scope) string { return "date" },
	defPer:    50,
	sorts:     map[string]string{"date": "date"},
	relevance: true,
	filters: map[string]pageFilter{
		"text":         fulltextFilter("text"),
		"content_type": enumFilter("content_type", logdb.MessageTypes),
		"chat_id":      numberFilter("chat_id"),
		"is_outgoing":  boolFilter("is_outgoing"),
		"is_forwarded": boolFilter("is_forwarded"),
		"is_edited":    boolFilter("is_edited"),
		"is_deleted":   nullFilter("deleted"),
		"date":         unixDateFilter("date"),
	},
	search: messageSearch,
}

// allowed is whether a viewer may filter or sort the list by key: a
// viewer who sees the rows in part only by the keys it shows.
func (l *listSpec) allowed(sc scope.Scope, key string) bool {
	return l.limitedUse == nil || l.entity.Full(sc) || l.limitedUse[key]
}

// listRun is a list page's search, as the loaders get it back.
type listRun struct {
	state listState
	res   *logdb.SearchResult
}

// refusal is a search the list refuses, in the search's words.
func refusal(format string, a ...any) error {
	return &logdb.SearchError{Msg: fmt.Sprintf(format, a...)}
}

// condsOf turns the page's filters into search conditions: a filter
// the page knows by its own key and operator (listSpec.filters), else,
// for a registry list, a field of the search registry with one of the
// search's operators, which the search checks. Anything else is
// refused, as the Drogon lists refuse it; a list without the registry
// drops it instead. kept are the filters applied.
func (l *listSpec) condsOf(sc scope.Scope, raw []listCond) (out []logdb.Cond, kept []listCond, err error) {
	out, kept = make([]logdb.Cond, 0, len(raw)), []listCond{}
	for _, c := range raw {
		n := c.N
		if n == "" {
			n = "AND"
		}
		if !l.registry {
			n = "AND"
		}
		if n != "AND" && n != "OR" {
			return nil, nil, refusal("invalid connector: '%s'", c.N)
		}
		if c.Value.tooLong() {
			if !l.registry {
				continue
			}
			return nil, nil, refusal("value too long (max %d)", logdb.MaxValueLen)
		}
		var lc logdb.Cond
		ok := false
		// Every page filter is one condition, so the connectors keep
		// their meaning.
		if pf := l.filters[c.Field]; pf != nil && !c.Value.List && l.allowed(sc, c.Field) {
			var cs []logdb.Cond
			if cs, ok = pf(sc, c.Op, c.Value.S); ok && len(cs) == 1 {
				lc = cs[0]
			} else {
				ok = false
			}
		}
		if pf := l.lists[c.Field]; pf != nil && c.Value.List && l.allowed(sc, c.Field) {
			var cs []logdb.Cond
			if cs, ok = pf(sc, c.Op, c.Value.L); ok && len(cs) == 1 {
				lc = cs[0]
			} else {
				ok = false
			}
		}
		if !ok && l.registry && logdb.IsOperator(c.Op) {
			lc = logdb.Cond{C: c.Field, O: c.Op, V: c.Value.S, L: c.Value.L,
				IsList: c.Value.List, HasV: logdb.TakesValue(c.Op)}
			ok = true
		}
		if !ok && !l.registry {
			continue
		}
		if !ok {
			return nil, nil, refusal("operator '%s' not allowed for field '%s'", c.Op, c.Field)
		}
		if !l.registry && len(out) >= logdb.MaxConds/2 {
			continue
		}
		lc.N = n
		out = append(out, lc)
		if n == "AND" {
			c.N = ""
		}
		kept = append(kept, c)
	}
	return out, kept, nil
}

// legacyQuery turns the query of a Drogon list URL (search, offset,
// limit, sort, order: web/static/search.js) into this list's (f, page,
// per, sort, dir), so bookmarks and links keep working; nil when the
// query is not one, or mixes in this list's own parameters.
func legacyQuery(q url.Values) (url.Values, error) {
	if !q.Has("search") && !q.Has("offset") && !q.Has("limit") && !q.Has("order") {
		return nil, nil
	}
	for _, k := range []string{"f", "page", "per", "dir"} {
		if q.Has(k) {
			return nil, nil
		}
	}
	out := url.Values{}
	if v := q.Get("q"); v != "" {
		out.Set("q", v)
	}
	conds, err := logdb.ParseConditions(q.Get("search"))
	if err != nil {
		return nil, err
	}
	if len(conds) > 0 {
		lc := make([]listCond, 0, len(conds))
		for i, c := range conds {
			n := c.N
			if i == len(conds)-1 || n == "AND" {
				// The last connector means nothing; AND is the
				// default.
				n = ""
			}
			lc = append(lc, listCond{Field: c.C, Op: c.O, N: n,
				Value: condValue{S: c.V, L: c.L, List: c.IsList}})
		}
		b, _ := json.Marshal(lc)
		out.Set("f", string(b))
	}
	per := 0
	if q.Has("limit") {
		per = min(max(atoi(q.Get("limit")), 1), maxListPer)
		out.Set("per", strconv.Itoa(per))
	}
	if off := min(max(atoi(q.Get("offset")), 0), logdb.MaxOffset); off > 0 {
		if per == 0 {
			// Drogon's lists showed 50 rows a page by default.
			per = 50
			out.Set("per", "50")
		}
		if page := off/per + 1; page > 1 {
			out.Set("page", strconv.Itoa(page))
		}
	}
	if v := q.Get("sort"); v != "" {
		out.Set("sort", v)
	}
	if strings.EqualFold(q.Get("order"), "asc") {
		out.Set("dir", "asc")
	} else if q.Has("order") {
		out.Set("dir", "desc")
	}
	return out, nil
}

// run reads the list state from the query, searches, and fills the
// state in. A filter or search text the list refuses, or a search that
// runs out of time, shows the whole list instead, with the reason in
// the state's error, as the Drogon pages do.
func (l *listSpec) run(ctx context.Context, pr *pageRequest) (*listRun, error) {
	s, sc, q := pr.s, pr.scope, pr.query
	var failed error
	if lq, err := legacyQuery(q); err != nil {
		failed = err
	} else if lq != nil {
		// The page's URL becomes the translated one.
		q, pr.query, pr.rewritten = lq, lq, true
	}
	defPer := l.defPer
	if pr.rowsPerPage > 0 {
		defPer = pr.rowsPerPage
	}
	st := listState{Q: strings.TrimSpace(q.Get("q")), Conds: []listCond{}, Page: 1, Per: defPer}
	if n, err := strconv.Atoi(q.Get("per")); err == nil && n > 0 {
		st.Per = min(n, maxListPer)
	}
	if n, err := strconv.Atoi(q.Get("page")); err == nil && n > 1 {
		// No deeper than the search's offsets go.
		st.Page = min(n, logdb.MaxOffset/st.Per+1)
	}
	if len(st.Q) > logdb.MaxValueLen {
		if st.Q = ""; l.registry {
			failed = refusal("value too long (max %d)", logdb.MaxValueLen)
		}
	}

	var conds []logdb.Cond
	if f := q.Get("f"); f != "" && failed == nil {
		var raw []listCond
		if len(f) > logdb.MaxSearchBytes {
			if l.registry {
				failed = refusal("search parameter too large")
			}
		} else if err := json.Unmarshal([]byte(f), &raw); err != nil {
			if l.registry {
				failed = refusal("f must be a JSON array of conditions")
			}
		} else {
			conds, st.Conds, failed = l.condsOf(sc, raw)
			if st.Conds == nil {
				st.Conds = []listCond{}
			}
		}
	}

	st.Sort, st.Dir = q.Get("sort"), q.Get("dir")
	sortKey, known := l.sorts[st.Sort]
	switch {
	case known && l.allowed(sc, st.Sort):
	case st.Sort != "" && l.registry && l.entity.Sortable(sc, st.Sort):
		sortKey = st.Sort
	default:
		st.Sort, sortKey = "", ""
	}
	if st.Dir != "asc" {
		st.Dir = "desc"
	}
	explicit := st.Sort != ""
	if !explicit {
		st.Sort = l.defSort(sc)
		sortKey = l.sorts[st.Sort]
	}

	req := logdb.SearchRequest{Conds: conds, Order: st.Dir, Internal: true, Limit: st.Per}
	// The search engine answers the search text, when there is one
	// (searchengine.go); the list's answer keeps its shape. The message
	// lists take the search API's options for it from the page's query
	// (fuzzy, highlight, facets, engine=mysql) and answer its backend,
	// highlights and facets (messagesProps).
	switch {
	case s.engine != nil && (l.relevance || logdb.EngineEntity(l.entity)):
		s.withEngineOptions(q, &req)
	case s.engine != nil:
		req.Engine = s.engine
	}
	if st.Q != "" && failed == nil {
		req.And = [][]logdb.Cond{l.search(sc, st.Q)}
	}
	// A search text over messages orders by relevance unless a sort
	// was asked for.
	if explicit || len(req.And) == 0 || !l.relevance {
		req.Sort = sortKey
	} else {
		st.Sort = relevanceSort
	}
	if l.registry {
		st.Fields = l.entity.FieldInfos(sc)
	}
	// fallBack drops the search and shows the whole list, with err as
	// the reason; a plain list whose asked-for sort ran out of time
	// falls back to the default sort. False when nothing is left to
	// drop.
	fallBack := func(err error) bool {
		switch {
		case len(req.Conds) > 0 || len(req.And) > 0:
			req.Conds, req.And, st.Conds, st.Q = nil, nil, []listCond{}, ""
			if st.Sort == relevanceSort {
				st.Sort = l.defSort(sc)
				req.Sort = l.sorts[st.Sort]
			}
		case explicit && errors.Is(err, logdb.ErrSearchTimeout):
			explicit = false
			st.Sort = l.defSort(sc)
			req.Sort = l.sorts[st.Sort]
		default:
			return false
		}
		st.Error = listErrorOf(err)
		return true
	}
	if failed != nil {
		st.Q, st.Error = "", listErrorOf(failed)
	}

	clamped := false
	for {
		req.Offset = (st.Page - 1) * st.Per
		res, err := s.search(ctx, s.ro, sc, l.entity, req)
		var se *logdb.SearchError
		if errors.As(err, &se) && (len(req.Conds) > 0 || len(req.And) > 0) {
			fallBack(err)
			continue
		}
		if errors.Is(err, logdb.ErrNotFound) {
			return nil, errPageNotFound
		}
		if errors.Is(err, logdb.ErrSearchTimeout) {
			if fallBack(err) {
				continue
			}
			// Even the plain list ran out of time: no rows, and the
			// reason.
			st.Pages, st.Total, st.TotalExact = 1, 0, false
			st.Error = listErrorOf(err)
			return &listRun{state: st, res: &logdb.SearchResult{Entity: l.entity}}, nil
		}
		if err != nil {
			return nil, err
		}
		st.Total, st.TotalExact = res.Total, res.TotalExact
		st.Pages = max(1, int((res.Total+int64(st.Per)-1)/int64(st.Per)))
		maxPage := logdb.MaxOffset/st.Per + 1
		st.Pages = min(st.Pages, maxPage)
		if st.Page > st.Pages && !clamped && res.TotalExact {
			st.Page, clamped = st.Pages, true
			continue
		}
		run := &listRun{state: st, res: res}
		if n, ok, err := logdb.ArchiveTotal(ctx, s.ro, sc, l.entity); err != nil {
			return nil, err
		} else if ok {
			run.state.ArchiveTotal = &n
		}
		return run, nil
	}
}

// listErrorOf is the state's error for a refused search or one that
// ran out of time.
func listErrorOf(err error) *listError {
	if errors.Is(err, logdb.ErrSearchTimeout) {
		return &listError{Code: "search_timeout", Message: err.Error()}
	}
	return &listError{Code: "bad_search", Message: err.Error()}
}

// dbTime is a DATETIME as the pages take it, RFC 3339 in UTC (the
// logger's database keeps UTC).
func dbTime(s string) string {
	t, err := time.Parse(time.DateTime, s)
	if err != nil {
		return s
	}
	return t.UTC().Format(time.RFC3339)
}

func dbTimePtr(r logdb.Row, c string) *string {
	if r.Null(c) {
		return nil
	}
	t := dbTime(r.Str(c))
	return &t
}

func unixTime(n int64) string {
	return time.Unix(n, 0).UTC().Format(time.RFC3339)
}

func (s *Server) photoPtr(id uint64) *string {
	if id == 0 {
		return nil
	}
	u := s.photoURL(id)
	return &u
}

// storedPhoto is a picture's URL (photoPtr) when its bytes are in the
// store, else nil: for the answers that show a photo only when it can
// be served at once.
func (s *Server) storedPhoto(f logdb.FileRef) *string {
	if f.ID == 0 || !f.Exists || !f.OnDisk || s.tokens == nil {
		return nil
	}
	return s.photoPtr(f.ID)
}

func ptrTo[T any](v T) *T { return &v }

// listUser is a user as lists show one (UserSummary). Only id,
// name, username, type and photo are there for a viewer who sees users
// in part.
type listUser struct {
	ID          int64   `json:"id"`
	Name        string  `json:"name"`
	Username    string  `json:"username"`
	Type        string  `json:"type"`
	Photo       *string `json:"photo"`
	ContactName *string `json:"contact_name,omitempty"`
	MsgCount    *int64  `json:"msg_count,omitempty"`
	IsVerified  *bool   `json:"is_verified,omitempty"`
	IsPremium   *bool   `json:"is_premium,omitempty"`
	IsScam      *bool   `json:"is_scam,omitempty"`
	IsFake      *bool   `json:"is_fake,omitempty"`
	IsSupport   *bool   `json:"is_support,omitempty"`
	Language    *string `json:"language,omitempty"`
	CreatedAt   *string `json:"created_at,omitempty"`
	UpdatedAt   *string `json:"updated_at,omitempty"`
}

// userRow is a row of the users page (UserRow): the summary and, for
// a viewer who sees users in full, the Drogon list's other columns.
type userRow struct {
	listUser
	Phone             *string `json:"phone,omitempty"`
	Bio               *string `json:"bio,omitempty"`
	RestrictionReason *string `json:"restriction_reason,omitempty"`
	HasSensitive      *bool   `json:"has_sensitive_content,omitempty"`
	RestrictsNewChats *bool   `json:"restricts_new_chats,omitempty"`
	PaidMessageStars  *int64  `json:"paid_message_star_count,omitempty"`
	// Pointers to pointers: null is "none", absent is "not shown".
	PersonalChatID **int64  `json:"personal_chat_id,omitempty"`
	EmojiStatus    **string `json:"emoji_status,omitempty"`
}

// usersProps are the users page's (UsersProps).
type usersProps struct {
	listState
	Rows []userRow `json:"rows"`
	profileEngine
}

// profileEngine is the search engine's part of the users' and groups'
// pages, as messagesProps' without facets: only with an engine
// configured, for a bio or description "matches" filter.
type profileEngine struct {
	Backend    *logdb.Backend `json:"backend,omitempty"`
	Highlights [][]string     `json:"highlights,omitempty"`
	Engine     *engineCaps    `json:"engine,omitempty"`
}

func (s *Server) profileEngineOf(ctx context.Context, sc scope.Scope, l *listSpec,
	run *listRun) profileEngine {
	return profileEngine{Backend: run.res.Backend, Highlights: run.res.Highlights,
		Engine: s.listEngineCaps(ctx, sc, l.entity)}
}

func loadUsers(ctx context.Context, pr *pageRequest) (any, error) {
	s := pr.s
	run, err := usersList.run(ctx, pr)
	if err != nil {
		return nil, err
	}
	full := logdb.Users.Full(pr.scope)
	p := usersProps{listState: run.state, Rows: []userRow{},
		profileEngine: s.profileEngineOf(ctx, pr.scope, usersList, run)}
	for _, r := range run.res.Rows {
		u := listUser{ID: r.Int("id"),
			Name:     logdb.DisplayName(r.Str("first_name"), r.Str("last_name")),
			Username: r.Str("username"), Type: r.Str("type"),
			Photo: s.photoPtr(r.Uint("profile_photo_file_id"))}
		if full {
			u.ContactName = ptrTo(logdb.ContactName(r))
			u.MsgCount = ptrTo(r.Int("msg_count"))
			u.IsVerified, u.IsPremium = ptrTo(r.Bool("is_verified")), ptrTo(r.Bool("is_premium"))
			u.IsScam, u.IsFake = ptrTo(r.Bool("is_scam")), ptrTo(r.Bool("is_fake"))
			u.IsSupport = ptrTo(r.Bool("is_support"))
			u.Language = ptrTo(r.Str("language"))
			u.CreatedAt, u.UpdatedAt = dbTimePtr(r, "created_at"), dbTimePtr(r, "updated_at")
		}
		row := userRow{listUser: u}
		if full {
			row.Phone, row.Bio = ptrTo(r.Str("phone")), ptrTo(r.Str("bio"))
			row.RestrictionReason = ptrTo(r.Str("restriction_reason"))
			row.HasSensitive = ptrTo(r.Bool("has_sensitive"))
			row.RestrictsNewChats = ptrTo(r.Bool("restricts_new_chats"))
			row.PaidMessageStars = ptrTo(r.Int("paid_star_count"))
			var pc *int64
			if !r.Null("personal_chat_id") {
				pc = ptrTo(r.Int("personal_chat_id"))
			}
			row.PersonalChatID = &pc
			var es *string
			if !r.Null("emoji_status") {
				// A 64-bit custom emoji id: a string.
				es = ptrTo(r.Str("emoji_status"))
			}
			row.EmojiStatus = &es
		}
		p.Rows = append(p.Rows, row)
	}
	return p, nil
}

// listGroup is a row of the groups page.
type listGroup struct {
	ID          int64   `json:"id"`
	Title       string  `json:"title"`
	Username    string  `json:"username"`
	Type        string  `json:"type"`
	Photo       *string `json:"photo"`
	MsgCount    int64   `json:"msg_count"`
	Exposed     bool    `json:"exposed"`
	AdminCount  int64   `json:"admin_count"`
	Description string  `json:"description"`
	CreatedAt   string  `json:"created_at"`
	UpdatedAt   string  `json:"updated_at"`
	// As GroupSummary's: Telegram's count, null before the daemon has
	// stored one.
	MemberCount   *int64  `json:"member_count"`
	MemberCountAt *string `json:"member_count_at"`
}

// groupsProps are the groups page's (GroupsProps).
type groupsProps struct {
	listState
	Rows []listGroup `json:"rows"`
	profileEngine
}

func loadGroups(ctx context.Context, pr *pageRequest) (any, error) {
	s := pr.s
	run, err := groupsList.run(ctx, pr)
	if err != nil {
		return nil, err
	}
	p := groupsProps{listState: run.state, Rows: []listGroup{},
		profileEngine: s.profileEngineOf(ctx, pr.scope, groupsList, run)}
	for _, r := range run.res.Rows {
		p.Rows = append(p.Rows, listGroup{ID: r.Int("id"),
			Title: logdb.GroupTitle(r.Str("title")), Username: r.Str("username"),
			Type: r.Str("type"), Photo: s.photoPtr(r.Uint("photo_file_id")),
			MsgCount: r.Int("msg_count"), Exposed: r.Bool("exposed"),
			AdminCount: r.Int("admin_count"), Description: r.Str("description"),
			CreatedAt: dbTime(r.Str("created_at")), UpdatedAt: dbTime(r.Str("updated_at"))})
		if !r.Null("member_count") {
			n := r.Int("member_count")
			p.Rows[len(p.Rows)-1].MemberCount = &n
			p.Rows[len(p.Rows)-1].MemberCountAt = dbTimePtr(r, "member_count_at")
		}
	}
	return p, nil
}

type listChatName struct {
	ID    int64  `json:"id"`
	Title string `json:"title"`
	Kind  string `json:"kind"`
}

type listIDName struct {
	ID   int64  `json:"id"`
	Name string `json:"name"`
}

type listMsgRef struct {
	Scope string `json:"scope"`
	ID    uint64 `json:"id"`
}

// listFile is a row of the files page (FileRow).
type listFile struct {
	ID         uint64        `json:"id"`
	FileType   string        `json:"file_type"`
	Name       string        `json:"name"`
	Ext        string        `json:"ext"`
	Render     string        `json:"render"`
	Size       uint64        `json:"size"`
	Hits       int64         `json:"hits"`
	Stored     bool          `json:"stored"`
	Fetchable  bool          `json:"fetchable"`
	Fetch      *string       `json:"fetch"`
	CreatedAt  string        `json:"created_at"`
	AccessedAt string        `json:"accessed_at"`
	TgFileID   string        `json:"tg_file_id"`
	SHA256     *string       `json:"sha256"`
	URL        *string       `json:"url"`
	Thumb      *string       `json:"thumb"`
	Chat       *listChatName `json:"chat"`
	Sender     *listIDName   `json:"sender"`
	Message    *listMsgRef   `json:"message"`
	Minithumb  string        `json:"minithumb,omitempty"` // a video's blurred preview, a data: URL
	// Duration is a video's, audio's, voice note's or video note's
	// length in seconds, from the content_extra of the message that
	// carried it; left out when unknown.
	Duration int `json:"duration,omitempty"`
	// Role is what the file is to Message when it is not that
	// message's own file (a cover, a link preview's picture); null
	// when it is, or when there is no message.
	Role *listFileRole `json:"role"`
}

// listFileRole is FileRow.role: the file's role (logdb.FileRoles) and
// the content kind of the message it plays it in.
type listFileRole struct {
	Role        string `json:"role"`
	MessageKind string `json:"message_kind,omitempty"`
}

// filesProps are the files page's (FilesProps).
type filesProps struct {
	listState
	Rows        []listFile `json:"rows"`
	Preview     *listFile  `json:"preview"`
	CanFetch    bool       `json:"can_fetch"`
	StoredBytes *int64     `json:"stored_bytes,omitempty"`
}

// thumbTypes are the file types whose bytes are an image the pages
// can show as a thumbnail; notImage are the extensions of those that
// are not one after all: a video sticker (webm) and an animated one
// (tgs, Lottie), which an image cannot draw.
var (
	thumbTypes = map[string]bool{"photo": true, "sticker": true, "thumbnail": true}
	notImage   = map[string]bool{"webm": true, "tgs": true}
)

// hasThumb is whether a file's own bytes serve as its thumbnail: a
// stored picture that is an image.
func hasThumb(stored bool, fileType, ext string) bool {
	return stored && thumbTypes[fileType] && !notImage[strings.ToLower(ext)]
}

// isVideo is whether a file is a video whose thumbnail is a picture
// of its own (FileStills): a video, an animation, a video note, a
// video (.webm) sticker.
func isVideo(fileType, ext string) bool {
	switch fileType {
	case "video", "animation", "video_note":
		return true
	case "sticker":
		return strings.EqualFold(ext, "webm")
	}
	return false
}

// listFileRows are the rows as FileRow. A file has a URL the request
// may open, as each row of the Drogon files list links to
// /files/<token>: a stored one's bytes, and an unstored one's whenever
// a page view may have them fetched from Telegram (Server.fileLink, a
// small file), which opening the URL then does through the daemon; a
// big one the request may fetch has the action instead. Only a stored
// picture has a thumbnail, so that listing files fetches nothing: a
// stored photo or sticker is its own, and a video's is the stored
// cover, else the stored Telegram thumbnail, of a message that carried
// it. A file's length is that of the same message, read in the same
// batch (logdb.FileOrigins).
func (s *Server) listFileRows(ctx context.Context, sc scope.Scope, signedIn bool,
	rows []logdb.Row) ([]listFile, error) {
	ids := make([]uint64, 0, len(rows))
	for _, r := range rows {
		ids = append(ids, r.Uint("id"))
	}
	origins, err := logdb.FileOrigins(ctx, s.ro, sc, ids)
	if err != nil {
		return nil, err
	}
	stills, err := s.fileStills(ctx, sc, rows)
	if err != nil {
		return nil, err
	}
	out := make([]listFile, 0, len(rows))
	for _, r := range rows {
		f := s.fileRow(sc, signedIn, r, stills)
		// A file sent in no message the viewer sees (a profile or
		// group photo) has no chat and no message: null.
		if o, ok := origins[f.ID]; ok {
			f.Chat = &listChatName{ID: o.ChatID, Title: o.ChatTitle, Kind: o.ChatKind}
			f.Message = &listMsgRef{Scope: o.Scope, ID: o.MessageID}
			if o.Role != "" {
				// The message's detail describes its own file.
				f.Role = &listFileRole{Role: o.Role, MessageKind: o.Kind}
			} else {
				f.Duration = parseExtra(o.Extra).Duration
			}
			if o.SenderID != 0 {
				f.Sender = &listIDName{ID: o.SenderID, Name: o.Sender}
			}
		}
		out = append(out, f)
	}
	return out, nil
}

// fileStills are the stills of the videos among rows of the files
// registry's columns, in one batch (logdb.FileStills).
func (s *Server) fileStills(ctx context.Context, sc scope.Scope,
	rows []logdb.Row) (map[uint64]logdb.FileStill, error) {
	return logdb.FileStills(ctx, s.ro, sc, videoIDs(rows))
}

// videoIDs are the ids of the videos among rows of the files registry's
// columns (isVideo).
func videoIDs(rows []logdb.Row) []uint64 {
	var videos []uint64
	for _, r := range rows {
		if isVideo(r.Str("file_type"), r.Str("file_ext")) {
			videos = append(videos, r.Uint("id"))
		}
	}
	return videos
}

// fileRender is how a file of the files registry renders, as the chat
// renders a message's media of that type (renderOf): a sticker by its
// extension, a document by its extension.
func fileRender(fileType, ext string) string {
	f := logdb.FileRef{Type: fileType, Ext: ext}
	return renderOf(fileType, stickerFormat(contentExtra{}, f), f)
}

// fileRow is a row of the files registry's columns as FileRow, without
// the message that carried it (listFileRows adds that).
func (s *Server) fileRow(sc scope.Scope, signedIn bool, r logdb.Row,
	stills map[uint64]logdb.FileStill) listFile {
	f := listFile{ID: r.Uint("id"), FileType: r.Str("file_type"),
		Name: r.Str("orig_file_name"), Ext: r.Str("file_ext"),
		Size: r.Uint("file_size"), Hits: r.Int("hit_count"), Stored: r.Bool("on_disk"),
		CreatedAt: dbTime(r.Str("created_at")), AccessedAt: dbTime(r.Str("accessed_at")),
		TgFileID: r.Str("tg_file_id"),
	}
	if f.Name == "" {
		f.Name = strconv.FormatUint(f.ID, 10)
		if f.Ext != "" {
			f.Name += "." + f.Ext
		}
	}
	if h := r.Str("sha256_hex"); h != "" {
		// A metadata-only row has none.
		h = strings.ToLower(h)
		f.SHA256 = &h
	}
	l := s.fileLink(sc, signedIn, f.ID, f.Size, f.Stored, r.Bool("is_generated"))
	f.URL, f.Fetchable, f.Fetch = l.URL, l.Fetchable, l.Fetch
	if hasThumb(f.Stored, f.FileType, f.Ext) {
		f.Thumb = f.URL
	}
	if f.FileType == "thumbnail" {
		// A video's still is a picture to the pages
		// (FileRow.file_type has no thumbnail).
		f.FileType = "photo"
	}
	f.Render = fileRender(f.FileType, f.Ext)
	if st, ok := stills[f.ID]; ok {
		// Stored, so the size does not matter.
		if id := cmp.Or(st.Cover, st.Thumbnail); id != 0 {
			f.Thumb = s.fileLink(sc, signedIn, id, 0, true, false).URL
		}
		// As the chat's media (mediaDetail): only valid base64.
		if _, err := base64.StdEncoding.DecodeString(st.Minithumb); err == nil && st.Minithumb != "" {
			f.Minithumb = "data:image/jpeg;base64," + st.Minithumb
		}
	}
	return f
}

func loadFiles(ctx context.Context, pr *pageRequest) (any, error) {
	s := pr.s
	run, err := filesList.run(ctx, pr)
	if err != nil {
		return nil, err
	}
	sc := pr.scope
	p := filesProps{listState: run.state, CanFetch: sc.Can(scope.FetchFiles)}
	signedIn := pr.sess != nil
	if p.Rows, err = s.listFileRows(ctx, sc, signedIn, run.res.Rows); err != nil {
		return nil, err
	}
	if want := pr.query.Get("file"); isInt(want) {
		res, err := logdb.Search(ctx, s.ro, sc, logdb.Files, logdb.SearchRequest{
			Conds: []logdb.Cond{cond("id", "=", want)}, Internal: true, Limit: 1})
		if err != nil {
			return nil, err
		}
		if rows, err := s.listFileRows(ctx, sc, signedIn, res.Rows); err != nil {
			return nil, err
		} else if len(rows) == 1 {
			p.Preview = &rows[0]
		}
	}
	if n, ok := logdb.StoredBytes(ctx, s.ro, sc); ok {
		p.StoredBytes = &n
	}
	return p, nil
}

// listChat is a message's chat (ChatRef).
type listChat struct {
	ID    int64   `json:"id"`
	Title string  `json:"title"`
	Kind  string  `json:"kind"`
	Photo *string `json:"photo"`
}

// listMessage is a row of the message lists (MessageRow).
type listMessage struct {
	ID          uint64    `json:"id"`
	Scope       string    `json:"scope"`
	MessageID   int64     `json:"message_id"`
	Date        string    `json:"date"`
	Chat        listChat  `json:"chat"`
	Sender      *listUser `json:"sender"`
	ContentType string    `json:"content_type"`
	Snippet     string    `json:"snippet"`
	Forwarded   *string   `json:"forwarded"`
	Edited      bool      `json:"edited"`
	DeletedAt   *string   `json:"deleted_at"`
	// DeletedOnRecheck: found deleted by a recheck (onRecheck).
	DeletedOnRecheck bool `json:"deleted_on_recheck,omitempty"`
	// ServiceType is a service message's action (chat_add_members,
	// pin_message, ...), as the chat's MessageExtra gives it, so a
	// list can label the row; null for every other row.
	ServiceType *string `json:"service_type"`
}

// messagesProps are the message lists' (MessagesProps).
type messagesProps struct {
	listState
	Rows  []listMessage `json:"rows"`
	Scope string        `json:"scope"`
	// The search engine's part, as in the search API's answer: only
	// with an engine configured. Engine is the capabilities entry of
	// the list's entity, so the page offers the options without a
	// further request.
	Backend    *logdb.Backend `json:"backend,omitempty"`
	Highlights [][]string     `json:"highlights,omitempty"`
	Facets     []logdb.Facet  `json:"facets,omitempty"`
	Engine     *engineCaps    `json:"engine,omitempty"`
}

// snippetRunes is how much of a message's text a list shows.
const snippetRunes = 300

// listSnippet is a row's text, or its summary when it has none
// (snippets.go), cut to snippetRunes with an ellipsis.
func (v view) listSnippet(k kinds, row uint64, text, contentType, fileName string) string {
	if text == "" {
		text = v.summary(k, row, contentType, fileName)
	}
	if utf8.RuneCountInString(text) <= snippetRunes {
		return text
	}
	n := 0
	for i := range text {
		if n == snippetRunes {
			return text[:i] + "..."
		}
		n++
	}
	return text
}

func loadMessages(ctx context.Context, pr *pageRequest, l *listSpec, msgScope string) (any, error) {
	s := pr.s
	run, err := l.run(ctx, pr)
	if err != nil {
		return nil, err
	}
	sc := pr.scope
	p := messagesProps{listState: run.state, Rows: []listMessage{}, Scope: msgScope,
		Backend: run.res.Backend, Highlights: run.res.Highlights, Facets: run.res.Facets,
		Engine: s.listEngineCaps(ctx, sc, l.entity)}
	// The rows without text are summarised by their kind.
	textless := []logdb.Message{}
	for _, r := range run.res.Rows {
		if r.Str("text") == "" {
			textless = append(textless, logdb.Message{RowID: r.Uint("id"),
				ContentType: r.Str("content_type"), Extra: []byte(r.Str("kind_extra"))})
		}
	}
	k, err := s.loadSummaries(ctx, sc, textless)
	if err != nil {
		return nil, err
	}
	v := pr.view()
	var fwd []uint64
	for _, r := range run.res.Rows {
		m := listMessage{ID: r.Uint("id"), Scope: msgScope, MessageID: r.Int("message_id"),
			Date: unixTime(r.Int("date_unix")), ContentType: r.Str("content_type"),
			Snippet: v.listSnippet(k, r.Uint("id"), r.Str("text"), r.Str("content_type"),
				r.Str("kind_file")),
			Edited: r.Int("edit_date") != 0, DeletedAt: dbTimePtr(r, "deleted_at"),
			DeletedOnRecheck: !r.Null("deleted_at") && r.Bool("deleted_on_recheck")}
		if st := r.Str("service_type"); st != "" && m.ContentType == "service" {
			m.ServiceType = &st
		}
		if msgScope == "group" {
			m.Chat = listChat{ID: r.Int("chat_id"), Title: logdb.GroupTitle(r.Str("group_title")),
				Kind: "group", Photo: s.photoPtr(r.Uint("group_photo"))}
			switch {
			case !r.Null("sender_user_id"):
				m.Sender = &listUser{ID: r.Int("sender_user_id"),
					Name:     logdb.DisplayName(r.Str("su_first"), r.Str("su_last")),
					Username: r.Str("su_username"), Type: r.Str("su_type"),
					Photo: s.photoPtr(r.Uint("su_photo"))}
				if m.Sender.Type == "" {
					m.Sender.Type = "unknown"
				}
			case !r.Null("sender_chat_id"):
				// Posted as a chat: named only when the viewer
				// may see that chat.
				m.Sender = &listUser{ID: r.Int("sender_chat_id"), Name: "(hidden)",
					Type: "unknown"}
				if sc.SeesGroup(r.Int("sender_chat_id")) {
					m.Sender.Name = logdb.GroupTitle(r.Str("sg_title"))
					m.Sender.Username = r.Str("sg_username")
					m.Sender.Photo = s.photoPtr(r.Uint("sg_photo"))
				}
			}
		} else {
			peer := logdb.DisplayName(r.Str("peer_first"), r.Str("peer_last"))
			photo := s.photoPtr(r.Uint("peer_photo"))
			m.Chat = listChat{ID: r.Int("chat_id"), Title: peer, Kind: "user", Photo: photo}
			if !r.Bool("is_outgoing") {
				t := r.Str("peer_type")
				if t == "" {
					t = "unknown"
				}
				m.Sender = &listUser{ID: r.Int("chat_id"), Name: peer,
					Username: r.Str("peer_username"), Type: t, Photo: photo}
			}
		}
		if r.Bool("is_forwarded") {
			fwd = append(fwd, m.ID)
		}
		p.Rows = append(p.Rows, m)
	}
	origins, err := logdb.ForwardOrigins(ctx, s.ro, sc, msgScope, fwd)
	if err != nil {
		return nil, err
	}
	for i := range p.Rows {
		if name, ok := origins[p.Rows[i].ID]; ok {
			p.Rows[i].Forwarded = &name
		} else if r := run.res.Rows[i]; r.Bool("is_forwarded") {
			p.Rows[i].Forwarded = ptrTo("unknown")
		}
	}
	return p, nil
}

func loadGroupMessages(ctx context.Context, pr *pageRequest) (any, error) {
	return loadMessages(ctx, pr, groupMessagesList, "group")
}

func loadPrivateMessages(ctx context.Context, pr *pageRequest) (any, error) {
	return loadMessages(ctx, pr, privateMessagesList, "private")
}
