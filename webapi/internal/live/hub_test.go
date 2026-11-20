// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package live_test

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"strings"
	"testing"
	"time"

	"tgloggerd/webapi/internal/daemon"
	"tgloggerd/webapi/internal/live"
	"tgloggerd/webapi/internal/live/livetest"
)

var testKey = []byte("0123456789abcdef0123456789abcdef")

var quiet = slog.New(slog.NewTextHandler(io.Discard, nil))

func all(e live.Event) (live.Event, bool) { return e, true }

type env struct {
	t   *testing.T
	d   *livetest.Daemon
	hub *live.Hub
}

// start runs a Hub against a fake daemon until the test ends.
func start(t *testing.T, o live.Options) *env {
	t.Helper()
	d := livetest.New(t, testKey)
	dc, err := daemon.New(d.URL, testKey, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	o.Log = quiet
	if o.RetryMin == 0 {
		o.RetryMin = 10 * time.Millisecond
		o.RetryMax = 50 * time.Millisecond
	}
	hub := live.NewHub(live.DaemonSource{Client: dc, Log: quiet}, o)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { hub.Run(ctx); close(done) }()
	t.Cleanup(func() {
		cancel()
		select {
		case <-done:
		case <-time.After(5 * time.Second):
			t.Error("the hub did not stop")
		}
	})
	return &env{t: t, d: d, hub: hub}
}

// until waits for cond, failing the test after 5 seconds.
func until(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for !cond() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(5 * time.Millisecond)
	}
}

func next(t *testing.T, s *live.Sub) live.Event {
	t.Helper()
	select {
	case e := <-s.Events():
		return e
	case <-time.After(5 * time.Second):
		t.Fatal("no event")
		return live.Event{}
	}
}

func noEvent(t *testing.T, s *live.Sub) {
	t.Helper()
	select {
	case e := <-s.Events():
		t.Fatalf("unexpected event %+v", e)
	case <-time.After(50 * time.Millisecond):
	}
}

func msg(seq uint64, chat int64) live.Event {
	return live.Event{Seq: seq, Kind: live.KindMessageNew, ChatID: chat, MessageID: int64(seq)}
}

// connect accepts the hub's stream and says hello at seq.
func (e *env) connect(want string, hello uint64) *livetest.Conn {
	e.t.Helper()
	c := e.d.Accept(e.t)
	if c.Since != want {
		e.t.Fatalf("since=%q, want %q", c.Since, want)
	}
	c.Send(live.Event{Seq: hello, Kind: live.KindHello})
	until(e.t, "the hub to be up", e.hub.Up)
	return c
}

func subscribe(t *testing.T, h *live.Hub, last uint64, m live.Match) (*live.Sub, live.Start) {
	t.Helper()
	s, st, err := h.Subscribe(last, m)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(s.Close)
	return s, st
}

func TestHubDownUntilHello(t *testing.T) {
	t.Parallel()
	e := start(t, live.Options{})
	if _, _, err := e.hub.Subscribe(0, all); !errors.Is(err, live.ErrDown) {
		t.Fatalf("before the hello: %v, want ErrDown", err)
	}
	e.connect("", 100)
	s, st := subscribe(t, e.hub, 0, all)
	if st.Kind != live.KindHello || st.Seq != 100 || len(st.Replay) != 0 {
		t.Fatalf("start %+v", st)
	}
	_ = s
}

func TestHubFanOutAndMatch(t *testing.T) {
	t.Parallel()
	e := start(t, live.Options{})
	c := e.connect("", 100)
	a, _ := subscribe(t, e.hub, 0, all)
	only := func(ev live.Event) (live.Event, bool) { return ev, ev.ChatID == -1 }
	b, _ := subscribe(t, e.hub, 0, only)

	c.Keepalive()
	c.Send(msg(101, -1))
	c.Send(msg(102, -2))
	c.Send(live.Event{Seq: 103, Kind: "something.new", ChatID: -1})
	c.Send(msg(104, -1))
	for _, want := range []uint64{101, 102, 104} {
		if got := next(t, a).Seq; got != want {
			t.Fatalf("a got %d, want %d", got, want)
		}
	}
	for _, want := range []uint64{101, 104} {
		if got := next(t, b).Seq; got != want {
			t.Fatalf("b got %d, want %d", got, want)
		}
	}
	noEvent(t, a)
	noEvent(t, b)
}

