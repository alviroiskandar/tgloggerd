// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"context"
	"maps"
	"math/rand/v2"
	"net/http"
	"net/netip"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"tgloggerd/webapi/internal/auth"
	"tgloggerd/webapi/internal/live"
	"tgloggerd/webapi/internal/logdb"
)

// Live chat events for signed-out viewers of exposed groups
// (docs/design/decisions/2026-11-21-chat-live-public.md): GET
// /api/events without a session follows a few chat:<id> topics of
// exposed groups. Exposure is read from a snapshot, so the hub's match
// stays a memory lookup; the visitors' streams have caps of their own;
// their frames carry no seq and their events no file_id.

// PublicLiveConfig limits the signed-out streams (WEBAPI_LIVE_PUBLIC_*).
// A zero field takes its default; a negative Max turns signed-out
// streams off and a negative Cache the shared window cache.
type PublicLiveConfig struct {
	// Max is the signed-out streams open at once, in all; PerAddr from
	// one client address (an IPv4 address or an IPv6 /64);
	// OpensPerMinute the streams one address may open in a minute.
	// Defaults 200, 4 and 10.
	Max, PerAddr, OpensPerMinute int
	// Topics is the chat topics one stream may follow (default 4);
	// Buffer the events that may wait for one (default 32).
	Topics, Buffer int
	// Refetch is the least time a client leaves between two refetches
	// of a chat window, sent in the stream's first frame (default 5
	// s); Cache how old a signed-out chat window may be (default 2 s);
	// Exposure how often the exposure snapshot is read again while a
	// signed-out stream is open (default 10 s).
	Refetch, Cache, Exposure time.Duration
}

const (
	defaultPublicMax      = 200
	defaultPublicPerAddr  = 4
	defaultPublicOpens    = 10
	defaultPublicTopics   = 4
	defaultPublicBuffer   = 32
	defaultPublicRefetch  = 5 * time.Second
	defaultPublicCache    = 2 * time.Second
	defaultPublicExposure = 10 * time.Second
	// publicRetryMin and publicRetrySpan: a signed-out stream asks for
	// a reconnection delay of 5 to 15 s, a different one per stream,
	// so the visitors do not all come back at the same moment after
	// the daemon's stream dropped them together.
	publicRetryMin  = 5000
	publicRetrySpan = 10000
	// publicCapRetry is the Retry-After of a cap that is full.
	publicCapRetry = 30
	// exposureTimeout bounds one read of the exposure snapshot.
	exposureTimeout = 5 * time.Second
)

func (c PublicLiveConfig) withDefaults() PublicLiveConfig {
	def := func(v *int, d int) {
		if *v == 0 {
			*v = d
		}
	}
	defDur := func(v *time.Duration, d time.Duration) {
		if *v == 0 {
			*v = d
		}
	}
	def(&c.Max, defaultPublicMax)
	def(&c.PerAddr, defaultPublicPerAddr)
	def(&c.OpensPerMinute, defaultPublicOpens)
	def(&c.Topics, defaultPublicTopics)
	def(&c.Buffer, defaultPublicBuffer)
	defDur(&c.Refetch, defaultPublicRefetch)
	defDur(&c.Cache, defaultPublicCache)
	defDur(&c.Exposure, defaultPublicExposure)
	return c
}

// publicLive is the state of the signed-out streams and windows,
// made on first use (Server.public), so a Server built without New
// has it too.
type publicLive struct {
	cfg    PublicLiveConfig
	expo   *exposure
	caps   *visitorCaps
	window *windowCache
}

func (s *Server) public() *publicLive {
	s.publicOnce.Do(func() {
		cfg := s.liveCfg.Public.withDefaults()
		s.publicState = &publicLive{
			cfg:    cfg,
			expo:   newExposure(s, cfg.Exposure),
			caps:   newVisitorCaps(cfg),
			window: newWindowCache(cfg.Cache),
		}
		if s.clock != nil {
			s.publicState.window.now = s.clock
		}
	})
	return s.publicState
}

// exposedSet is one reading of telegram_public_groups. changed is
// closed when a newer reading that differs replaces it.
type exposedSet struct {
	ids     map[int64]bool
	changed chan struct{}
}

