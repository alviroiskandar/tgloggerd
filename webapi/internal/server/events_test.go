// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"bufio"
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"slices"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"tgloggerd/webapi/internal/auth"
	"tgloggerd/webapi/internal/daemon"
	"tgloggerd/webapi/internal/live"
	"tgloggerd/webapi/internal/live/livetest"
	"tgloggerd/webapi/internal/scope"
	"tgloggerd/webapi/internal/testdb"
	"tgloggerd/webapi/internal/webdb"
)

// The GET /api/events tests need no database: the session's epoch
// comes from a stub, and the daemon is livetest's fake on loopback.

type liveEnv struct {
	t       *testing.T
	d       *livetest.Daemon
	conn    *livetest.Conn // the hub's stream, after hello
	hub     *live.Hub
	s       *Server
	url     string
	cookie  string
	epoch   atomic.Uint32
	cookies *auth.Cookies
}

type liveOpts struct {
	hub     live.Options
	cfg     LiveConfig
	noHello bool // leave the hub waiting for its first frame
	noHub   bool // live updates off
	// state, when set, is the account's session state (else a
	// superadmin's), and ro the logger's database its scope reads.
	state *webdb.SessionState
	ro    *sql.DB
}

func newLiveEnv(t *testing.T, o liveOpts) *liveEnv {
	t.Helper()
	e := &liveEnv{t: t}
	quiet := slog.New(slog.NewTextHandler(io.Discard, nil))
	signer := auth.NewSigner(testKey)
	e.cookies = &auth.Cookies{Signer: signer, Timeout: 3600, Secure: true}
	e.s = &Server{
		log:     quiet,
		signer:  signer,
		cookies: e.cookies,
		verifier: &auth.Verifier{Cookies: e.cookies,
			State: func(context.Context, uint64, [32]byte) (webdb.SessionState, error) {
				st := webdb.SessionState{Role: "superadmin", Active: true, SessionRow: 1}
				if o.state != nil {
					st = *o.state
				}
				st.Epoch = e.epoch.Load()
				return st, nil
			}},
		liveCfg: o.cfg,
		ro:      o.ro,
	}
	if !o.noHub {
		e.d = livetest.New(t, testKey)
		dc, err := daemon.New(e.d.URL, testKey, time.Second)
		if err != nil {
			t.Fatal(err)
		}
		o.hub.Log = quiet
		o.hub.RetryMin, o.hub.RetryMax = 10*time.Millisecond, 50*time.Millisecond
		e.hub = live.NewHub(live.DaemonSource{Client: dc, Log: quiet}, o.hub)
		e.s.live = e.hub
		ctx, cancel := context.WithCancel(context.Background())
		done := make(chan struct{})
		go func() { e.hub.Run(ctx); close(done) }()
		t.Cleanup(func() { cancel(); <-done })
		e.conn = e.d.Accept(t)
		if !o.noHello {
			e.conn.Send(live.Event{Seq: 100, Kind: live.KindHello})
			waitFor(t, "the hub", e.hub.Up)
		}
	}
	mux := http.NewServeMux()
	for _, rt := range e.s.routes() {
		mux.HandleFunc(rt.pattern, rt.handler)
	}
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	e.url = srv.URL
	e.cookie = e.cookies.Issue(httptest.NewRecorder(), e.cookies.NewSession(1, "admin", "admin", 0))
	return e
}

func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for !cond() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(5 * time.Millisecond)
	}
}

type frame struct {
	id, event, data string
	comment         bool
	retry           string
}

// stream is one open GET /api/events.
type stream struct {
	t      *testing.T
	resp   *http.Response
	frames chan frame
	cancel context.CancelFunc
}