func TestHubResumeFromHistory(t *testing.T) {
	t.Parallel()
	e := start(t, live.Options{History: 3})
	c := e.connect("", 100)
	watch, _ := subscribe(t, e.hub, 0, all)
	for seq := uint64(101); seq <= 105; seq++ {
		c.Send(msg(seq, -1))
	}
	for seq := uint64(101); seq <= 105; seq++ {
		next(t, watch)
	}

	// 103, 104 and 105 are kept: a client at 102 or later resumes.
	_, st := subscribe(t, e.hub, 103, all)
	if st.Kind != live.KindHello || st.Seq != 105 || len(st.Replay) != 2 ||
		st.Replay[0].Seq != 104 || st.Replay[1].Seq != 105 {
		t.Fatalf("resume at 103: %+v", st)
	}
	_, st = subscribe(t, e.hub, 102, all)
	if st.Kind != live.KindHello || len(st.Replay) != 3 {
		t.Fatalf("resume at 102: %+v", st)
	}
	_, st = subscribe(t, e.hub, 105, all)
	if st.Kind != live.KindHello || len(st.Replay) != 0 {
		t.Fatalf("resume at 105: %+v", st)
	}
	// Too old, and from another history.
	for _, last := range []uint64{101, 99, 106, 9999} {
		_, st = subscribe(t, e.hub, last, all)
		if st.Kind != live.KindResync || st.Seq != 105 || len(st.Replay) != 0 {
			t.Fatalf("resume at %d: %+v", last, st)
		}
	}
}

func TestHubReconnectResumes(t *testing.T) {
	t.Parallel()
	e := start(t, live.Options{Grace: 5 * time.Second})
	c := e.connect("", 100)
	s, _ := subscribe(t, e.hub, 0, all)
	c.Send(msg(101, -1))
	c.Send(msg(102, -1))
	next(t, s)
	next(t, s)
	c.End()

	// The daemon still has everything: a hello, nothing lost.
	c = e.connect("102", 102)
	c.Send(msg(103, -1))
	if got := next(t, s); got.Seq != 103 || got.Kind != live.KindMessageNew {
		t.Fatalf("after resuming: %+v", got)
	}
	c.End()

	// The daemon restarted: a resync reaches the subscriber.
	c = e.d.Accept(t)
	if c.Since != "103" {
		t.Fatalf("since=%q", c.Since)
	}
	c.Send(live.Event{Seq: 5000, Kind: live.KindResync})
	if got := next(t, s); got.Kind != live.KindResync || got.Seq != 5000 {
		t.Fatalf("after the restart: %+v", got)
	}
	c.Send(msg(5001, -1))
	if got := next(t, s); got.Seq != 5001 {
		t.Fatalf("after the resync: %+v", got)
	}
	// A client from before the restart cannot resume.
	_, st := subscribe(t, e.hub, 103, all)
	if st.Kind != live.KindResync {
		t.Fatalf("old client: %+v", st)
	}
}

func TestHubResyncMidStream(t *testing.T) {
	t.Parallel()
	e := start(t, live.Options{})
	c := e.connect("", 100)
	s, _ := subscribe(t, e.hub, 0, all)
	c.Send(live.Event{Seq: 200, Kind: live.KindResync})
	if got := next(t, s); got.Kind != live.KindResync || got.Seq != 200 {
		t.Fatalf("%+v", got)
	}
}

