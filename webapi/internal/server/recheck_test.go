// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"slices"
	"strings"
	"sync"
	"testing"
	"time"

	"tgloggerd/webapi/internal/auth"
	"tgloggerd/webapi/internal/daemon"
	"tgloggerd/webapi/internal/testdb"
)

// The chat rechecks asked of the daemon (recheck.go), against a fake
// daemon on loopback that records what it is asked.

// fakeRecheckDaemon answers POST /v1/chats/{id}/recheck with status
// (202 by default) and records each ask, and 404 to anything else; while hold is set, every
// answer waits for it to be closed.
type fakeRecheckDaemon struct {
	t   *testing.T
	srv *httptest.Server

	mu     sync.Mutex
	asks   []recheckKey
	status int
	hold   chan struct{}
	bad    []string // what was wrong with a request, if anything
}

func newFakeRecheckDaemon(t *testing.T) *fakeRecheckDaemon {
	f := &fakeRecheckDaemon{t: t, status: http.StatusAccepted}
	bearer, err := daemon.Bearer(testKey)
	if err != nil {
		t.Fatal(err)
	}
	f.srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !strings.HasSuffix(r.URL.Path, "/recheck") {
			// The pages' other calls (status, member counts) are
			// not this test's.
			http.NotFound(w, r)
			return
		}
		var chat int64
		var body struct {
			Around *int64 `json:"around"`
		}
		_, perr := fmt.Sscanf(r.URL.Path, "/v1/chats/%d/recheck", &chat)
		derr := json.NewDecoder(r.Body).Decode(&body)
		f.mu.Lock()
		switch {
		case r.Method != http.MethodPost || perr != nil || fmt.Sprintf("/v1/chats/%d/recheck", chat) != r.URL.Path:
			f.bad = append(f.bad, r.Method+" "+r.URL.Path)
		case r.Header.Get("Authorization") != bearer:
			f.bad = append(f.bad, "authorization "+r.Header.Get("Authorization"))
		case derr != nil || body.Around == nil:
			f.bad = append(f.bad, fmt.Sprintf("body: %v", derr))
		default:
			f.asks = append(f.asks, recheckKey{chat, *body.Around})
		}
		status, hold := f.status, f.hold
		f.mu.Unlock()
		if hold != nil {
			<-hold
		}
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(status)
		switch status {
		case http.StatusAccepted:
			io.WriteString(w, `{"queued":true}`)
		case http.StatusOK:
			io.WriteString(w, `{"queued":false,"reason":"recent"}`)
		default:
			io.WriteString(w, `{"error":"unavailable"}`)
		}
	}))
	t.Cleanup(f.srv.Close)
	return f
}

func (f *fakeRecheckDaemon) client() *daemon.Client {
	dc, err := daemon.New(f.srv.URL, testKey, time.Second)
	if err != nil {
		f.t.Fatal(err)
	}
	return dc
}

// taken returns the asks so far and forgets them, and fails the test
// on any malformed request.
func (f *fakeRecheckDaemon) taken() []recheckKey {
	f.t.Helper()
	f.mu.Lock()
	defer f.mu.Unlock()
	if len(f.bad) > 0 {
		f.t.Errorf("bad requests: %v", f.bad)
		f.bad = nil
	}
	a := f.asks
	f.asks = nil
	return a
}

func (f *fakeRecheckDaemon) set(status int, hold chan struct{}) {
	f.mu.Lock()
	f.status, f.hold = status, hold
	f.mu.Unlock()
}

// fakeClock is a clock a test moves by hand.
type fakeClock struct {
	mu sync.Mutex
	t  time.Time
}

func (c *fakeClock) now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.t
}

func (c *fakeClock) add(d time.Duration) {
	c.mu.Lock()
	c.t = c.t.Add(d)
	c.mu.Unlock()
}

func quietLog() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

// recheckEnv is a chat environment ("sample" and "chats" fixtures)
// whose daemon is f, with rechecks limited by cfg.
func recheckEnv(t *testing.T, f *fakeRecheckDaemon, cfg RecheckConfig) (*chatEnv, *rechecker) {
	t.Helper()
	ro := testdb.New(t, testdb.Logger, "sample", "chats").DB
	app := testdb.New(t, testdb.Web, "accounts").DB
	signer := auth.NewSigner(testKey)
	e := &chatEnv{t: t, ro: ro, cookies: &auth.Cookies{Signer: signer, Timeout: 3600, Secure: true},
		tokens: auth.NewFileTokens(testKey)}
	clock := func() time.Time { return chatNow }
	dc := f.client()
	rc := newRechecker(quietLog(), dc, clock, cfg)
	e.h = New(Deps{
		Log: quietLog(), RO: ro, App: app, Signer: signer, Cookies: e.cookies,
		Limiter: auth.NewRateLimiter(5, time.Hour), Daemon: dc, Tokens: e.tokens,
		Media: MediaConfig{StoreFileCap: 1 << 30}, clock: clock, rechecks: rc,
	})
	t.Cleanup(rc.wait)
	return e, rc
}