// exposure is the snapshot of the exposed groups the signed-out
// streams match against. It is read while at least one such stream is
// open: when the first opens, every interval after that, and at once
// when the MCP exposure pages change it (refresh). A reading that
// fails keeps the previous one.
type exposure struct {
	s        *Server
	interval time.Duration
	load     func(ctx context.Context) (map[int64]bool, error)

	cur atomic.Pointer[exposedSet]

	mu    sync.Mutex
	users int
	stop  chan struct{}
}

func newExposure(s *Server, interval time.Duration) *exposure {
	x := &exposure{s: s, interval: interval}
	x.load = func(ctx context.Context) (map[int64]bool, error) {
		ids, err := logdb.AllExposedGroupIDs(ctx, s.ro)
		if err != nil {
			return nil, err
		}
		m := make(map[int64]bool, len(ids))
		for _, id := range ids {
			m[id] = true
		}
		return m, nil
	}
	return x
}

// read loads the snapshot and swaps it in when it differs. Called with
// mu held.
func (x *exposure) read(ctx context.Context) error {
	ctx, cancel := context.WithTimeout(ctx, exposureTimeout)
	defer cancel()
	ids, err := x.load(ctx)
	if err != nil {
		return err
	}
	old := x.cur.Load()
	if old != nil && maps.Equal(old.ids, ids) {
		return nil
	}
	x.cur.Store(&exposedSet{ids: ids, changed: make(chan struct{})})
	if old != nil {
		close(old.changed)
	}
	return nil
}

// acquire is called by a signed-out stream before it checks its
// topics; release when it ends. The first one reads the snapshot and
// starts the refresher, the last one stops it.
func (x *exposure) acquire(ctx context.Context) error {
	x.mu.Lock()
	defer x.mu.Unlock()
	if x.users == 0 {
		if err := x.read(ctx); err != nil {
			return err
		}
		x.stop = make(chan struct{})
		go x.refresher(x.stop)
	}
	x.users++
	return nil
}

func (x *exposure) release() {
	x.mu.Lock()
	defer x.mu.Unlock()
	x.users--
	if x.users == 0 {
		close(x.stop)
	}
}

func (x *exposure) refresher(stop chan struct{}) {
	t := time.NewTicker(x.interval)
	defer t.Stop()
	for {
		select {
		case <-stop:
			return
		case <-t.C:
		}
		x.mu.Lock()
		if x.users > 0 {
			if err := x.read(context.Background()); err != nil {
				x.s.log.Warn("live: exposure snapshot", "err", err)
			}
		}
		x.mu.Unlock()
	}
}

// refresh reads the snapshot again at once, after the exposure was
// changed here; nothing when no signed-out stream is open, as the
// first one reads it anyway.
func (x *exposure) refresh(ctx context.Context) {
	x.mu.Lock()
	defer x.mu.Unlock()
	if x.users == 0 {
		return
	}
	if err := x.read(context.WithoutCancel(ctx)); err != nil {
		x.s.log.Warn("live: exposure snapshot", "err", err)
	}
}

// set is the current snapshot; nil before the first reading.
func (x *exposure) set() *exposedSet { return x.cur.Load() }

func (x *exposure) has(id int64) bool {
	set := x.cur.Load()
	return set != nil && set.ids[id]
}

// visitorCaps holds the signed-out streams' caps: in all, per client
// address, and opens per address per minute.
type visitorCaps struct {
	max, perAddr int
	opens        *auth.RateLimiter

	mu     sync.Mutex
	total  int
	byAddr map[string]int
}

func newVisitorCaps(c PublicLiveConfig) *visitorCaps {
	return &visitorCaps{max: c.Max, perAddr: c.PerAddr,
		opens: auth.NewRateLimiter(c.OpensPerMinute, time.Minute), byAddr: map[string]int{}}
}

// addrKey is the key a client address is capped by: an IPv4 address
// as it is, an IPv6 address by its /64, since one host holds a whole
// prefix.
func addrKey(ip string) string {
	a, err := netip.ParseAddr(ip)
	if err != nil {
		return ip
	}
	a = a.Unmap()
	if a.Is4() {
		return a.String()
	}
	p, err := a.Prefix(64)
	if err != nil {
		return a.String()
	}
	return p.String()
}