func TestHubDaemonDown(t *testing.T) {
	t.Parallel()
	e := start(t, live.Options{Grace: 100 * time.Millisecond})
	c := e.connect("", 100)
	s, _ := subscribe(t, e.hub, 0, all)
	e.d.Refuse(http.StatusServiceUnavailable)
	c.End()

	until(t, "the hub to be down", func() bool { return !e.hub.Up() })
	if _, _, err := e.hub.Subscribe(0, all); !errors.Is(err, live.ErrDown) {
		t.Fatalf("while down: %v", err)
	}
	// Past the grace period the open streams end, so their clients
	// poll instead of waiting on a silent stream.
	select {
	case <-s.Done():
	case <-time.After(5 * time.Second):
		t.Fatal("the subscription was not ended")
	}
	if n := e.hub.Subscribers(); n != 0 {
		t.Fatalf("%d subscribers left", n)
	}

	e.d.Refuse(0)
	c = e.connect("100", 100)
	s2, _ := subscribe(t, e.hub, 100, all)
	c.Send(msg(101, -1))
	if got := next(t, s2); got.Seq != 101 {
		t.Fatalf("%+v", got)
	}
}

func TestHubGraceKeepsSubscribers(t *testing.T) {
	t.Parallel()
	e := start(t, live.Options{Grace: time.Hour})
	c := e.connect("", 100)
	s, _ := subscribe(t, e.hub, 0, all)
	c.End()
	c = e.connect("100", 100)
	c.Send(msg(101, -1))
	if got := next(t, s); got.Seq != 101 {
		t.Fatalf("%+v", got)
	}
	select {
	case <-s.Done():
		t.Fatal("ended within the grace period")
	default:
	}
}

func TestHubSlowSubscriber(t *testing.T) {
	t.Parallel()
	e := start(t, live.Options{Buffer: 2})
	c := e.connect("", 100)
	slow, _ := subscribe(t, e.hub, 0, all)
	fast, _ := subscribe(t, e.hub, 0, all)
	for seq := uint64(101); seq <= 110; seq++ {
		c.Send(msg(seq, -1))
		if got := next(t, fast); got.Seq != seq {
			t.Fatalf("fast got %d, want %d", got.Seq, seq)
		}
	}
	select {
	case <-slow.Lost():
	case <-time.After(5 * time.Second):
		t.Fatal("the slow subscriber was not told it fell behind")
	}
	if r := slow.Resync(); r.Kind != live.KindResync || r.Seq != 110 {
		t.Fatalf("resync %+v", r)
	}
	noEvent(t, slow)
	c.Send(msg(111, -1))
	if got := next(t, slow); got.Seq != 111 {
		t.Fatalf("after the resync: %+v", got)
	}
}

func TestHubSubscriberCap(t *testing.T) {
	t.Parallel()
	e := start(t, live.Options{MaxSubscribers: 2})
	e.connect("", 100)
	a, _ := subscribe(t, e.hub, 0, all)
	subscribe(t, e.hub, 0, all)
	if _, _, err := e.hub.Subscribe(0, all); !errors.Is(err, live.ErrFull) {
		t.Fatalf("third: %v, want ErrFull", err)
	}
	a.Close()
	a.Close()
	if n := e.hub.Subscribers(); n != 1 {
		t.Fatalf("%d subscribers", n)
	}
	subscribe(t, e.hub, 0, all)

	// The service's own subscribers are not capped, and closing one
	// frees no client slot.
	in, _, err := e.hub.SubscribeInternal(0, all)
	if err != nil {
		t.Fatalf("internal with the cap reached: %v", err)
	}
	in.Close()
	if _, _, err := e.hub.Subscribe(0, all); !errors.Is(err, live.ErrFull) {
		t.Fatalf("after an internal one closed: %v, want ErrFull", err)
	}
}

func TestHubStopEndsSubscribers(t *testing.T) {
	t.Parallel()
	d := livetest.New(t, testKey)
	dc, _ := daemon.New(d.URL, testKey, time.Second)
	hub := live.NewHub(live.DaemonSource{Client: dc, Log: quiet}, live.Options{Log: quiet})
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { hub.Run(ctx); close(done) }()
	d.Accept(t).Send(live.Event{Seq: 1, Kind: live.KindHello})
	until(t, "up", hub.Up)
	s, _, err := hub.Subscribe(0, all)
	if err != nil {
		t.Fatal(err)
	}
	cancel()
	<-done
	select {
	case <-s.Done():
	default:
		t.Fatal("still open after the hub stopped")
	}
	if hub.Up() {
		t.Fatal("up after stopping")
	}
}