func (e *liveEnv) request(query string, hdr ...string) (*http.Response, context.CancelFunc) {
	e.t.Helper()
	ctx, cancel := context.WithCancel(context.Background())
	req, _ := http.NewRequestWithContext(ctx, http.MethodGet, e.url+"/api/events?"+query, nil)
	if e.cookie != "" {
		req.AddCookie(&http.Cookie{Name: "tgw_session", Value: e.cookie})
	}
	for i := 0; i+1 < len(hdr); i += 2 {
		req.Header.Set(hdr[i], hdr[i+1])
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		cancel()
		e.t.Fatal(err)
	}
	return resp, cancel
}

func (e *liveEnv) open(query string, hdr ...string) *stream {
	e.t.Helper()
	resp, cancel := e.request(query, hdr...)
	if resp.StatusCode != http.StatusOK {
		b, _ := io.ReadAll(resp.Body)
		cancel()
		e.t.Fatalf("GET /api/events?%s: %d %s", query, resp.StatusCode, b)
	}
	st := &stream{t: e.t, resp: resp, frames: make(chan frame, 1024), cancel: cancel}
	e.t.Cleanup(st.close)
	go func() {
		defer close(st.frames)
		br := bufio.NewReader(resp.Body)
		var f frame
		seen := false
		for {
			line, err := br.ReadString('\n')
			if err != nil {
				return
			}
			line = strings.TrimSuffix(line, "\n")
			if line == "" {
				if seen {
					st.frames <- f
				}
				f, seen = frame{}, false
				continue
			}
			seen = true
			k, v, _ := strings.Cut(line, ":")
			v = strings.TrimPrefix(v, " ")
			switch k {
			case "":
				f.comment = true
			case "id":
				f.id = v
			case "event":
				f.event = v
			case "data":
				f.data = v
			case "retry":
				f.retry = v
			}
		}
	}()
	return st
}

func (st *stream) close() {
	st.cancel()
	st.resp.Body.Close()
}

// next returns the next frame that is not a heartbeat.
func (st *stream) next() frame {
	st.t.Helper()
	for {
		select {
		case f, ok := <-st.frames:
			if !ok {
				st.t.Fatal("the stream ended")
			}
			if f.comment || f.retry != "" {
				continue
			}
			return f
		case <-time.After(5 * time.Second):
			st.t.Fatal("no frame")
		}
	}
}

func (st *stream) expect(id, event string) live.Event {
	st.t.Helper()
	f := st.next()
	if f.id != id || f.event != event {
		st.t.Fatalf("frame id=%q event=%q data=%s, want id=%q event=%q",
			f.id, f.event, f.data, id, event)
	}
	var ev live.Event
	if err := json.Unmarshal([]byte(f.data), &ev); err != nil {
		st.t.Fatalf("data %q: %v", f.data, err)
	}
	if ev.Kind != event || fmt.Sprint(ev.Seq) != id && id != "" {
		st.t.Fatalf("data %s does not match the frame", f.data)
	}
	return ev
}

// ended waits for the server to close the stream.
func (st *stream) ended() {
	st.t.Helper()
	for {
		select {
		case f, ok := <-st.frames:
			if !ok {
				return
			}
			if !f.comment {
				st.t.Fatalf("frame %+v before the end", f)
			}
		case <-time.After(5 * time.Second):
			st.t.Fatal("the stream did not end")
		}
	}
}

func msgEv(seq uint64, chat int64) live.Event {
	return live.Event{Seq: seq, Kind: live.KindMessageNew, ChatID: chat, MessageID: 1}
}