// take counts an open from addr and takes a slot for its stream. It
// answers the release of the slot, or the Retry-After when refused.
func (c *visitorCaps) take(addr string) (release func(), retry int, ok bool) {
	key := addrKey(addr)
	// The limiter counts what it is told are failures; here every
	// open counts, refused or not, so a client in a reconnect loop is
	// slowed down too.
	if !c.opens.Allowed(key) {
		return nil, 60, false
	}
	c.opens.RecordFailure(key)
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.total >= c.max || c.byAddr[key] >= c.perAddr {
		return nil, publicCapRetry, false
	}
	c.total++
	c.byAddr[key]++
	var once sync.Once
	return func() {
		once.Do(func() {
			c.mu.Lock()
			defer c.mu.Unlock()
			c.total--
			if c.byAddr[key]--; c.byAddr[key] <= 0 {
				delete(c.byAddr, key)
			}
		})
	}, 0, true
}

// publicTopics reports whether t is what a signed-out stream may ask
// for: only chat:<id> topics of groups, at most n of them. Whether the
// groups are exposed is checked against the snapshot afterwards.
func publicTopics(t topics, n int) bool {
	if t.messages || t.stats || t.bots || len(t.users) > 0 || len(t.groups) > 0 ||
		len(t.chats) == 0 || len(t.chats) > n {
		return false
	}
	for id := range t.chats {
		if id >= 0 {
			return false
		}
	}
	return true
}

// publicEvent is what a signed-out stream gets of an event that
// passed its match: no seq (it cannot resume, and the seqs would tell
// it the event rate of the whole archive) and no file_id (a row id
// over every chat's files).
func publicEvent(e live.Event) live.Event {
	e.Seq = 0
	e.FileID = 0
	return e
}

// publicEvents serves GET /api/events without a session.
func (s *Server) publicEvents(w http.ResponseWriter, r *http.Request, t topics) {
	unauth := func() { writeError(w, http.StatusUnauthorized, "unauthenticated", nil) }
	p := s.public()
	if p.cfg.Max < 0 || !publicTopics(t, p.cfg.Topics) {
		unauth()
		return
	}
	if s.live == nil {
		liveOff(w, "Live updates are off.")
		return
	}
	if err := p.expo.acquire(r.Context()); err != nil {
		s.internalError(w, "live exposure", err)
		return
	}
	defer p.expo.release()
	for id := range t.chats {
		if !p.expo.has(id) {
			unauth()
			return
		}
	}
	release, retry, ok := p.caps.take(clientIP(r))
	if !ok {
		w.Header().Set("Retry-After", strconv.Itoa(retry))
		writeError(w, http.StatusServiceUnavailable, "unavailable",
			map[string]any{"message": "Too many live connections."})
		return
	}
	defer release()

	sub, _, err := s.live.SubscribeWith(0, func(e live.Event) (live.Event, bool) {
		switch e.Kind {
		case live.KindMessageNew, live.KindMessageEdited, live.KindMessageDeleted,
			live.KindFileStored:
		default:
			return e, false
		}
		if !t.wants(e) || !p.expo.has(e.ChatID) {
			return e, false
		}
		return publicEvent(e), true
	}, live.SubOptions{Buffer: p.cfg.Buffer, Uncapped: true})
	if err != nil {
		liveOff(w, "Live updates are not available right now.")
		return
	}
	defer sub.Close()

	// Every stream starts as a resync: the client refetches its window
	// (from the shared cache), and learns how often it may.
	first := live.Event{Kind: live.KindResync,
		RefetchInterval: int(max(p.cfg.Refetch/time.Second, 1))}
	set := p.expo.set()
	s.serveStream(w, r, sub, streamOpts{
		retry:  publicRetryMin + rand.IntN(publicRetrySpan),
		first:  []live.Event{first},
		resync: publicEvent,
		wake: func() <-chan struct{} {
			return set.changed
		},
		check: func() string {
			// A newer snapshot: the stream ends when one of its
			// groups was withdrawn. Never revoked, which would send
			// a visitor to the sign-in page.
			set = p.expo.set()
			for id := range t.chats {
				if !set.ids[id] {
					return live.ReasonScopeChanged
				}
			}
			return ""
		},
	})
}
