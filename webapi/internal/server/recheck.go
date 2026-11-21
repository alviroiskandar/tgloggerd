// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"context"
	"log/slog"
	"sync"
	"time"

	"tgloggerd/webapi/internal/daemon"
	"tgloggerd/webapi/internal/logdb"
)

// The logger learns of messages from TDLib's updates and a one-time
// history walk. After a restart Telegram may answer a busy chat's
// catch-up with "too much changed" instead of the missed messages, and
// edits and deletions made meanwhile are lost the same way. So when a
// chat window, a chat page or a message page is served, the daemon is
// asked to read that stretch from Telegram again (daemon.Client.Recheck,
// POST /v1/chats/{id}/recheck); what it finds arrives as the usual live
// events, so an open page updates by itself.
//
// The ask runs in the background, never in the answer's path, and its
// outcome is only logged at debug. Every ask costs the account
// Telegram requests, so this side limits them before the daemon's own
// limits:
//
//   - per stretch: the newest messages of a chat (around 0) at most once
//     per RecheckConfig.Newest, the stretch around one anchor (around
//     its message id) at most once per Older, whoever asks;
//   - signed-out visitors, who may be anyone in any number (an exposed
//     group's public archive, its crawlers), all draw from one token
//     bucket of PublicBurst tokens refilled at PublicPerMinute; an
//     older stretch takes a token only while more than PublicReserve
//     are left, so a burst keeps the newest messages fresh first;
//   - at most MaxInFlight asks wait for the daemon at once; one more is
//     dropped without counting against its stretch.
//
// The times are kept in memory, at most recheckKeep stretches: a
// restart may ask once more, and when that many are waiting out their
// interval a stretch not among them is not asked for.

// RecheckConfig is how chat rechecks are limited (WEBAPI_RECHECK_*);
// the zero value is DefaultRecheck.
type RecheckConfig struct {
	// Off asks for no rechecks at all.
	Off bool
	// Newest and Older are the least time between two asks for a
	// chat's newest messages and for the stretch around one anchor.
	Newest, Older time.Duration
	// PublicPerMinute refills the signed-out visitors' bucket, which
	// holds at most PublicBurst; older stretches leave PublicReserve
	// tokens to the newest ones. PublicPerMinute 0: signed-out
	// visitors ask for none.
	PublicPerMinute, PublicBurst, PublicReserve int
	// Timeout bounds one ask; the daemon answers at once.
	Timeout time.Duration
	// MaxInFlight is how many asks may wait for the daemon at once.
	MaxInFlight int
}

// DefaultRecheck are the limits without WEBAPI_RECHECK_* settings.
var DefaultRecheck = RecheckConfig{
	Newest:          time.Minute,
	Older:           time.Hour,
	PublicPerMinute: 20,
	PublicBurst:     20,
	PublicReserve:   10,
	Timeout:         5 * time.Second,
	MaxInFlight:     8,
}

// recheckKeep is how many stretches the rechecker remembers.
const recheckKeep = 20000

// recheckKey is a stretch: a chat (a group's id, or the other user's
// for a private chat) and the message id it lies around, 0 for the
// newest messages. The daemon keys its own limits the same way.
type recheckKey struct {
	chat, around int64
}

type rechecker struct {
	log   *slog.Logger
	dc    *daemon.Client
	clock func() time.Time
	cfg   RecheckConfig

	mu       sync.Mutex
	next     map[recheckKey]time.Time // no ask before
	tokens   float64                  // the public bucket
	refilled time.Time                // when tokens was last brought up to date
	inFlight int
	wg       sync.WaitGroup
}

func newRechecker(log *slog.Logger, dc *daemon.Client, clock func() time.Time, cfg RecheckConfig) *rechecker {
	if cfg == (RecheckConfig{}) {
		cfg = DefaultRecheck
	}
	return &rechecker{log: log, dc: dc, clock: clock, cfg: cfg,
		next: map[recheckKey]time.Time{}, tokens: float64(cfg.PublicBurst), refilled: clock()}
}