func TestEventsStream(t *testing.T) {
	t.Parallel()
	e := newLiveEnv(t, liveOpts{})
	st := e.open("topics=chat:-1,user:7,group:-3,stats")
	h := st.resp.Header
	if h.Get("Content-Type") != "text/event-stream" || h.Get("Cache-Control") != "no-store" ||
		h.Get("X-Accel-Buffering") != "no" {
		t.Fatalf("headers %v", h)
	}
	f := <-st.frames
	if f.retry != "3000" {
		t.Fatalf("first frame %+v, want retry", f)
	}
	st.expect("100", "hello")

	all := e.open("topics=messages")
	all.expect("100", "hello")

	e.conn.Send(msgEv(101, -1))
	e.conn.Send(msgEv(102, -2))
	e.conn.Send(live.Event{Seq: 103, Kind: live.KindUserUpdated, UserID: 8})
	e.conn.Send(live.Event{Seq: 104, Kind: live.KindUserUpdated, UserID: 7, Fields: []string{"photo"}})
	e.conn.Send(live.Event{Seq: 105, Kind: live.KindGroupUpdated, ChatID: -3})
	e.conn.Send(live.Event{Seq: 106, Kind: live.KindFileStored, FileID: 5})
	e.conn.Send(live.Event{Seq: 107, Kind: live.KindFileStored, FileID: 6, ChatID: -1, MessageID: 2})
	e.conn.Send(live.Event{Seq: 108, Kind: live.KindStatsChanged, ChatIDs: []int64{-1}, UserIDs: []int64{7}})
	e.conn.Send(live.Event{Seq: 109, Kind: "poll.voted", ChatID: -1})
	e.conn.Send(live.Event{Seq: 110, Kind: live.KindMessageDeleted, ChatID: -1, MessageID: 1})

	if ev := st.expect("101", "message.new"); ev.ChatID != -1 || ev.MessageID != 1 {
		t.Fatalf("%+v", ev)
	}
	if ev := st.expect("104", "user.updated"); ev.UserID != 7 || len(ev.Fields) != 1 {
		t.Fatalf("%+v", ev)
	}
	st.expect("105", "group.updated")
	if ev := st.expect("107", "file.stored"); ev.FileID != 6 || ev.MessageID != 2 {
		t.Fatalf("%+v", ev)
	}
	if ev := st.expect("108", "stats.changed"); len(ev.ChatIDs) != 1 || len(ev.UserIDs) != 1 {
		t.Fatalf("%+v", ev)
	}
	st.expect("110", "message.deleted")

	all.expect("101", "message.new")
	all.expect("102", "message.new")
	all.expect("107", "file.stored")
	all.expect("110", "message.deleted")
}

func TestEventsResume(t *testing.T) {
	t.Parallel()
	e := newLiveEnv(t, liveOpts{hub: live.Options{History: 4}})
	w := e.open("topics=chat:-1")
	w.expect("100", "hello")
	for seq := uint64(101); seq <= 106; seq++ {
		e.conn.Send(msgEv(seq, -1))
	}
	// Wait for the hub to have them all.
	for seq := 101; seq <= 106; seq++ {
		w.expect(fmt.Sprint(seq), "message.new")
	}

	st := e.open("topics=chat:-1&last=104")
	st.expect("106", "hello")
	st.expect("105", "message.new")
	st.expect("106", "message.new")

	// The browser's own reconnect: Last-Event-ID wins over last=.
	st = e.open("topics=chat:-1&last=101", "Last-Event-ID", "105")
	st.expect("106", "hello")
	st.expect("106", "message.new")

	// Further back than the history, or not a seq: a resync.
	e.open("topics=chat:-1&last=101").expect("106", "resync")
	e.open("topics=chat:-1", "Last-Event-ID", "x").expect("106", "resync")

	e.conn.Send(msgEv(107, -1))
	st.expect("107", "message.new")
}

