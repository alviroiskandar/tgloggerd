// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"bytes"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"net/url"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"tgloggerd/webapi/internal/auth"
	"tgloggerd/webapi/internal/testdb"
)

// A signed-out window is answered from the shared cache: the same
// bytes within its lifetime, even after the chat changed, and a fresh
// read after it. Signed-in viewers are never answered from it, and an
// answer that is not a 200 is not kept.
func TestPublicWindowCache(t *testing.T) {
	t.Parallel()
	ro := testdb.New(t, testdb.Logger, "sample", "chats").DB
	app := testdb.New(t, testdb.Web, "accounts").DB
	signer := auth.NewSigner(testKey)
	var now atomic.Int64
	now.Store(chatNow.Unix())
	e := &chatEnv{t: t, ro: ro, cookies: &auth.Cookies{Signer: signer, Timeout: 3600, Secure: true},
		tokens: auth.NewFileTokens(testKey)}
	e.h = New(Deps{
		Log:     slog.New(slog.NewTextHandler(io.Discard, nil)),
		RO:      ro,
		App:     app,
		Signer:  signer,
		Cookies: e.cookies,
		Limiter: auth.NewRateLimiter(5, time.Hour),
		Tokens:  e.tokens,
		Media:   MediaConfig{StoreFileCap: 1 << 30},
		clock:   func() time.Time { return time.Unix(now.Load(), 0) },
	})
	path := fmt.Sprintf("/api/chats/group/%d/messages?limit=5&ignored=1", openLab)
	code, h, first := e.get(path, "")
	if code != http.StatusOK || h.Get("Content-Type") != "application/json" ||
		h.Get("Cache-Control") == "" {
		t.Fatalf("first: %d %v %s", code, h, first)
	}
	if _, err := e.roExec("DELETE FROM telegram_public_groups WHERE group_id = ?", openLab); err != nil {
		t.Fatal(err)
	}
	// Within the lifetime, and with another order of the query and
	// parameters the window does not depend on: the kept answer.
	now.Add(1)
	other := fmt.Sprintf("/api/chats/group/%d/messages?other=2&limit=5", openLab)
	if code, _, b := e.get(other, ""); code != http.StatusOK || !bytes.Equal(b, first) {
		t.Fatalf("within the lifetime: %d, same bytes %v", code, bytes.Equal(b, first))
	}
	// A signed-in viewer reads for itself (the group is now hidden to
	// a member).
	if code, _, _ := e.get(path, e.as("researcher")); code != http.StatusNotFound {
		t.Fatalf("signed in: %d", code)
	}
	// After it: read again, and the refusal is not kept.
	now.Add(2)
	if code, _, b := e.get(path, ""); code != http.StatusUnauthorized {
		t.Fatalf("after the lifetime: %d %s", code, b)
	}
	if _, err := e.roExec("INSERT INTO telegram_public_groups (group_id, note) VALUES (?, 'tests')",
		openLab); err != nil {
		t.Fatal(err)
	}
	if code, _, b := e.get(path, ""); code != http.StatusOK || !bytes.Equal(b, first) {
		t.Fatalf("exposed again: %d %s", code, b)
	}
}

// Requests for the same window while it is read wait for that read:
// one read answers them all with the same bytes.
func TestWindowCacheOneRead(t *testing.T) {
	t.Parallel()
	c := newWindowCache(time.Minute)
	release := make(chan struct{})
	started := make(chan struct{})
	var once sync.Once
	fill := func(w http.ResponseWriter, r *http.Request) {
		once.Do(func() { close(started) })
		<-release
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"messages":[]}` + "\n"))
	}
	const n = 20
	bodies := make([][]byte, n)
	var wg sync.WaitGroup
	get := func(i int) {
		defer wg.Done()
		w := httptest.NewRecorder()
		c.serve(w, httptest.NewRequest(http.MethodGet, "/x", nil), "k", fill)
		bodies[i] = w.Body.Bytes()
	}
	wg.Add(1)
	go get(0)
	<-started
	for i := 1; i < n; i++ {
		wg.Add(1)
		go get(i)
	}
	// Let the others queue up behind the read before it ends.
	time.Sleep(50 * time.Millisecond)
	close(release)
	wg.Wait()
	if r := c.reads.Load(); r != 1 {
		t.Errorf("%d reads, want 1", r)
	}
	for i, b := range bodies {
		if string(b) != `{"messages":[]}`+"\n" {
			t.Errorf("answer %d: %q", i, b)
		}
	}
	// Another key is another read; no cache at all reads every time.
	c.serve(httptest.NewRecorder(), httptest.NewRequest(http.MethodGet, "/x", nil), "k2",
		func(w http.ResponseWriter, r *http.Request) { _, _ = w.Write([]byte("x")) })
	if r := c.reads.Load(); r != 2 {
		t.Errorf("%d reads, want 2", r)
	}
	off := newWindowCache(-1)
	calls := 0
	for range 2 {
		off.serve(httptest.NewRecorder(), httptest.NewRequest(http.MethodGet, "/x", nil), "k",
			func(w http.ResponseWriter, r *http.Request) { calls++ })
	}
	if calls != 2 {
		t.Errorf("without a cache: %d calls", calls)
	}
}

func TestWindowKey(t *testing.T) {
	t.Parallel()
	a, _ := url.ParseQuery("limit=5&before=9&x=1")
	b, _ := url.ParseQuery("before=9&limit=5")
	c, _ := url.ParseQuery("before=9&limit=6")
	if windowKey("group", "-1", a) != windowKey("group", "-1", b) {
		t.Error("the order or an ignored parameter changes the key")
	}
	if windowKey("group", "-1", b) == windowKey("group", "-1", c) ||
		windowKey("group", "-1", b) == windowKey("private", "-1", b) ||
		windowKey("group", "-1", b) == windowKey("group", "-2", b) {
		t.Error("different windows share a key")
	}
}