// window asks for the stretch of a chat window or chat page: with no
// anchor its newest messages, else the stretch around the anchor's
// message id, so the same page always names the same stretch. The
// oldest messages (after=0) are named by the window's first message;
// an empty window there names nothing. A window after an anchor that
// reaches the newest message names the newest messages: a live chat
// fetches its tail after its last message on every new batch, and
// naming each of those anchors would ask for a new stretch every time
// instead of once per Newest.
func (rc *rechecker) window(chat int64, a logdb.Anchor, w logdb.Window, signedIn bool) {
	var around int64
	switch {
	case a.Before != nil:
		around = *a.Before
	case a.After != nil && !w.HasNewer:
		rc.ask(chat, 0, signedIn)
		return
	case a.After != nil:
		around = *a.After
	case a.Around != nil:
		around = *a.Around
	default:
		rc.ask(chat, 0, signedIn)
		return
	}
	if around <= 0 {
		if len(w.Messages) == 0 {
			return
		}
		around = w.Messages[0].MessageID
	}
	rc.ask(chat, around, signedIn)
}

// ask asks for one stretch in the background if the limits allow it,
// and reports whether it did. It returns at once. A nil rechecker, one
// turned off or without the daemon API asks nothing.
func (rc *rechecker) ask(chat, around int64, signedIn bool) bool {
	if rc == nil || rc.cfg.Off || !rc.dc.Enabled() || chat == 0 || around < 0 {
		return false
	}
	k := recheckKey{chat, around}
	every := rc.cfg.Newest
	if around != 0 {
		every = rc.cfg.Older
	}
	now := rc.clock()
	rc.mu.Lock()
	defer rc.mu.Unlock()
	if now.Before(rc.next[k]) || rc.inFlight >= rc.cfg.MaxInFlight {
		return false
	}
	if _, known := rc.next[k]; !known && len(rc.next) >= recheckKeep {
		for o, t := range rc.next {
			if !now.Before(t) {
				delete(rc.next, o)
			}
		}
		if len(rc.next) >= recheckKeep {
			return false
		}
	}
	if !signedIn && !rc.takePublic(now, around == 0) {
		return false
	}
	rc.next[k] = now.Add(every)
	rc.inFlight++
	rc.wg.Add(1)
	go rc.run(k)
	return true
}

// takePublic takes a token of the signed-out visitors' bucket, after
// refilling it for the time gone by; an older stretch (newest false)
// only while more than the reserve would be left. Called with mu held.
func (rc *rechecker) takePublic(now time.Time, newest bool) bool {
	c := rc.cfg
	if c.PublicPerMinute <= 0 || c.PublicBurst <= 0 {
		return false
	}
	if d := now.Sub(rc.refilled); d > 0 {
		rc.tokens = min(float64(c.PublicBurst),
			rc.tokens+d.Minutes()*float64(c.PublicPerMinute))
		rc.refilled = now
	}
	floor := 1.0
	if !newest {
		floor += float64(c.PublicReserve)
	}
	if rc.tokens < floor {
		return false
	}
	rc.tokens--
	return true
}

// run is one ask; its own timeout bounds it, as the request that
// started it is long answered.
func (rc *rechecker) run(k recheckKey) {
	defer rc.wg.Done()
	ctx, cancel := context.WithTimeout(context.Background(), rc.cfg.Timeout)
	res, reply, ok := rc.dc.Recheck(ctx, k.chat, k.around, rc.cfg.Timeout)
	cancel()
	rc.mu.Lock()
	rc.inFlight--
	rc.mu.Unlock()
	switch {
	case !ok:
		rc.log.Debug("chat recheck: no answer", "chat", k.chat, "around", k.around,
			"status", reply.Status, "err", reply.Error)
	case !res.Queued:
		rc.log.Debug("chat recheck: not queued", "chat", k.chat, "around", k.around,
			"reason", res.Reason)
	default:
		rc.log.Debug("chat recheck: queued", "chat", k.chat, "around", k.around)
	}
}

// wait waits for the asks in flight, for tests. Call it only while no
// ask can start: sync.WaitGroup forbids an Add from zero that overlaps
// a Wait, and the race detector reports one.
func (rc *rechecker) wait() {
	rc.wg.Wait()
}
