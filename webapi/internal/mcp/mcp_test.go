// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package mcp

import (
	"context"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"testing"
)

// The parts that need no database. The tools themselves, against
// dummy data and the Drogon web's answers, are tested through the HTTP
// endpoint (internal/server/mcp_test.go).

func TestFormatFloat(t *testing.T) {
	t.Parallel()
	// As nlohmann::json dump() spells them.
	for f, want := range map[float64]string{
		0: "0.0", 25: "25.0", 100: "100.0", 1.5: "1.5", 15.384615384615385: "15.384615384615385",
		0.0001: "0.0001", 0.00001: "1e-05", 1e14: "100000000000000.0", 1e15: "1e+15",
		-2.5: "-2.5", 123456.789: "123456.789", 1.25e-7: "1.25e-07", 3e100: "3e+100",
	} {
		if got := formatFloat(f); got != want {
			t.Errorf("formatFloat(%v) = %s, want %s", f, got, want)
		}
	}
}

func TestEncode(t *testing.T) {
	t.Parallel()
	got := string(encode(map[string]any{
		"b": "<a href=\"x\">&</a>\u2028/\\\b\f\n\r\t\x01\x7f é",
		"a": []any{int64(-1), true, nil, 2.0},
		"c": map[string]any{},
		"d": "\xff",
	}))
	want := `{"a":[-1,true,null,2.0],"b":"<a href=\"x\">&</a>` + "\u2028" +
		`/\\\b\f\n\r\t\u0001` + "\x7f é" + `","c":{},"d":"` + "\uFFFD" + `"}`
	if got != want {
		t.Errorf("encode:\n got %s\nwant %s", got, want)
	}
}

func TestDecodeIntegers(t *testing.T) {
	t.Parallel()
	v, err := decode([]byte(`{"a":5,"b":5.0,"c":-0,"d":1e2,"e":"5","f":18446744073709551615}`))
	if err != nil {
		t.Fatal(err)
	}
	o := v.(map[string]any)
	for k, want := range map[string]bool{"a": true, "b": false, "c": true, "d": false, "e": false, "f": true} {
		if _, ok := intValue(o[k]); ok != want {
			t.Errorf("%s: integer %v, want %v", k, ok, want)
		}
	}
	if n, _ := intValue(o["f"]); n != -1 {
		t.Errorf("an unsigned above int64 wraps: %d", n)
	}
	if _, err := decode([]byte(`{} x`)); err == nil {
		t.Error("trailing data accepted")
	}
	if clampIntArg(map[string]any{"limit": o["f"]}, "limit", 50, 1, 200) != 1 {
		t.Error("clampIntArg does not take the low 32 bits")
	}
}

func TestDates(t *testing.T) {
	t.Parallel()
	for in, want := range map[any]string{
		"2026-01-31":                "1769817600",
		"2026-01-31T12:00:00":       "1769860800",
		"2026-01-31 12:00:00":       "1769860800",
		"2026-01-31T12:00:00Z":      "1769860800",
		"2026-01-31T12:00:00.250Z":  "1769860800",
		"2026-01-31T19:00:00+07:00": "1769860800",
		"2026-01-31T19:00:00+0700":  "1769860800",
		"2026-01-31T07:30:00-04:30": "1769860800",
		"2026-02-31":                "1772496000", // rolled over, as timegm does
		"2026-1-5":                  "1767571200",
		"1769860800":                "1769860800",
		"0":                         "0",
	} {
		got, err := toUnixSeconds(in)
		if err != nil || got != want {
			t.Errorf("%v: %s %v, want %s", in, got, err, want)
		}
	}
	v, _ := decode([]byte(`1769860800`))
	if got, err := toUnixSeconds(v); err != nil || got != "1769860800" {
		t.Errorf("a number: %s %v", got, err)
	}
	for in, msg := range map[any]string{
		"yesterday":              `could not parse date "yesterday"`,
		"2026-01-31T12:00:00+7":  "malformed UTC offset",
		"2026-01-31T12:00:00.":   "malformed fractional seconds",
		"2026-01-31 UTC":         `unrecognised trailing " UTC"`,
		"2026-01-31T25:00:00":    `unrecognised trailing "T25:00:00"`,
		strings.Repeat("1-", 20): "date value is too long",
		true:                     "a date value must be a string",
	} {
		_, err := toUnixSeconds(in)
		if err == nil || !strings.Contains(err.Error(), msg) {
			t.Errorf("%v: %v, want %q", in, err, msg)
		}
	}
}

