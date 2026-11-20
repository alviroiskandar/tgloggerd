// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package live

import (
	"context"
	"errors"
	"log/slog"
	"sync"
	"time"
)

// Options are the Hub's limits. A zero field takes its default.
type Options struct {
	// History is how many recent events are kept for subscribers that
	// reconnect; one further behind gets a resync. Default 4096.
	History int
	// Buffer is how many events may wait for one subscriber; a
	// subscriber that falls further behind gets a resync instead of
	// more memory. Default 256.
	Buffer int
	// MaxSubscribers caps the open subscriptions of clients (Subscribe).
	// The service's own subscribers (SubscribeInternal) do not count
	// against it. Default 256.
	MaxSubscribers int
	// Grace is how long subscribers stay connected while the source
	// reconnects; if it is not back by then, their streams end and
	// they fall back to polling. Default 5 s.
	Grace time.Duration
	// RetryMin and RetryMax bound the wait between two connection
	// attempts, doubling from one to the other. Defaults 1 s and 30 s.
	RetryMin, RetryMax time.Duration
	Log                *slog.Logger
}

func (o *Options) defaults() {
	if o.History <= 0 {
		o.History = 4096
	}
	if o.Buffer <= 0 {
		o.Buffer = 256
	}
	if o.MaxSubscribers <= 0 {
		o.MaxSubscribers = 256
	}
	if o.Grace <= 0 {
		o.Grace = 5 * time.Second
	}
	if o.RetryMin <= 0 {
		o.RetryMin = time.Second
	}
	if o.RetryMax < o.RetryMin {
		o.RetryMax = 30 * time.Second
		if o.RetryMax < o.RetryMin {
			o.RetryMax = o.RetryMin
		}
	}
	if o.Log == nil {
		o.Log = slog.Default()
	}
}

// Errors of Subscribe.
var (
	// ErrDown: the source is not connected, or the Hub has stopped.
	ErrDown = errors.New("live updates are not available")
	// ErrFull: MaxSubscribers client subscriptions are open.
	ErrFull = errors.New("too many live subscribers")
)

// Match decides whether a subscriber gets an event, and may narrow it
// (a stats.changed's lists). It runs under the Hub's lock for every
// event, so it must be quick and must not block.
type Match func(Event) (Event, bool)

// Hub holds the one subscription to the Source and fans its events out.
type Hub struct {
	src Source
	o   Options

	mu sync.Mutex
	// up: the source is connected and has sent its first frame.
	up bool
	// started: a first frame was ever received, so hi is a seq of the
	// source's and reconnecting resumes from it.
	started bool
	// The events after known up to hi are all in ring (minus kinds the
	// Hub does not pass on), so a subscriber that saw up to a seq in
	// [known, hi] can resume without a gap.
	known, hi uint64
	ring      ring
	subs      map[*Sub]struct{}
	// clients counts the subscriptions in subs that are not internal:
	// the ones MaxSubscribers caps.
	clients int
	// ups counts the source's connections, so a grace timer knows
	// whether the source came back since it was armed.
	ups     uint64
	stopped bool
}

// NewHub makes a Hub reading from src; Run connects it.
func NewHub(src Source, o Options) *Hub {
	o.defaults()
	return &Hub{src: src, o: o, ring: newRing(o.History), subs: map[*Sub]struct{}{}}
}

// Run keeps the source connected until ctx ends, resuming from the last
// seq after every disconnect. When ctx ends every subscriber's stream
// ends too.
func (h *Hub) Run(ctx context.Context) {
	wait := h.o.RetryMin
	failing := false
	for {
		h.mu.Lock()
		since := uint64(0)
		if h.started {
			since = h.hi
		}
		h.mu.Unlock()

		err := h.src.Stream(ctx, since, h.receive)
		if ctx.Err() != nil {
			h.stop()
			return
		}
		if h.down() {
			// It was up: a fresh disconnect, retried at once.
			wait = h.o.RetryMin
			failing = false
			h.o.Log.Warn("live: daemon event stream lost", "err", err)
		} else if !failing {
			failing = true
			h.o.Log.Warn("live: daemon event stream unavailable", "err", err)
		}

		t := time.NewTimer(wait)
		select {
		case <-ctx.Done():
			t.Stop()
			h.stop()
			return
		case <-t.C:
		}
		wait = min(wait*2, h.o.RetryMax)
	}
}

// receive takes one event of the source.
func (h *Hub) receive(e Event) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.stopped {
		return
	}
	switch e.Kind {
	case KindHello:
		// A hello resumes from since, which is hi; any other seq starts
		// a new history.
		if !h.started || e.Seq != h.hi {
			h.restart(e.Seq)
		}
		h.markUp()
	case KindResync:
		h.restart(e.Seq)
		h.markUp()
	default:
		if !h.up || e.Seq <= h.hi {
			return
		}
		h.hi = e.Seq
		if !Known(e.Kind) {
			return
		}
		if old, ok := h.ring.push(e); ok {
			h.known = old.Seq
		}
		h.publish(e)
	}
}

func (h *Hub) markUp() {
	if !h.up {
		h.o.Log.Info("live: daemon event stream connected", "seq", h.hi)
		h.ups++
	}
	h.up = true
}

// restart starts a new history at seq. Subscribers that were following
// the old one missed what cannot be sent, so they get a resync.
func (h *Hub) restart(seq uint64) {
	had := h.started
	h.started = true
	h.known, h.hi = seq, seq
	h.ring.clear()
	if !had {
		return
	}
	r := Event{Seq: seq, Kind: KindResync}
	for s := range h.subs {
		s.send(r)
	}
}