func TestEventsScope(t *testing.T) {
	t.Parallel()
	e := newLiveEnv(t, liveOpts{})
	const hiddenChat, hiddenUser = -2, 9
	e.s.liveFilter = func(_ scope.Scope, ev live.Event) (live.Event, bool) {
		return filterEvent(ev, func(id int64) bool { return id != hiddenChat },
			func(id int64) bool { return id != hiddenUser })
	}
	st := e.open("topics=messages,stats,chat:-2,group:-2,user:9,user:7")
	st.expect("100", "hello")
	e.conn.Send(msgEv(101, hiddenChat))
	e.conn.Send(live.Event{Seq: 102, Kind: live.KindMessageEdited, ChatID: hiddenChat, MessageID: 4})
	e.conn.Send(live.Event{Seq: 103, Kind: live.KindFileStored, FileID: 3, ChatID: hiddenChat, MessageID: 4})
	e.conn.Send(live.Event{Seq: 104, Kind: live.KindGroupUpdated, ChatID: hiddenChat})
	e.conn.Send(live.Event{Seq: 105, Kind: live.KindUserUpdated, UserID: hiddenUser})
	e.conn.Send(live.Event{Seq: 106, Kind: live.KindStatsChanged, ChatIDs: []int64{hiddenChat}, UserIDs: []int64{hiddenUser}})
	e.conn.Send(live.Event{Seq: 107, Kind: live.KindStatsChanged, ChatIDs: []int64{-1, hiddenChat, 7}, UserIDs: []int64{hiddenUser, 7}})
	e.conn.Send(live.Event{Seq: 108, Kind: live.KindUserUpdated, UserID: 7})

	ev := st.expect("107", "stats.changed")
	if fmt.Sprint(ev.ChatIDs, ev.UserIDs) != "[-1 7] [7]" {
		t.Fatalf("stats %+v", ev)
	}
	st.expect("108", "user.updated")

	// Resuming replays through the same filter.
	re := e.open("topics=messages,stats,group:-2,user:9", "Last-Event-ID", "100")
	re.expect("108", "hello")
	if ev := re.expect("107", "stats.changed"); len(ev.ChatIDs) != 2 {
		t.Fatalf("%+v", ev)
	}
}

func TestFilterEvent(t *testing.T) {
	t.Parallel()
	no := func(int64) bool { return false }
	yes := func(int64) bool { return true }
	stats := live.Event{Seq: 1, Kind: live.KindStatsChanged, ChatIDs: []int64{-1}, UserIDs: []int64{2}}
	if _, ok := filterEvent(stats, no, no); ok {
		t.Fatal("a stats.changed with nothing visible passed")
	}
	more := stats
	more.More = true
	if ev, ok := filterEvent(more, no, no); !ok || len(ev.ChatIDs) != 0 || !ev.More {
		t.Fatalf("more: %+v %v", ev, ok)
	}
	if ev, ok := filterEvent(stats, yes, no); !ok || len(ev.ChatIDs) != 1 || len(ev.UserIDs) != 0 ||
		len(stats.UserIDs) != 1 {
		t.Fatalf("narrowed: %+v %v (the shared event must stay whole)", ev, ok)
	}
	for _, ev := range []live.Event{{Kind: "x.y", ChatID: -1}, {Kind: live.KindUserUpdated}, {Kind: live.KindMessageNew}} {
		if _, ok := filterEvent(ev, yes, yes); ok {
			t.Fatalf("%+v passed", ev)
		}
	}
	if _, ok := eventInScope(scope.Everything(), msgEv(1, -5)); !ok {
		t.Fatal("the scope hides nothing yet")
	}
}

// blockedWriter is a browser that stops reading: its writes block
// until released.
type blockedWriter struct {
	h       http.Header
	mu      sync.Mutex
	buf     strings.Builder
	block   atomic.Bool
	gate    chan struct{}
	waiting chan struct{}
	once    sync.Once
}

func (b *blockedWriter) Header() http.Header { return b.h }
func (b *blockedWriter) WriteHeader(int)     {}
func (b *blockedWriter) Write(p []byte) (int, error) {
	if b.block.Load() {
		b.once.Do(func() { close(b.waiting) })
		<-b.gate
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.Write(p)
}
func (b *blockedWriter) Flush() {}
func (b *blockedWriter) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.String()
}

