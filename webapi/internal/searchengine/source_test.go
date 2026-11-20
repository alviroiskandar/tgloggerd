// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package searchengine

import (
	"context"
	"io"
	"log/slog"
	"testing"
	"time"

	"tgloggerd/webapi/internal/daemon"
	"tgloggerd/webapi/internal/live"
	"tgloggerd/webapi/internal/live/livetest"
)

// An indexer reconnecting to a hub whose browser slots are all taken
// still subscribes: the cap is the browsers', not the service's.
func TestHubSourceIgnoresClientCap(t *testing.T) {
	t.Parallel()
	key := []byte("0123456789abcdef0123456789abcdef")
	quiet := slog.New(slog.NewTextHandler(io.Discard, nil))
	d := livetest.New(t, key)
	dc, err := daemon.New(d.URL, key, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	hub := live.NewHub(live.DaemonSource{Client: dc, Log: quiet}, live.Options{
		MaxSubscribers: 1, Log: quiet,
		RetryMin: 10 * time.Millisecond, RetryMax: 50 * time.Millisecond})
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { hub.Run(ctx); close(done) }()
	t.Cleanup(func() { cancel(); <-done })
	c := d.Accept(t)
	c.Send(live.Event{Seq: 100, Kind: live.KindHello})
	deadline := time.Now().Add(5 * time.Second)
	for !hub.Up() {
		if time.Now().After(deadline) {
			t.Fatal("the hub did not come up")
		}
		time.Sleep(5 * time.Millisecond)
	}

	// A browser takes the only slot.
	browser, _, err := hub.Subscribe(0, func(e live.Event) (live.Event, bool) { return e, true })
	if err != nil {
		t.Fatal(err)
	}
	defer browser.Close()

	got := make(chan live.Event, 8)
	sctx, scancel := context.WithCancel(context.Background())
	var serr error
	ended := make(chan struct{})
	go func() {
		serr = HubSource{Hub: hub}.Stream(sctx, 0, func(e live.Event) { got <- e })
		close(ended)
	}()
	defer func() { scancel(); <-ended }()
	next := func() live.Event {
		t.Helper()
		select {
		case e := <-got:
			return e
		case <-ended:
			t.Fatalf("the indexer's stream ended: %v", serr)
		case <-time.After(5 * time.Second):
			t.Fatal("no event for the indexer")
		}
		return live.Event{}
	}
	if e := next(); e.Kind != live.KindHello || e.Seq != 100 {
		t.Fatalf("first %+v", e)
	}
	c.Send(live.Event{Seq: 101, Kind: live.KindMessageNew, ChatID: -1, MessageID: 1})
	if e := next(); e.Seq != 101 || e.Kind != live.KindMessageNew {
		t.Fatalf("got %+v", e)
	}
	// The indexer took no browser slot.
	if _, _, err := hub.Subscribe(0, func(e live.Event) (live.Event, bool) { return e, true }); err != live.ErrFull {
		t.Fatalf("a second browser: %v, want ErrFull", err)
	}
	browser.Close()
	other, _, err := hub.Subscribe(0, func(e live.Event) (live.Event, bool) { return e, true })
	if err != nil {
		t.Fatalf("the freed slot: %v", err)
	}
	other.Close()
}