// publish hands an event to every subscriber that matches it. Called
// with mu held.
func (h *Hub) publish(e Event) {
	for s := range h.subs {
		if ev, ok := s.match(e); ok {
			s.send(ev)
		}
	}
}

// down notes a disconnect and reports whether the source had been up.
// New subscribers are refused from now on; the open ones are kept for
// the grace period, in case the source comes back at once.
func (h *Hub) down() bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	was := h.up
	h.up = false
	if was {
		u := h.ups
		time.AfterFunc(h.o.Grace, func() {
			h.mu.Lock()
			defer h.mu.Unlock()
			if h.ups == u && !h.up {
				h.closeAll()
			}
		})
	}
	return was
}

func (h *Hub) stop() {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.stopped = true
	h.up = false
	h.closeAll()
}

func (h *Hub) closeAll() {
	for s := range h.subs {
		h.drop(s)
	}
}

// drop removes a subscription. Called with mu held, for one in subs.
func (h *Hub) drop(s *Sub) {
	delete(h.subs, s)
	if !s.internal {
		h.clients--
	}
	close(s.done)
}

// Up reports whether new subscribers are accepted.
func (h *Hub) Up() bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.up && !h.stopped
}

// Subscribers is the number of open subscriptions.
func (h *Hub) Subscribers() int {
	h.mu.Lock()
	defer h.mu.Unlock()
	return len(h.subs)
}

// Start is where a new subscription begins: Kind hello (nothing missed,
// Replay holds the matching events after the client's seq) or resync
// (the client's seq cannot be resumed), at Seq.
type Start struct {
	Kind   string
	Seq    uint64
	Replay []Event
}

// Subscribe opens a subscription for a client that last saw seq last
// (0: none). Events are matched by match. Close it when done.
func (h *Hub) Subscribe(last uint64, match Match) (*Sub, Start, error) {
	return h.subscribe(last, match, false)
}

// SubscribeInternal is Subscribe for the service's own subscribers
// (the search indexers): they do not count against MaxSubscribers, so
// a burst of browser streams cannot lock them out and leave them
// behind. Their number is fixed by the service, not by its clients.
func (h *Hub) SubscribeInternal(last uint64, match Match) (*Sub, Start, error) {
	return h.subscribe(last, match, true)
}

func (h *Hub) subscribe(last uint64, match Match, internal bool) (*Sub, Start, error) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if !h.up || h.stopped {
		return nil, Start{}, ErrDown
	}
	if !internal && h.clients >= h.o.MaxSubscribers {
		return nil, Start{}, ErrFull
	}
	st := Start{Kind: KindHello, Seq: h.hi}
	switch {
	case last == 0 || last == h.hi:
	case last >= h.known && last < h.hi:
		h.ring.each(func(e Event) {
			if e.Seq <= last {
				return
			}
			if ev, ok := match(e); ok {
				st.Replay = append(st.Replay, ev)
			}
		})
	default:
		st.Kind = KindResync
	}
	s := &Sub{
		h:        h,
		match:    match,
		internal: internal,
		ch:       make(chan Event, h.o.Buffer),
		lostc:    make(chan struct{}, 1),
		done:     make(chan struct{}),
	}
	h.subs[s] = struct{}{}
	if !internal {
		h.clients++
	}
	return s, st, nil
}

// Sub is one subscription.
type Sub struct {
	h        *Hub
	match    Match
	internal bool // opened by SubscribeInternal
	ch       chan Event
	// lost: an event did not fit in ch. Guarded by h.mu; lostc wakes
	// the reader.
	lost  bool
	lostc chan struct{}
	done  chan struct{}
}

// send queues an event without blocking. Called with h.mu held.
func (s *Sub) send(e Event) {
	if s.lost {
		return
	}
	select {
	case s.ch <- e:
	default:
		s.lost = true
		select {
		case s.lostc <- struct{}{}:
		default:
		}
	}
}

// Events delivers the subscription's events in order.
func (s *Sub) Events() <-chan Event { return s.ch }

// Lost fires when the subscriber fell behind; call Resync then.
func (s *Sub) Lost() <-chan struct{} { return s.lostc }

// Done is closed when the Hub ends the subscription: the source stayed
// away past the grace period, or the Hub stopped.
func (s *Sub) Done() <-chan struct{} { return s.done }

// Resync drops what is queued and returns the resync to send instead:
// the events after its seq come through Events again.
func (s *Sub) Resync() Event {
	s.h.mu.Lock()
	defer s.h.mu.Unlock()
	for {
		select {
		case <-s.ch:
			continue
		default:
		}
		break
	}
	s.lost = false
	return Event{Seq: s.h.hi, Kind: KindResync}
}

// Close ends the subscription and frees its slot.
func (s *Sub) Close() {
	s.h.mu.Lock()
	defer s.h.mu.Unlock()
	if _, ok := s.h.subs[s]; ok {
		s.h.drop(s)
	}
}

// ring keeps the most recent events, oldest first.
type ring struct {
	buf        []Event
	start, len int
}

func newRing(n int) ring { return ring{buf: make([]Event, n)} }

// push appends e and returns the event it pushed out, if any.
func (r *ring) push(e Event) (Event, bool) {
	if r.len < len(r.buf) {
		r.buf[(r.start+r.len)%len(r.buf)] = e
		r.len++
		return Event{}, false
	}
	old := r.buf[r.start]
	r.buf[r.start] = e
	r.start = (r.start + 1) % len(r.buf)
	return old, true
}

func (r *ring) clear() {
	clear(r.buf)
	r.start, r.len = 0, 0
}

func (r *ring) each(fn func(Event)) {
	for i := 0; i < r.len; i++ {
		fn(r.buf[(r.start+i)%len(r.buf)])
	}
}