func TestEventsSlowSubscriber(t *testing.T) {
	t.Parallel()
	e := newLiveEnv(t, liveOpts{hub: live.Options{Buffer: 2}})
	bw := &blockedWriter{h: http.Header{}, gate: make(chan struct{}), waiting: make(chan struct{})}
	bw.block.Store(true)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	r := httptest.NewRequest(http.MethodGet, "/api/events?topics=chat:-1", nil).WithContext(ctx)
	r.AddCookie(&http.Cookie{Name: "tgw_session", Value: e.cookie})
	done := make(chan struct{})
	go func() { e.s.RequireSession(e.s.events)(bw, r); close(done) }()
	<-bw.waiting // stuck writing its first frame
	waitFor(t, "the subscription", func() bool { return e.hub.Subscribers() == 1 })

	for seq := uint64(101); seq <= 120; seq++ {
		e.conn.Send(msgEv(seq, -1))
	}
	// A watcher that reads shows when the hub has published them all.
	waitFor(t, "the events", func() bool {
		s, st, err := e.hub.Subscribe(0, func(ev live.Event) (live.Event, bool) { return ev, false })
		if err != nil {
			return false
		}
		s.Close()
		return st.Seq == 120
	})
	bw.block.Store(false)
	close(bw.gate)
	waitFor(t, "the resync", func() bool {
		return strings.Contains(bw.String(), "id: 120\nevent: resync\n")
	})
	if n := strings.Count(bw.String(), "event: message.new"); n >= 20 {
		t.Fatalf("all %d events were kept for a reader that stopped", n)
	}
	cancel()
	<-done
}

func TestEventsSubscriberCap(t *testing.T) {
	t.Parallel()
	e := newLiveEnv(t, liveOpts{hub: live.Options{MaxSubscribers: 1}})
	a := e.open("topics=stats")
	a.expect("100", "hello")
	resp, cancel := e.request("topics=stats")
	b, _ := io.ReadAll(resp.Body)
	cancel()
	if resp.StatusCode != http.StatusServiceUnavailable || resp.Header.Get("Retry-After") == "" ||
		!strings.Contains(string(b), `"unavailable"`) {
		t.Fatalf("over the cap: %d %s", resp.StatusCode, b)
	}
	// A browser that goes away frees its slot.
	a.close()
	waitFor(t, "the slot", func() bool { return e.hub.Subscribers() == 0 })
	e.open("topics=stats").expect("100", "hello")
}

func TestEventsHeartbeat(t *testing.T) {
	t.Parallel()
	e := newLiveEnv(t, liveOpts{cfg: LiveConfig{Heartbeat: 30 * time.Millisecond}})
	st := e.open("topics=stats")
	st.expect("100", "hello")
	for {
		select {
		case f := <-st.frames:
			if f.comment {
				return
			}
		case <-time.After(5 * time.Second):
			t.Fatal("no heartbeat")
		}
	}
}

func TestEventsRevoked(t *testing.T) {
	t.Parallel()
	e := newLiveEnv(t, liveOpts{cfg: LiveConfig{Recheck: 30 * time.Millisecond}})
	st := e.open("topics=stats")
	st.expect("100", "hello")
	e.epoch.Store(1) // signed out everywhere
	f := st.next()
	if f.event != "session" || f.id != "" || f.data != `{"kind":"session","reason":"revoked"}` {
		t.Fatalf("%+v", f)
	}
	st.ended()
	waitFor(t, "the slot", func() bool { return e.hub.Subscribers() == 0 })
}

func TestEventsDaemonDown(t *testing.T) {
	t.Parallel()
	refused := func(e *liveEnv) {
		t.Helper()
		resp, cancel := e.request("topics=stats")
		defer cancel()
		b, _ := io.ReadAll(resp.Body)
		if resp.StatusCode != http.StatusServiceUnavailable || !strings.Contains(string(b), `"live_off"`) {
			t.Fatalf("%d %s", resp.StatusCode, b)
		}
	}
	// Off by configuration, and the daemon not connected yet.
	refused(newLiveEnv(t, liveOpts{noHub: true}))
	refused(newLiveEnv(t, liveOpts{noHello: true}))

	// The daemon goes away: open streams end after the grace period and
	// new ones are refused until it is back.
	e := newLiveEnv(t, liveOpts{hub: live.Options{Grace: 50 * time.Millisecond}})
	st := e.open("topics=stats")
	st.expect("100", "hello")
	e.d.Refuse(http.StatusServiceUnavailable)
	e.conn.End()
	st.ended()
	refused(e)
	e.d.Refuse(0)
	c := e.d.Accept(t)
	c.Send(live.Event{Seq: 100, Kind: live.KindHello})
	waitFor(t, "the hub", e.hub.Up)
	e.open("topics=stats&last=100").expect("100", "hello")
}

