// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"

	"tgloggerd/webapi/internal/auth"
	"tgloggerd/webapi/internal/testdb"
)

// The MCP endpoint, on the logger fixture "mcp" and the web fixtures
// "accounts" and "mcp_tokens", each test in databases of its own.
//
// testdata/mcp/cases.json is a set of requests derived from the Drogon
// web's McpController, gwmcp and Tools.cpp: transport, tokens, every
// tool and its error paths, the exposure gate. testdata/mcp/drogon.json
// holds the Drogon web's answers to them, captured from it running on
// the same fixtures with the same WEB_APP_KEY (testKey) and
// WEB_PUBLIC_URL; TestMCPDrogonAnswers wants the same status and the
// same bytes from this service. The answers were captured with the
// cases replayed in order against a fresh copy of the fixtures, which
// is what this test does too.

// The fixture's ids (fixtures/logger/mcp.sql, fixtures/web/mcp_tokens.sql).
const (
	mcpG1      = int64(-1002000000001) // exposed
	mcpG3      = int64(-1002000000003) // not exposed
	mcpLive    = "tgmcp_1111111111111111111111111111111111111111111111111111111111111111"
	mcpRevoked = "tgmcp_2222222222222222222222222222222222222222222222222222222222222222"
	mcpPublic  = "https://tgd.example.test"
)

type mcpCase struct {
	Name    string            `json:"name"`
	Method  string            `json:"method"`
	Query   string            `json:"query"`
	Headers map[string]string `json:"headers"`
	Body    string            `json:"body"`
	// Compare is "exact" (status and body bytes) or "status" (status
	// and the JSON-RPC error code: the message quotes the JSON
	// parser's own wording, which differs between the two).
	Compare string `json:"compare"`
}

type mcpAnswer struct {
	Status      int    `json:"status"`
	ContentType string `json:"content_type"`
	Allow       string `json:"allow"`
	Auth        string `json:"www_authenticate"`
	Body        string `json:"body"`
}

type mcpEnv struct {
	t   *testing.T
	h   http.Handler
	lg  *testdb.DB
	web *testdb.DB
}

func newMCPEnv(t *testing.T, cfg MCPConfig, opts ...func(*Deps)) *mcpEnv {
	t.Helper()
	lg := testdb.New(t, testdb.Logger, "mcp")
	web := testdb.New(t, testdb.Web, "accounts", "mcp_tokens")
	signer := auth.NewSigner(testKey)
	if cfg.PublicURL == "" {
		cfg.PublicURL = mcpPublic
	}
	d := Deps{
		Log:     slog.New(slog.NewTextHandler(io.Discard, nil)),
		RO:      interpolating(t, lg),
		App:     interpolating(t, web),
		Signer:  signer,
		Cookies: &auth.Cookies{Signer: signer, Timeout: 3600, Secure: true},
		Limiter: auth.NewRateLimiter(5, time.Hour),
		Tokens:  auth.NewFileTokens(testKey),
		MCP:     cfg,
	}
	for _, o := range opts {
		o(&d)
	}
	h := New(d)
	return &mcpEnv{t: t, h: h, lg: lg, web: web}
}

func (e *mcpEnv) do(c mcpCase) *httptest.ResponseRecorder {
	u := "/mcp"
	if c.Query != "" {
		u += "?" + c.Query
	}
	req := httptest.NewRequest(c.Method, u, strings.NewReader(c.Body))
	for k, v := range c.Headers {
		req.Header.Set(k, v)
	}
	rec := httptest.NewRecorder()
	e.h.ServeHTTP(rec, req)
	return rec
}