func TestCompile(t *testing.T) {
	t.Parallel()
	f := func(s string) any {
		v, err := decode([]byte(s))
		if err != nil {
			t.Fatal(err)
		}
		return v
	}
	for _, c := range []struct{ in, sql, binds, ft string }{
		{`{"field":"text","op":"match","value":"kernel"}`,
			"(MATCH(m.text) AGAINST(? IN BOOLEAN MODE))", "kernel", "kernel"},
		{`{"and":[{"field":"group_id","value":-5},{"not":{"field":"text","op":"match","value":"x"}}]}`,
			"((m.chat_id = ?) AND (NOT (MATCH(m.text) AGAINST(? IN BOOLEAN MODE))))", "-5|x", ""},
		{`{"or":[{"field":"edited","op":"=","value":false},{"field":"deleted","op":"!=","value":false}]}`,
			"((m.edit_date = 0) OR (m.deleted_at IS NOT NULL))", "", ""},
		{`{"field":"deleted_at","op":"between","value":["2026-01-31",1]}`,
			"(m.deleted_at BETWEEN FROM_UNIXTIME(?) AND FROM_UNIXTIME(?))", "1769817600|1", ""},
		{`{"field":"sender_user_id","op":"in","value":[1,"+2"]}`,
			"(m.sender_user_id IN (?, ?))", "1|+2", ""},
		{`{"field":"edited","op":"is_not_null"}`, "(m.edit_date <> 0)", "", ""},
	} {
		got, err := compile(msgSchema, f(c.in))
		if err != nil {
			t.Errorf("%s: %v", c.in, err)
			continue
		}
		var b []string
		for _, x := range got.binds {
			b = append(b, x.(string))
		}
		if got.sql != c.sql || strings.Join(b, "|") != c.binds || got.ftValue != c.ft {
			t.Errorf("%s:\n got %s %v %q\nwant %s %s %q", c.in, got.sql, b, got.ftValue, c.sql, c.binds, c.ft)
		}
	}
	got, _ := compile(userSchema, f(`{"field":"username","op":"contains","value":"a_b%"}`))
	if got.sql != `(un.username LIKE ? ESCAPE '\\')` || got.binds[0] != `%a\_b\%%` {
		t.Errorf("contains: %s %v", got.sql, got.binds)
	}
	if got, _ := compile(msgSchema, f(`{}`)); got.sql != "" {
		t.Errorf("{} is no filter: %s", got.sql)
	}
	deep := `{"field":"date","op":">","value":0}`
	for i := 0; i < 9; i++ {
		deep = `{"not":` + deep + `}`
	}
	many := `{"or":[` + strings.TrimSuffix(strings.Repeat(`{"field":"date","value":0},`, 64), ",") + `]}`
	for in, msg := range map[string]string{
		deep:                                        "nested too deeply (max 8)",
		many:                                        "too many conditions (max 64)",
		`{"field":"nope"}`:                          `unknown field "nope"`,
		`{"field":"text","value":"x"}`:              `the only operator is "match"`,
		`{"field":"content_type","value":"x"}`:      `"x" is not valid for field "content_type"`,
		`{"field":"date","op":"~","value":1}`:       `unknown operator "~"`,
		`{"field":"date","op":"in","value":[]}`:     `"in" needs a non-empty array`,
		`{"field":"group_id","value":"1x"}`:         `needs an integer, got "1x"`,
		`{"field":"deleted","op":"<","value":true}`: "is a flag",
		`{"and":{}}`:                                `"AND" needs an array`,
		`[1]`:                                       "each filter must be a JSON object",
		`{"field":"date","or":[]}`:                  "exactly one of",
	} {
		_, err := compile(msgSchema, f(in))
		if err == nil || !strings.Contains(err.Error(), msg) {
			t.Errorf("%s: %v, want %q", in, err, msg)
		}
	}
}

func TestWords(t *testing.T) {
	t.Parallel()
	// A run of three or more becomes one, as the tool's description
	// says (the Drogon code kept two).
	for in, want := range map[string]string{
		"yesss": "yes", "coffee": "coffee", "hmmmm": "hm", "sooooo": "so", "hmm": "hmm",
		"\U00020820\U00020820": "\U00020820\U00020820", "ééé": "é", "ab": "ab",
	} {
		if got := collapseElongation(in); got != want {
			t.Errorf("collapse(%q) = %q, want %q", in, got, want)
		}
	}
	for in, want := range map[string]bool{
		"wkwkwkwk": true, "hahaha": true, "aaaa": true, "kakak": false, "bandung": false, "wkw": false,
	} {
		if got := isRepetitive(in); got != want {
			t.Errorf("isRepetitive(%q) = %v", in, got)
		}
	}
	freq := map[string]int64{}
	var total int64
	f := &wordFilter{minLen: 3, useStopwords: true, extra: map[string]struct{}{"panic": {}}}
	countWords("Kernel PANIC yang gak wkwkwk see https://x.org/kernel mail rina@example.org "+
		"@someone 2026 kernel_dev yesss Ünïcode", f, freq, &total)
	var got []string
	for w, n := range freq {
		got = append(got, w+"="+string(rune('0'+n)))
	}
	sort.Strings(got)
	if strings.Join(got, " ") != "dev=1 kernel=2 mail=1 see=1 someone=1 yes=1 Ünïcode=1" || total != 8 {
		t.Errorf("counted %v (%d)", got, total)
	}
}