func TestEventsBadRequests(t *testing.T) {
	t.Parallel()
	e := newLiveEnv(t, liveOpts{})
	many := make([]string, maxTopics+1)
	for i := range many {
		many[i] = fmt.Sprintf("user:%d", i+1)
	}
	for _, q := range []string{"", "topics=", "topics=chat:", "topics=chat:x",
		"topics=chat:0", "topics=room:1", "topics=everything", "topics=stats&last=x",
		"topics=" + strings.Join(many, ",")} {
		resp, cancel := e.request(q)
		b, _ := io.ReadAll(resp.Body)
		cancel()
		if resp.StatusCode != http.StatusBadRequest || !strings.Contains(string(b), "bad_request") {
			t.Errorf("%.40q: %d %s", q, resp.StatusCode, b)
		}
	}
	e.cookie = ""
	resp, cancel := e.request("topics=stats")
	cancel()
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("without a session: %d", resp.StatusCode)
	}
}

func TestEventsManyTopics(t *testing.T) {
	t.Parallel()
	// A list page subscribes to everything on screen: a request line
	// of some 16 KB, repeats included.
	e := newLiveEnv(t, liveOpts{})
	var ts []string
	for i := 0; i < 1000; i++ {
		ts = append(ts, fmt.Sprintf("user:%d", 1000000000+i%600))
	}
	ts = append(ts, "group:-1001234567890", "stats")
	q := "topics=" + strings.Join(ts, ",")
	if len(q) < 16000 {
		t.Fatalf("only %d bytes", len(q))
	}
	st := e.open(q)
	st.expect("100", "hello")
	e.conn.Send(live.Event{Seq: 101, Kind: live.KindUserUpdated, UserID: 1000000599})
	e.conn.Send(live.Event{Seq: 102, Kind: live.KindGroupUpdated, ChatID: -1001234567890})
	st.expect("101", "user.updated")
	st.expect("102", "group.updated")
}

// eventInScope follows the roles: a superadmin gets every chat's and
// user's events, a member without grants none.
func TestEventInScopeRoles(t *testing.T) {
	t.Parallel()
	super := scope.For(scope.Viewer{ID: 1, Role: scope.Superadmin})
	member := scope.For(scope.Viewer{ID: 2, Role: scope.Member})
	events := []live.Event{
		{Kind: live.KindMessageNew, ChatID: -1001, MessageID: 5},
		{Kind: live.KindMessageNew, ChatID: 4242, MessageID: 6},
		{Kind: live.KindUserUpdated, UserID: 4242},
		{Kind: live.KindGroupUpdated, ChatID: -1001},
		{Kind: live.KindStatsChanged, ChatIDs: []int64{-1001}, UserIDs: []int64{4242}},
	}
	for _, e := range events {
		if _, ok := eventInScope(super, e); !ok {
			t.Errorf("superadmin misses %+v", e)
		}
		if _, ok := eventInScope(member, e); ok {
			t.Errorf("member gets %+v", e)
		}
	}
}