// call runs one tool with the live token and returns the result object.
func (e *mcpEnv) call(tool string, args any) map[string]any {
	e.t.Helper()
	body, _ := json.Marshal(map[string]any{
		"jsonrpc": "2.0", "id": 1, "method": "tools/call",
		"params": map[string]any{"name": tool, "arguments": args},
	})
	rec := e.do(mcpCase{Method: http.MethodPost, Body: string(body),
		Headers: map[string]string{"Authorization": "Bearer " + mcpLive}})
	if rec.Code != http.StatusOK {
		e.t.Fatalf("%s: %d %s", tool, rec.Code, rec.Body)
	}
	var r struct {
		Result map[string]any `json:"result"`
		Error  any            `json:"error"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &r); err != nil || r.Error != nil {
		e.t.Fatalf("%s: %v %s", tool, err, rec.Body)
	}
	return r.Result
}

func loadMCPCases(t *testing.T) ([]mcpCase, map[string]mcpAnswer) {
	t.Helper()
	var cases []mcpCase
	var answers map[string]mcpAnswer
	for f, v := range map[string]any{"testdata/mcp/cases.json": &cases, "testdata/mcp/drogon.json": &answers} {
		b, err := os.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		if err := json.Unmarshal(b, v); err != nil {
			t.Fatalf("%s: %v", f, err)
		}
	}
	return cases, answers
}

// mcpOverride is the answer this service gives on purpose where Drogon
// answered otherwise, with why.
type mcpOverride struct {
	Why  string `json:"why"`
	Body string `json:"body"`
}

func loadMCPOverrides(t *testing.T) map[string]mcpOverride {
	t.Helper()
	var o map[string]mcpOverride
	b, err := os.ReadFile("testdata/mcp/overrides.json")
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(b, &o); err != nil {
		t.Fatalf("overrides.json: %v", err)
	}
	return o
}

func rpcErrorCode(body string) any {
	var r struct {
		Error struct {
			Code any `json:"code"`
		} `json:"error"`
	}
	_ = json.Unmarshal([]byte(body), &r)
	return r.Error.Code
}

func TestMCPDrogonAnswers(t *testing.T) {
	t.Parallel()
	cases, answers := loadMCPCases(t)
	overrides := loadMCPOverrides(t)
	record := map[string]string{}
	defer func() {
		if out := os.Getenv("WEBAPI_MCP_RECORD"); out != "" && len(record) > 0 {
			b, _ := json.MarshalIndent(record, "", "  ")
			if err := os.WriteFile(out, append(b, '\n'), 0o644); err != nil {
				t.Error(err)
			}
		}
	}()
	e := newMCPEnv(t, MCPConfig{Threads: 2})
	for _, c := range cases {
		want, ok := answers[c.Name]
		if !ok {
			t.Errorf("%s: no Drogon answer", c.Name)
			continue
		}
		rec := e.do(c)
		got := rec.Body.String()
		if rec.Code != want.Status {
			t.Errorf("%s: status %d, Drogon %d\n got %s\nwant %s", c.Name, rec.Code, want.Status, got, want.Body)
			continue
		}
		if a := rec.Header().Get("WWW-Authenticate"); a != "" || want.Auth != "" {
			t.Errorf("%s: WWW-Authenticate %q, Drogon %q", c.Name, a, want.Auth)
		}
		if a := rec.Header().Get("Allow"); a != want.Allow {
			t.Errorf("%s: Allow %q, Drogon %q", c.Name, a, want.Allow)
		}
		if want.Body != "" {
			if ct := rec.Header().Get("Content-Type"); ct != want.ContentType {
				t.Errorf("%s: Content-Type %q, Drogon %q", c.Name, ct, want.ContentType)
			}
		}
		// A case this service answers differently on purpose has its
		// own expected answer in overrides.json.
		if o, ok := overrides[c.Name]; ok {
			if o.Body == want.Body {
				t.Errorf("%s: override equals the Drogon answer; drop it", c.Name)
			}
			want.Body = o.Body
		}
		if got != want.Body && c.Compare != "status" {
			record[c.Name] = got
		}
		switch c.Compare {
		case "status":
			if g, w := rpcErrorCode(got), rpcErrorCode(want.Body); g != w {
				t.Errorf("%s: error code %v, Drogon %v", c.Name, g, w)
			}
		default:
			if got != want.Body {
				t.Errorf("%s: answers differ\n got %s\nwant %s", c.Name, got, want.Body)
			}
		}
	}
}

// Nothing of a group that is not on the exposure list is ever
// answered, whatever a tool is asked: its ids, titles, texts, admins
// and history stay out of every answer, while the exposed groups are
// answered.
func TestMCPNeverAnswersUnexposed(t *testing.T) {
	t.Parallel()
	e := newMCPEnv(t, MCPConfig{})
	// Texts and names found only in the unexposed groups' rows: every
	// message, title, username, edit, preview and admin of theirs. Their
	// ids are not on the list, since an answer may echo the id it was
	// asked about ("no readable group with id ...").
	secrets := []string{"Secret Ops", "secretops", "not for MCP", "Keluarga",
		"secret kernel plan", "kernel kernel kernel", "family dinner", "secret plan",
		"secret.example", "Boss", `"secret"`, `"family"`, `"dinner"`}
	mids := func(ns ...int64) []any {
		out := []any{}
		for _, n := range ns {
			out = append(out, n<<20)
		}
		return out
	}
	calls := []struct {
		tool string
		args map[string]any
	}{
		{"telegram_list_groups", map[string]any{"limit": 200}},
		{"telegram_get_group", map[string]any{"group_ids": []any{mcpG3, -1002000000004, mcpG1}}},
		{"telegram_get_group_history", map[string]any{"group_id": mcpG3}},
		{"telegram_list_group_admins", map[string]any{"group_id": mcpG3}},
		{"telegram_list_recent_messages", map[string]any{"limit": 200}},
		{"telegram_list_recent_messages", map[string]any{"group_id": mcpG3}},
		{"telegram_list_group_senders", map[string]any{"group_id": mcpG3, "include_total": true}},
		{"telegram_count_user_messages", map[string]any{"group_id": mcpG3, "user_id": 9300000002}},
		{"telegram_popular_words", map[string]any{"group_id": mcpG3, "start_date": 0, "include_stopwords": true}},
		{"telegram_search_messages", map[string]any{"limit": 200, "include_total": true}},
		{"telegram_search_messages", map[string]any{"group_id": mcpG3, "include_total": true}},
		{"telegram_search_messages", map[string]any{"filter": map[string]any{"or": []any{
			map[string]any{"field": "group_id", "op": "in", "value": []any{mcpG3, -1002000000004}},
			map[string]any{"field": "text", "op": "match", "value": "secret family"},
			map[string]any{"field": "deleted", "op": "=", "value": true},
		}}, "include_total": true, "limit": 200}},
		{"telegram_search_messages", map[string]any{"order_by": "deleted_at", "limit": 200}},
		{"telegram_search_messages", map[string]any{"order_by": "edit_date", "limit": 200,
			"filter": map[string]any{"field": "edited", "op": "is_not_null"}}},
		{"telegram_get_messages", map[string]any{"group_id": mcpG3, "message_ids": mids(301, 302), "include_history": true}},
		{"telegram_get_messages", map[string]any{"group_id": mcpG1, "message_ids": mids(301, 302, 401)}},
		{"telegram_get_message_history", map[string]any{"group_id": mcpG3, "message_id": int64(301) << 20}},
	}
	for _, c := range calls {
		r := e.call(c.tool, c.args)
		b, _ := json.Marshal(r)
		for _, s := range secrets {
			if bytes.Contains(b, []byte(s)) {
				t.Errorf("%s %v answers %q: %s", c.tool, c.args, s, b)
			}
		}
	}

	// The exposed groups are answered.
	r := e.call("telegram_list_groups", map[string]any{})
	if n := r["structuredContent"].(map[string]any)["count"]; n != float64(2) {
		t.Errorf("list_groups: %v groups, want the 2 exposed", n)
	}
}

// With nothing exposed, every message tool answers no rows.
func TestMCPEmptyAllowlist(t *testing.T) {
	t.Parallel()
	e := newMCPEnv(t, MCPConfig{})
	if _, err := e.lg.Exec("DELETE FROM telegram_public_groups"); err != nil {
		t.Fatal(err)
	}
	sc := func(r map[string]any) map[string]any {
		if r["isError"] == true {
			return nil
		}
		return r["structuredContent"].(map[string]any)
	}
	if c := sc(e.call("telegram_list_groups", map[string]any{}))["count"]; c != float64(0) {
		t.Errorf("list_groups: %v", c)
	}
	if c := sc(e.call("telegram_list_recent_messages", map[string]any{}))["count"]; c != float64(0) {
		t.Errorf("recent: %v", c)
	}
	s := sc(e.call("telegram_search_messages", map[string]any{"include_total": true}))
	if s["count"] != float64(0) || s["total"] != float64(0) {
		t.Errorf("search: %v", s)
	}
	m := sc(e.call("telegram_get_messages", map[string]any{"group_id": mcpG1, "message_ids": []any{int64(101) << 20}}))
	if m["count"] != float64(0) || m["missing_count"] != float64(1) {
		t.Errorf("get_messages: %v", m)
	}
	w := sc(e.call("telegram_popular_words", map[string]any{"group_id": mcpG1, "start_date": 0}))
	if w["messages_scanned"] != float64(0) {
		t.Errorf("popular_words: %v", w)
	}
	for _, tool := range []string{"telegram_get_group", "telegram_get_message_history", "telegram_get_group_history"} {
		r := e.call(tool, map[string]any{"group_id": mcpG1, "message_id": int64(101) << 20})
		if r["isError"] != true {
			t.Errorf("%s: %v, want a not-found error", tool, r)
		}
	}
}

// The tokens: missing, malformed, unknown, revoked and a disabled
// account's are refused with a plain 401; a live one works from the
// header or the query string, and its last use is stamped.
func TestMCPTokens(t *testing.T) {
	t.Parallel()
	e := newMCPEnv(t, MCPConfig{})
	ping := `{"jsonrpc":"2.0","id":1,"method":"ping"}`
	for _, c := range []struct {
		name, auth, query string
		want              int
	}{
		{"missing", "", "", 401},
		{"malformed", "Bearer tgmcp_12", "", 401},
		{"upper-case hex", "Bearer " + strings.ToUpper(mcpLive), "", 401},
		{"unknown", "Bearer tgmcp_" + strings.Repeat("ab", 32), "", 401},
		{"revoked", "Bearer " + mcpRevoked, "", 401},
		{"disabled account", "Bearer tgmcp_" + strings.Repeat("33", 32), "", 401},
		{"revoked in the query", "", "key=" + mcpRevoked, 401},
		{"live", "Bearer " + mcpLive, "", 200},
		{"live in the query", "", "key=" + mcpLive, 200},
		{"header before query", "Bearer " + mcpRevoked, "key=" + mcpLive, 401},
	} {
		h := map[string]string{}
		if c.auth != "" {
			h["Authorization"] = c.auth
		}
		rec := e.do(mcpCase{Method: http.MethodPost, Query: c.query, Body: ping, Headers: h})
		if rec.Code != c.want {
			t.Errorf("%s: %d %s, want %d", c.name, rec.Code, rec.Body, c.want)
		}
		if rec.Header().Get("WWW-Authenticate") != "" {
			t.Errorf("%s: a WWW-Authenticate header", c.name)
		}
	}
	var used, unused int
	if err := e.web.QueryRow("SELECT COUNT(last_used_at) FROM web_mcp_tokens WHERE id = 1").Scan(&used); err != nil {
		t.Fatal(err)
	}
	if err := e.web.QueryRow("SELECT COUNT(last_used_at) FROM web_mcp_tokens WHERE id <> 1").Scan(&unused); err != nil {
		t.Fatal(err)
	}
	if used != 1 || unused != 0 {
		t.Errorf("last_used_at stamped on %d live and %d refused tokens", used, unused)
	}

	// Revoking takes effect on the next request.
	if _, err := e.web.Exec("UPDATE web_mcp_tokens SET revoked_at = NOW() WHERE id = 1"); err != nil {
		t.Fatal(err)
	}
	rec := e.do(mcpCase{Method: http.MethodPost, Body: ping,
		Headers: map[string]string{"Authorization": "Bearer " + mcpLive}})
	if rec.Code != 401 {
		t.Errorf("after revoking: %d", rec.Code)
	}
}

// A token is authorised by its current owner, not by its minter: a
// disabled owner and a member owner without mcp_tokens are refused like
// an unknown token; a token transferred to an active owner works
// although its minter is disabled; one without an owner record counts
// as the superadmins'. Each change takes effect on the next request.
func TestMCPTokenOwner(t *testing.T) {
	t.Parallel()
	e := newMCPEnv(t, MCPConfig{})
	tok := func(c string) string { return "tgmcp_" + strings.Repeat(c, 32) }
	exec := func(q string, args ...any) {
		t.Helper()
		if _, err := e.web.Exec(q, args...); err != nil {
			t.Fatal(err)
		}
	}
	// id, minter, owner (0 for no owner record), token.
	for _, k := range []struct {
		id, minter, owner int
		tok               string
	}{
		{4, 7, 5, tok("44")}, // intern2025's, given to researcher
		{5, 1, 6, tok("55")}, // admin's, given to mod_kernel
		{6, 7, 0, tok("66")}, // intern2025's, no owner record
		{7, 1, 3, tok("77")}, // admin's, given to gw_admin
	} {
		exec("INSERT INTO web_mcp_tokens (id, web_user_id, name, token_sha256) "+
			"VALUES (?, ?, ?, UNHEX(SHA2(?, 256)))", k.id, k.minter, fmt.Sprint("t", k.id), k.tok)
		if k.owner != 0 {
			exec("INSERT INTO web_resource_relations (resource_type, resource_id, relation, "+
				"user_id) VALUES ('mcp_token', ?, 'owner', ?)", k.id, k.owner)
		}
	}
	exec("INSERT INTO web_member_abilities (web_user_id, ability) VALUES (5, 'mcp_tokens')")

	ping := `{"jsonrpc":"2.0","id":1,"method":"ping"}`
	check := func(name, token string, want int) {
		t.Helper()
		rec := e.do(mcpCase{Method: http.MethodPost, Body: ping,
			Headers: map[string]string{"Authorization": "Bearer " + token}})
		if rec.Code != want {
			t.Errorf("%s: %d %s, want %d", name, rec.Code, rec.Body, want)
		}
		if want == 401 && !strings.Contains(rec.Body.String(), "Invalid or revoked token") {
			t.Errorf("%s: %s, want the unknown token's answer", name, rec.Body)
		}
	}
	check("disabled owner", tok("33"), 401)
	check("member owner with mcp_tokens, minter disabled", tok("44"), 200)
	check("member owner without mcp_tokens", tok("55"), 401)
	check("no owner record, minter disabled", tok("66"), 200)
	check("admin owner, by role", tok("77"), 200)

	exec("INSERT INTO web_member_abilities (web_user_id, ability) VALUES (6, 'mcp_tokens')")
	check("member owner given mcp_tokens", tok("55"), 200)
	exec("DELETE FROM web_member_abilities WHERE web_user_id = 5")
	check("member owner that lost mcp_tokens", tok("44"), 401)
	exec("UPDATE web_users SET is_active = 0 WHERE id = 6")
	check("member owner disabled", tok("55"), 401)
	exec("UPDATE web_resource_relations SET user_id = 4 WHERE resource_type = 'mcp_token' " +
		"AND resource_id = 3")
	check("transferred from a disabled owner to an active admin", tok("33"), 200)
	exec("UPDATE web_users SET is_active = 1 WHERE id = 7")
	exec("UPDATE web_users SET is_active = 0 WHERE id = 3")
	check("minter enabled again, owner disabled", tok("77"), 401)
	exec("UPDATE web_users SET is_active = 0 WHERE role = 'superadmin'")
	check("no owner record, no active superadmin", tok("66"), 401)

	var stamped string
	if err := e.web.QueryRow("SELECT GROUP_CONCAT(id ORDER BY id) FROM web_mcp_tokens " +
		"WHERE last_used_at IS NOT NULL").Scan(&stamped); err != nil {
		t.Fatal(err)
	}
	if stamped != "3,4,5,6,7" {
		t.Errorf("last use stamped on tokens %s", stamped)
	}
}

// MCP_ALLOWED_ORIGINS, when set, refuses other browser origins with a
// 403 before the token is looked at; requests without Origin pass.
func TestMCPOrigins(t *testing.T) {
	t.Parallel()
	e := newMCPEnv(t, MCPConfig{AllowedOrigins: " https://a.example, https://b.example\r"})
	ping := `{"jsonrpc":"2.0","id":1,"method":"ping"}`
	for _, c := range []struct {
		origin string
		want   int
	}{{"", 200}, {"https://a.example", 200}, {"https://b.example", 200}, {"https://evil.example", 403}} {
		h := map[string]string{"Authorization": "Bearer " + mcpLive}
		if c.origin != "" {
			h["Origin"] = c.origin
		}
		if rec := e.do(mcpCase{Method: http.MethodPost, Body: ping, Headers: h}); rec.Code != c.want {
			t.Errorf("origin %q: %d, want %d", c.origin, rec.Code, c.want)
		}
	}
	rec := e.do(mcpCase{Method: http.MethodPost, Body: ping,
		Headers: map[string]string{"Origin": "https://evil.example"}})
	if rec.Code != 403 {
		t.Errorf("unlisted origin without a token: %d, want 403", rec.Code)
	}
	if e := newMCPEnv(t, MCPConfig{AllowedOrigins: "*"}); e.do(mcpCase{Method: http.MethodPost, Body: ping,
		Headers: map[string]string{"Authorization": "Bearer " + mcpLive, "Origin": "https://x.example"}}).Code != 200 {
		t.Error("* refuses an origin")
	}
}

// Without dates telegram_popular_words covers the last 30 days.
func TestMCPPopularWordsDefaultWindow(t *testing.T) {
	t.Parallel()
	e := newMCPEnv(t, MCPConfig{})
	// A group of the test's own, so the fixture's messages, whose
	// dates may or may not fall in the window, stay out of it.
	const g = int64(-1002000000009)
	now := time.Now().Unix()
	for _, q := range []string{
		"INSERT INTO telegram_groups (id, type, title, description, msg_count) " +
			"VALUES (-1002000000009, 'supergroup', 'Own', '', 2)",
		"INSERT INTO telegram_public_groups (group_id) VALUES (-1002000000009)",
	} {
		if _, err := e.lg.Exec(q); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := e.lg.Exec("INSERT INTO telegram_group_messages (id, chat_id, message_id, "+
		"sender_user_id, date, content_type, text) VALUES "+
		"(5901, ?, 901 << 20, 9300000001, ?, 'text', 'zebracorn zebracorn lately'), "+
		"(5902, ?, 902 << 20, 9300000001, ?, 'text', 'unicornzebra long ago')",
		g, now-86400, g, now-31*86400); err != nil {
		t.Fatal(err)
	}
	r := e.call("telegram_popular_words", map[string]any{"group_id": g})["structuredContent"].(map[string]any)
	start := int64(r["start_date"].(float64))
	if d := now - 30*86400 - start; d < -5 || d > 5 {
		t.Errorf("start_date %d, want about %d", start, now-30*86400)
	}
	if r["end_date"] != nil || r["messages_scanned"] != float64(1) {
		t.Errorf("window: %v", r)
	}
	words := r["words"].([]any)
	if len(words) != 2 || words[0].(map[string]any)["word"] != "zebracorn" {
		t.Errorf("words %v", words)
	}
}

// File links are left out without a public base URL, and are the
// /files/<token> links of the pages with one.
func TestMCPFileLinks(t *testing.T) {
	t.Parallel()
	e := newMCPEnv(t, MCPConfig{PublicURL: "https://other.example"})
	r := e.call("telegram_get_user", map[string]any{"user_id": 9300000001})["structuredContent"].(map[string]any)
	if got, want := r["profile_photo_url"], "https://other.example/files/"+auth.NewFileTokens(testKey).Encrypt(770002); got != want {
		t.Errorf("profile_photo_url %v, want %s", got, want)
	}
	s := &Server{tokens: auth.NewFileTokens(testKey)}
	if u := s.mcpFileURL(770002); u != "" {
		t.Errorf("without a base URL: %q", u)
	}
}

// A match value MySQL's full-text parser cannot read is a tool error
// that says how to write it, not the parser's bare message; a valid
// value with operators still searches.
func TestMCPMatchSyntax(t *testing.T) {
	t.Parallel()
	cases, _ := loadMCPCases(t)
	var base mcpCase
	for _, c := range cases {
		if c.Name == "search-match" {
			base = c
		}
	}
	if base.Name == "" {
		t.Fatal("no search-match case")
	}
	e := newMCPEnv(t, MCPConfig{Threads: 2})
	call := func(value string) map[string]any {
		c := base
		v, _ := json.Marshal(value)
		c.Body = strings.Replace(base.Body, `"value":"kernel"`, `"value":`+string(v), 1)
		rec := e.do(c)
		if rec.Code != 200 {
			t.Fatalf("%q: status %d %s", value, rec.Code, rec.Body)
		}
		var ans struct {
			Result map[string]any `json:"result"`
		}
		if err := json.Unmarshal(rec.Body.Bytes(), &ans); err != nil {
			t.Fatal(err)
		}
		return ans.Result
	}
	for _, bad := range []string{"+", "kernel +", "-", "(kernel"} {
		r := call(bad)
		text := fmt.Sprint(r["content"])
		if r["isError"] != true || !strings.Contains(text, "not valid full-text search syntax") ||
			!strings.Contains(text, "(MySQL: syntax error,") {
			t.Errorf("%q: %v", bad, r)
		}
	}
	if r := call("+kernel -rust"); r["isError"] == true {
		t.Errorf("a valid value refused: %v", r)
	}
}