func TestDaemonSourceFraming(t *testing.T) {
	t.Parallel()
	d := livetest.New(t, testKey)
	dc, _ := daemon.New(d.URL, testKey, time.Second)
	src := live.DaemonSource{Client: dc, Log: quiet}
	got := make(chan live.Event, 8)
	errc := make(chan error, 1)
	go func() {
		errc <- src.Stream(context.Background(), 7, func(e live.Event) { got <- e })
	}()
	c := d.Accept(t)
	if c.Since != "7" {
		t.Fatalf("since=%q", c.Since)
	}
	c.Raw("id: 7\ndata: {\"seq\":7,\"kind\":\"hello\"}\n\n")
	c.Raw(": keepalive\n\n")
	c.Raw("id: 8\r\ndata:{\"seq\":8,\"kind\":\"stats.changed\",\r\ndata: \"chat_ids\":[-5],\"user_ids\":[3],\"more\":true}\r\n\r\n")
	c.Raw("data: not json\n\n")
	c.Raw("data: {\"seq\":9,\"kind\":\"user.updated\",\"user_id\":3,\"fields\":[\"photo\"]}\n\n")
	c.End()
	if err := <-errc; err == nil {
		t.Fatal("no error at the end of the stream")
	}
	close(got)
	var seen []string
	for e := range got {
		seen = append(seen, e.Kind)
		if e.Kind == live.KindStatsChanged && (len(e.ChatIDs) != 1 || e.ChatIDs[0] != -5 ||
			len(e.UserIDs) != 1 || !e.More) {
			t.Fatalf("stats %+v", e)
		}
		if e.Kind == live.KindUserUpdated && (e.UserID != 3 || len(e.Fields) != 1) {
			t.Fatalf("user %+v", e)
		}
	}
	if strings.Join(seen, ",") != "hello,stats.changed,user.updated" {
		t.Fatalf("events %v", seen)
	}
}

func TestDaemonSourceIdle(t *testing.T) {
	t.Parallel()
	d := livetest.New(t, testKey)
	dc, _ := daemon.New(d.URL, testKey, time.Second)
	src := live.DaemonSource{Client: dc, Log: quiet, Idle: 100 * time.Millisecond}
	errc := make(chan error, 1)
	go func() { errc <- src.Stream(context.Background(), 0, func(live.Event) {}) }()
	d.Accept(t)
	select {
	case err := <-errc:
		if err == nil {
			t.Fatal("no error")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("a silent stream was not given up")
	}
}

func TestDaemonSourceRefused(t *testing.T) {
	t.Parallel()
	d := livetest.New(t, testKey)
	d.Refuse(http.StatusServiceUnavailable)
	dc, _ := daemon.New(d.URL, testKey, time.Second)
	err := live.DaemonSource{Client: dc}.Stream(context.Background(), 0, func(live.Event) {})
	var se *daemon.StatusError
	if !errors.As(err, &se) || se.Status != 503 || se.RetryAfter != "1" {
		t.Fatalf("%v", err)
	}
	if err := (live.DaemonSource{}).Stream(context.Background(), 0, func(live.Event) {}); !errors.Is(err, daemon.ErrOff) {
		t.Fatalf("off: %v", err)
	}
}

func TestEventJSON(t *testing.T) {
	t.Parallel()
	for _, c := range []struct {
		e    live.Event
		want string
	}{
		{msg(5, -100), `{"seq":5,"kind":"message.new","chat_id":-100,"message_id":5}`},
		{live.Event{Seq: 6, Kind: live.KindStatsChanged}, `{"seq":6,"kind":"stats.changed","chat_ids":[],"user_ids":[]}`},
		{live.Event{Kind: live.KindSession, Reason: live.ReasonRevoked}, `{"kind":"session","reason":"revoked"}`},
		{live.Event{Seq: 7, Kind: live.KindFileStored, FileID: 9, ChatID: -1, MessageID: 3}, `{"seq":7,"kind":"file.stored","chat_id":-1,"message_id":3,"file_id":9}`},
	} {
		b, err := c.e.MarshalJSON()
		if err != nil || string(b) != c.want {
			t.Errorf("%+v: %s, want %s (%v)", c.e, b, c.want, err)
		}
	}
}