// A member's stream carries the events of its granted groups only, and
// of the users in them; a stats.changed keeps only their ids.
func TestLiveMemberGrants(t *testing.T) {
	t.Parallel()
	lg := testdb.New(t, testdb.Logger, "sample")
	e := newLiveEnv(t, liveOpts{ro: lg.DB, state: &webdb.SessionState{Role: "member", Active: true,
		SessionRow: 1, GrantedGroups: []int64{kernelChat, rustChat}}})
	// Dewi and Andi appear in the groups; Bima does not.
	st := e.open("topics=messages,stats,user:9299000001,user:9299000002,user:9299000004," +
		"group:-1001000000001,group:-1001000000002")
	st.expect("100", "hello")
	e.conn.Send(live.Event{Seq: 101, Kind: live.KindMessageNew, ChatID: gnuWeeb, MessageID: 1})
	e.conn.Send(live.Event{Seq: 102, Kind: live.KindMessageNew, ChatID: 9299000001, MessageID: 1})
	e.conn.Send(live.Event{Seq: 103, Kind: live.KindUserUpdated, UserID: 9299000002})
	e.conn.Send(live.Event{Seq: 104, Kind: live.KindGroupUpdated, ChatID: gnuWeeb})
	e.conn.Send(live.Event{Seq: 105, Kind: live.KindMessageNew, ChatID: kernelChat, MessageID: 2})
	e.conn.Send(live.Event{Seq: 106, Kind: live.KindUserUpdated, UserID: 9299000004})
	e.conn.Send(live.Event{Seq: 107, Kind: live.KindGroupUpdated, ChatID: kernelChat})
	e.conn.Send(live.Event{Seq: 108, Kind: live.KindStatsChanged,
		ChatIDs: []int64{gnuWeeb, rustChat}, UserIDs: []int64{9299000002, 9299000001}})
	st.expect("105", "message.new")
	st.expect("106", "user.updated")
	st.expect("107", "group.updated")
	ev := st.expect("108", "stats.changed")
	if !slices.Equal(ev.ChatIDs, []int64{rustChat}) || !slices.Equal(ev.UserIDs, []int64{9299000001}) {
		t.Errorf("stats.changed %+v", ev)
	}
}

// stuckWriter is a client that stopped reading on a connection no
// proxy bounds: a write blocks until the write deadline passes, or
// for good when none is set (until the test releases it).
type stuckWriter struct {
	h        http.Header
	mu       sync.Mutex
	deadline time.Time
	release  chan struct{}
	writing  chan struct{}
	once     sync.Once
}

func (s *stuckWriter) Header() http.Header { return s.h }
func (s *stuckWriter) WriteHeader(int)     {}
func (s *stuckWriter) Flush()              {}
func (s *stuckWriter) SetWriteDeadline(t time.Time) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.deadline = t
	return nil
}
func (s *stuckWriter) Write(p []byte) (int, error) {
	s.once.Do(func() { close(s.writing) })
	s.mu.Lock()
	dl := s.deadline
	s.mu.Unlock()
	var expired <-chan time.Time
	if !dl.IsZero() {
		expired = time.After(time.Until(dl))
	}
	select {
	case <-expired:
		return 0, os.ErrDeadlineExceeded
	case <-s.release:
		return len(p), nil
	}
}

// A client that stops reading ends its stream after the write timeout
// and frees its slot, without nginx's send timeout to end it.
func TestEventsWriteDeadline(t *testing.T) {
	t.Parallel()
	e := newLiveEnv(t, liveOpts{cfg: LiveConfig{WriteTimeout: 50 * time.Millisecond}})
	sw := &stuckWriter{h: http.Header{}, release: make(chan struct{}), writing: make(chan struct{})}
	t.Cleanup(func() { close(sw.release) })
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	r := httptest.NewRequest(http.MethodGet, "/api/events?topics=chat:-1", nil).WithContext(ctx)
	r.AddCookie(&http.Cookie{Name: "tgw_session", Value: e.cookie})
	done := make(chan struct{})
	go func() { e.s.RequireSession(e.s.events)(sw, r); close(done) }()
	<-sw.writing
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("a stream nobody reads is still open")
	}
	waitFor(t, "the slot", func() bool { return e.hub.Subscribers() == 0 })
}