// The word lists are the Drogon web's, word for word.
func TestWordListsMatchDrogon(t *testing.T) {
	t.Parallel()
	quoted := regexp.MustCompile(`"([^"]*)"`)
	read := func(file, from string) map[string]bool {
		b, err := os.ReadFile(filepath.Join("../../../web/src/mcp", file))
		if err != nil {
			t.Fatal(err)
		}
		s := string(b)
		s = s[strings.Index(s, from):]
		s = s[:strings.Index(s, "};")]
		out := map[string]bool{}
		for _, m := range quoted.FindAllStringSubmatch(s, -1) {
			out[m[1]] = true
		}
		return out
	}
	sw := read("Stopwords.hpp", "kStopwords[] = {")
	if len(sw) != 885 || len(stopwords) != len(sw) {
		t.Errorf("stopwords: %d here, %d in Drogon", len(stopwords), len(sw))
	}
	for w := range sw {
		if !isStopword(w) {
			t.Errorf("stopword %q missing", w)
		}
	}
	// The Go list is Drogon's plus goNoiseExtra: words that only now
	// result from collapsing a run to one character ("hmmm" -> "hm").
	nw := read("NoiseWords.hpp", "kNoise = {")
	if len(nw) != 384 || len(noiseWords) != len(nw)+len(goNoiseExtra) {
		t.Errorf("noise words: %d here, %d in Drogon plus %d", len(noiseWords), len(nw), len(goNoiseExtra))
	}
	for _, w := range goNoiseExtra {
		if !isNoiseWord(w) {
			t.Errorf("extra noise word %q missing", w)
		}
	}
	for w := range nw {
		if !isNoiseWord(w) {
			t.Errorf("noise word %q missing", w)
		}
	}
}

func TestUnifiedDiff(t *testing.T) {
	t.Parallel()
	for _, c := range []struct{ a, b, want string }{
		{"same", "same", ""},
		{"a\nb\nc", "a\nB\nc", "@@ -2,1 +2,1 @@\n-b\n+B\n"},
		{"a\nc", "a\nb\nc", "@@ -1,0 +2,1 @@\n+b\n"},
		{"x\ny", "y\nz", "@@ -1,2 +1,2 @@\n-x\n y\n+z\n"},
		{strings.Repeat("a\n", 401), "b", "@@ too large to diff @@\n-402 line(s) replaced by 1 line(s); request format \"full\" for the text\n"},
	} {
		if got := unifiedDiff(c.a, c.b); got != c.want {
			t.Errorf("diff(%q, %q) = %q, want %q", c.a, c.b, got, c.want)
		}
	}
}

func TestCursor(t *testing.T) {
	t.Parallel()
	c := cursorDecode(cursorEncode("edit_date", "1794700900", 5003))
	if !c.ok || c.key != "edit_date" || c.value != "1794700900" || c.id != 5003 {
		t.Errorf("round trip: %+v", c)
	}
	for _, bad := range []string{"", "!!", "a", cursorEncode("date", "1 OR 1", 1), cursorEncode("date", "", 1), "ZGF0ZQ"} {
		if cursorDecode(bad).ok {
			t.Errorf("%q accepted", bad)
		}
	}
	if strtoll("12x") != 12 || strtoll(" -7") != -7 || strtoll("99999999999999999999") != 1<<63-1 {
		t.Error("strtoll")
	}
	if s, cut := truncateUTF8("héllo", 2); s != "h" || !cut {
		t.Errorf("truncate: %q %v", s, cut)
	}
}