// Serving a chat window, a chat page or a message page asks the daemon
// for a recheck of that stretch in the background: the answer does not
// wait for the daemon, a failing daemon changes nothing in it, and the
// same page names the same stretch each time.
func TestRecheckOnServe(t *testing.T) {
	t.Parallel()
	f := newFakeRecheckDaemon(t)
	e, rc := recheckEnv(t, f, RecheckConfig{})
	admin := e.as("admin")

	// The answer comes while the daemon still holds the ask.
	hold := make(chan struct{})
	f.set(http.StatusAccepted, hold)
	done := make(chan int, 1)
	go func() {
		code, _, _ := e.get(fmt.Sprintf("/api/chats/group/%d/messages", pagingLab), admin)
		done <- code
	}()
	select {
	case code := <-done:
		if code != 200 {
			t.Errorf("newest window: %d", code)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("the window waited for the daemon")
	}
	close(hold)
	rc.wait()
	if got := f.taken(); !slices.Equal(got, []recheckKey{{pagingLab, 0}}) {
		t.Fatalf("newest window asked %v", got)
	}

	// A failing daemon is only logged.
	f.set(http.StatusServiceUnavailable, nil)
	cases := []struct {
		path string
		want recheckKey
	}{
		{fmt.Sprintf("/api/chats/group/%d/messages?before=%d", pagingLab, labMsg(61)),
			recheckKey{pagingLab, labMsg(61)}},
		{fmt.Sprintf("/api/chats/group/%d/messages?after=%d", pagingLab, labMsg(5)),
			recheckKey{pagingLab, labMsg(5)}},
		{fmt.Sprintf("/api/chats/group/%d/messages?around=%d", pagingLab, labMsg(90)),
			recheckKey{pagingLab, labMsg(90)}},
		// The oldest messages: around their first.
		{fmt.Sprintf("/api/chats/group/%d/messages?after=0", pagingLab),
			recheckKey{pagingLab, labMsg(1)}},
		{fmt.Sprintf("/api/chats/private/%d/messages", rina), recheckKey{rina, 0}},
		// The chat page: opened at a message, and its newest.
		{fmt.Sprintf("/api/page/groups/%d/chat?at=%d", pagingLab, labMsg(30)),
			recheckKey{pagingLab, labMsg(30)}},
		{fmt.Sprintf("/api/page/groups/%d/chat", openLab), recheckKey{openLab, 0}},
		// The message page: the stretch the chat opened at it names.
		{"/api/page/messages/group/1100", recheckKey{pagingLab, labMsg(100)}},
	}
	for _, c := range cases {
		if code, _, body := e.get(c.path, admin); code != 200 {
			t.Fatalf("%s: %d %s", c.path, code, body)
		}
		rc.wait()
		if got := f.taken(); !slices.Equal(got, []recheckKey{c.want}) {
			t.Errorf("%s asked %v, want %v", c.path, got, c.want)
		}
	}
	// Asked again within the hour (the message page's stretch is the
	// chat's at=100), nothing is asked.
	for _, p := range []string{
		fmt.Sprintf("/api/chats/group/%d/messages?before=%d", pagingLab, labMsg(61)),
		fmt.Sprintf("/api/chats/group/%d/messages?around=%d", pagingLab, labMsg(100)),
		fmt.Sprintf("/api/chats/group/%d/messages", pagingLab),
	} {
		e.get(p, admin)
	}
	rc.wait()
	if got := f.taken(); len(got) != 0 {
		t.Errorf("asked again: %v", got)
	}

	// What the viewer may not see asks nothing: a signed-out visitor
	// of a group that is not exposed, a member's hidden private chat.
	e.get(fmt.Sprintf("/api/chats/group/%d/messages?before=%d", pagingLab, labMsg(7)), "")
	e.get(fmt.Sprintf("/api/chats/private/%d/messages?before=%d", rina, 900003<<20), e.as("researcher"))
	rc.wait()
	if got := f.taken(); len(got) != 0 {
		t.Errorf("hidden chats asked %v", got)
	}
	// A signed-out visitor of an exposed group asks too.
	e.get(fmt.Sprintf("/api/chats/group/%d/messages?before=%d", openLab, 800003<<20), "")
	rc.wait()
	if got := f.taken(); !slices.Equal(got, []recheckKey{{openLab, 800003 << 20}}) {
		t.Errorf("signed-out visitor asked %v", got)
	}
}

// The limits per stretch, on a clock the test moves: a chat's newest
// messages once a minute, the stretch around one anchor once an hour,
// whoever asks; different stretches do not hold each other up.
func TestRecheckStretchLimits(t *testing.T) {
	t.Parallel()
	f := newFakeRecheckDaemon(t)
	clock := &fakeClock{t: chatNow}
	rc := newRechecker(quietLog(), f.client(), clock.now, RecheckConfig{})
	t.Cleanup(rc.wait)
	ask := func(chat, around int64, signedIn bool, want bool) {
		t.Helper()
		if got := rc.ask(chat, around, signedIn); got != want {
			t.Errorf("ask(%d, %d, %v) = %v, want %v", chat, around, signedIn, got, want)
		}
		rc.wait()
	}
	ask(pagingLab, 0, true, true)
	ask(pagingLab, 0, true, false)
	ask(pagingLab, 0, false, false) // the stretch's interval binds visitors too
	ask(openLab, 0, true, true)
	ask(pagingLab, 100, true, true)
	ask(pagingLab, 200, true, true)
	ask(pagingLab, 100, true, false)
	clock.add(59 * time.Second)
	ask(pagingLab, 0, true, false)
	clock.add(time.Second)
	ask(pagingLab, 0, true, true)
	ask(pagingLab, 100, true, false)
	clock.add(time.Hour - time.Minute - time.Second)
	ask(pagingLab, 100, true, false)
	clock.add(time.Second)
	ask(pagingLab, 100, true, true)
	ask(pagingLab, 200, true, true)
	want := []recheckKey{{pagingLab, 0}, {openLab, 0}, {pagingLab, 100}, {pagingLab, 200},
		{pagingLab, 0}, {pagingLab, 100}, {pagingLab, 200}}
	if got := f.taken(); !slices.Equal(got, want) {
		t.Errorf("asked %v\nwant %v", got, want)
	}

	// The intervals are settings; off asks nothing, nor does a
	// rechecker without the daemon API.
	rc2 := newRechecker(quietLog(), f.client(), clock.now, RecheckConfig{Newest: 10 * time.Second,
		Older: 20 * time.Second, PublicPerMinute: 1, PublicBurst: 1, Timeout: time.Second,
		MaxInFlight: 1})
	if !rc2.ask(1, 0, true) {
		t.Error("first ask refused")
	}
	rc2.wait()
	clock.add(10 * time.Second)
	if !rc2.ask(1, 0, true) {
		t.Error("ask after the newest interval refused")
	}
	rc2.wait()
	f.taken()
	off := newRechecker(quietLog(), f.client(), clock.now, RecheckConfig{Off: true, Newest: time.Second})
	none := newRechecker(quietLog(), nil, clock.now, RecheckConfig{})
	if off.ask(2, 0, true) || none.ask(2, 0, true) || (*rechecker)(nil).ask(2, 0, true) {
		t.Error("asked while off")
	}
}

// At most MaxInFlight asks wait for the daemon; one more is dropped and
// does not count against its stretch, so it is asked once the daemon
// has answered.
func TestRecheckInFlight(t *testing.T) {
	t.Parallel()
	f := newFakeRecheckDaemon(t)
	clock := &fakeClock{t: chatNow}
	cfg := DefaultRecheck
	cfg.MaxInFlight = 2
	rc := newRechecker(quietLog(), f.client(), clock.now, cfg)
	hold := make(chan struct{})
	f.set(http.StatusAccepted, hold)
	if !rc.ask(1, 0, true) || !rc.ask(2, 0, true) {
		t.Fatal("asks refused")
	}
	if rc.ask(3, 0, true) {
		t.Error("a third ask while two wait")
	}
	close(hold)
	rc.wait()
	if !rc.ask(3, 0, true) {
		t.Error("the dropped stretch is held up")
	}
	rc.wait()
	if got := f.taken(); len(got) != 3 {
		t.Errorf("asked %v", got)
	}
}

// All signed-out visitors share one bucket: a burst of them over many
// chats and anchors gets PublicBurst asks through, older stretches only
// while more than PublicReserve tokens are left, and the bucket refills
// at PublicPerMinute. Signed-in viewers do not draw from it.
func TestRecheckPublicBucket(t *testing.T) {
	t.Parallel()
	f := newFakeRecheckDaemon(t)
	clock := &fakeClock{t: chatNow}
	cfg := DefaultRecheck
	cfg.MaxInFlight = 1000 // only the bucket stops them
	rc := newRechecker(quietLog(), f.client(), clock.now, cfg)
	t.Cleanup(rc.wait)
	var chats int64 // distinct chats, so no stretch limit stops them
	burst := func(n int, older bool) int {
		t.Helper()
		base := -1002000000000 - chats
		chats += int64(n)
		var (
			wg     sync.WaitGroup
			mu     sync.Mutex
			passed int
		)
		for i := range n {
			wg.Add(1)
			go func() {
				defer wg.Done()
				around := int64(0)
				if older {
					around = int64(1000 + i)
				}
				ok := rc.ask(base-int64(i), around, false)
				if ok {
					mu.Lock()
					passed++
					mu.Unlock()
				}
			}()
		}
		// Only once every ask is made: a Wait on the rechecker's
		// WaitGroup must not overlap an ask that starts it from
		// zero, so no goroutine of the burst waits itself.
		wg.Wait()
		rc.wait()
		return passed
	}
	// Older stretches first: they stop with the reserve left.
	if n := burst(200, true); n != DefaultRecheck.PublicBurst-DefaultRecheck.PublicReserve {
		t.Errorf("older burst: %d passed", n)
	}
	// The reserve still serves the newest messages.
	if n := burst(200, false); n != DefaultRecheck.PublicReserve {
		t.Errorf("newest burst on the reserve: %d passed", n)
	}
	if n := burst(50, false); n != 0 {
		t.Errorf("empty bucket: %d passed", n)
	}
	// Signed-in viewers are not limited by it.
	for i := range 30 {
		if !rc.ask(-1003000000000-int64(i), 0, true) {
			t.Fatalf("signed-in ask %d refused", i)
		}
		rc.wait()
	}
	// Half a minute refills half the rate: all of it newest-only.
	clock.add(30 * time.Second)
	if n := burst(100, true); n != 0 {
		t.Errorf("older on a low bucket: %d passed", n)
	}
	if n := burst(100, false); n != DefaultRecheck.PublicPerMinute/2 {
		t.Errorf("newest after 30 s: %d passed", n)
	}
	// It never holds more than the burst.
	clock.add(time.Hour)
	if n := burst(300, false); n != DefaultRecheck.PublicBurst {
		t.Errorf("after an hour: %d passed", n)
	}
	total := 2*DefaultRecheck.PublicBurst + DefaultRecheck.PublicPerMinute/2 + 30
	if got := f.taken(); len(got) != total {
		t.Errorf("the daemon was asked %d times, want %d", len(got), total)
	}

	// PublicPerMinute 0: signed-out visitors ask for nothing.
	cfg = DefaultRecheck
	cfg.PublicPerMinute = 0
	shut := newRechecker(quietLog(), f.client(), clock.now, cfg)
	if shut.ask(5, 0, false) || !shut.ask(5, 0, true) {
		t.Error("with no public rate")
	}
	shut.wait()
	f.taken()
}

// A live chat refetches its tail after its last message on every new
// batch. A window after an anchor that reaches the newest message
// names the newest stretch, so those refetches ask once per Newest
// instead of once per new message; one that stops short of the newest
// still names its anchor.
func TestRecheckLiveTail(t *testing.T) {
	t.Parallel()
	f := newFakeRecheckDaemon(t)
	e, rc := recheckEnv(t, f, RecheckConfig{})
	admin := e.as("admin")
	f.set(http.StatusAccepted, nil)
	for _, c := range []struct {
		path string
		want []recheckKey
	}{
		{fmt.Sprintf("/api/chats/group/%d/messages?after=%d", openLab, 800001<<20),
			[]recheckKey{{openLab, 0}}},
		// The next batch's refetch, within the minute: nothing.
		{fmt.Sprintf("/api/chats/group/%d/messages?after=%d", openLab, 800002<<20), nil},
		{fmt.Sprintf("/api/chats/group/%d/messages?after=%d", openLab, 800004<<20), nil},
		// Not at the newest (130 messages, windows of 60).
		{fmt.Sprintf("/api/chats/group/%d/messages?after=%d", pagingLab, labMsg(5)),
			[]recheckKey{{pagingLab, labMsg(5)}}},
	} {
		if code, _, body := e.get(c.path, admin); code != 200 {
			t.Fatalf("%s: %d %s", c.path, code, body)
		}
		rc.wait()
		if got := f.taken(); !slices.Equal(got, c.want) {
			t.Errorf("%s asked %v, want %v", c.path, got, c.want)
		}
	}
}