func TestTokens(t *testing.T) {
	t.Parallel()
	good := "tgmcp_" + strings.Repeat("0a", 32)
	for s, want := range map[string]bool{
		good: true, strings.ToUpper(good): false, good + "0": false, good[1:]: false,
		"tgmcp_" + strings.Repeat("0g", 32): false,
	} {
		if LooksLikeToken(s) != want {
			t.Errorf("LooksLikeToken(%q) != %v", s, want)
		}
	}
	for h, want := range map[string]string{
		"Bearer " + good: good, "bearer\t " + good + " \r\n": good, "BEARER x": "x",
		"Bearer": "", "Bearerx": "", "Basic " + good: "", "": "",
	} {
		if got := FromAuthorizationHeader(h); got != want {
			t.Errorf("FromAuthorizationHeader(%q) = %q, want %q", h, got, want)
		}
	}
}

// Direct messages are out of reach but through a token's scope: no
// file of this package but private.go, which only the gate's choice
// reaches, names their table. The name is put together so this file
// does not name it either.
func TestNoPrivateMessages(t *testing.T) {
	t.Parallel()
	table := "telegram_" + "private_messages"
	files, _ := filepath.Glob("*.go")
	if len(files) == 0 {
		t.Fatal("no files")
	}
	named := false
	for _, f := range files {
		b, err := os.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		has := strings.Contains(string(b), table)
		if f == "private.go" {
			named = has
		} else if has {
			t.Errorf("%s names %s", f, table)
		}
	}
	if !named {
		t.Errorf("private.go does not name %s: is the check still checking?", table)
	}
}

// What a scope lets through, before the exposure list: everything when
// not narrowed; when narrowed the groups named and, with Default, the
// groups in no other federation.
func TestReachAllowsGroup(t *testing.T) {
	t.Parallel()
	if !(Reach{}).allowsGroup(-1) {
		t.Error("an unnarrowed reach refuses a group")
	}
	r := Reach{narrowed: true, groups: map[int64]bool{-1: true}}
	if !r.allowsGroup(-1) || r.allowsGroup(-2) {
		t.Error("a group entry")
	}
	r = Reach{narrowed: true, groups: map[int64]bool{}, defaultFed: true,
		federated: map[int64]bool{-2: true}}
	if !r.allowsGroup(-1) || r.allowsGroup(-2) {
		t.Error("the Default federation")
	}
	if (Reach{narrowed: true}).allowsGroup(-1) {
		t.Error("a narrowed reach with nothing active reaches a group")
	}
}

// The protocol without a database: tools/list lists every tool in name
// order, read-only, and a handler's failures take the right channel.
func TestServer(t *testing.T) {
	t.Parallel()
	s := NewServer(ServerInfo{Name: "x", Version: "1"}, []Tool{
		{Name: "b", Handler: func(context.Context, map[string]any) (any, error) { return nil, toolErr("no") }},
		{Name: "a", Handler: func(context.Context, map[string]any) (any, error) {
			return nil, &RPCError{InvalidParams, "bad"}
		}},
		{Name: "c", Handler: func(context.Context, map[string]any) (any, error) { return nil, os.ErrClosed }},
		{Name: "a"}, // duplicate and without a handler: left out
	})
	for body, want := range map[string]string{
		`{"jsonrpc":"2.0","id":1,"method":"tools/list"}`: `{"id":1,"jsonrpc":"2.0","result":{"tools":[` +
			`{"annotations":{"readOnlyHint":true},"inputSchema":{"properties":{},"type":"object"},"name":"a"},` +
			`{"annotations":{"readOnlyHint":true},"inputSchema":{"properties":{},"type":"object"},"name":"b"},` +
			`{"annotations":{"readOnlyHint":true},"inputSchema":{"properties":{},"type":"object"},"name":"c"}]}}`,
		`{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"a"}}`: `{"error":{"code":-32602,"message":"bad"},"id":2,"jsonrpc":"2.0"}`,
		`{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"b"}}`: `{"id":3,"jsonrpc":"2.0","result":{"content":[{"text":"no","type":"text"}],"isError":true}}`,
		`{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"c"}}`: `{"id":4,"jsonrpc":"2.0","result":{"content":[{"text":"Tool failed: file already closed","type":"text"}],"isError":true}}`,
		`{"jsonrpc":"2.0","id":{"x":1},"method":"nope"}`:                       `{"error":{"code":-32601,"message":"Unknown method: nope"},"id":null,"jsonrpc":"2.0"}`,
	} {
		got, ok := s.HandleRaw(context.Background(), []byte(body))
		if !ok || string(got) != want {
			t.Errorf("%s:\n got %s\nwant %s", body, got, want)
		}
	}
	if _, ok := s.HandleRaw(context.Background(), []byte(`{"jsonrpc":"2.0","method":"x"}`)); ok {
		t.Error("a notification answered")
	}
}
