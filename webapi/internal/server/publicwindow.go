// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"bytes"
	"context"
	"net/http"
	"net/url"
	"sync"
	"sync/atomic"
	"time"
)

// The signed-out chat windows' shared cache. Every live event makes
// each visitor's page refetch its window, and visitors at the same
// place of a chat ask for the same answer: a signed-out window depends
// only on the chat and the query (viewFor(Anonymous, false); its file
// links are tokens that do not depend on the request). So one read
// answers every visitor asking for the same window within the cache's
// lifetime, and while it runs the others wait for it instead of
// starting their own.

// windowCacheMax bounds the windows kept; when it is reached and none
// has expired, answers are served without being kept.
const windowCacheMax = 4096

// windowParams are the query parameters a chat window depends on; the
// others are ignored by it, and so by the key.
var windowParams = []string{"before", "after", "around", "after_ts", "limit", "topic"}

// windowAnswer is an answer recorded once and written to many.
type windowAnswer struct {
	status int
	header http.Header
	body   []byte
}

type windowEntry struct {
	done    chan struct{} // closed when ans is set
	ans     windowAnswer
	expires time.Time
}

type windowCache struct {
	ttl time.Duration // <= 0: no cache
	now func() time.Time

	mu      sync.Mutex
	entries map[string]*windowEntry
	// reads counts the windows read for the cache, for the tests.
	reads atomic.Int64
}

func newWindowCache(ttl time.Duration) *windowCache {
	return &windowCache{ttl: ttl, now: time.Now, entries: map[string]*windowEntry{}}
}

// windowKey is the key of a chat window request.
func windowKey(kind, id string, q url.Values) string {
	k := url.Values{}
	for _, p := range windowParams {
		if v := q.Get(p); v != "" {
			k.Set(p, v)
		}
	}
	return kind + "/" + id + "?" + k.Encode()
}

// recorder takes an answer to keep.
type recorder struct {
	h      http.Header
	status int
	buf    bytes.Buffer
}

func (r *recorder) Header() http.Header { return r.h }
func (r *recorder) WriteHeader(code int) {
	if r.status == 0 {
		r.status = code
	}
}
func (r *recorder) Write(p []byte) (int, error) {
	if r.status == 0 {
		r.status = http.StatusOK
	}
	return r.buf.Write(p)
}

// serve answers r from the cache, or by fill, whose answer every
// request waiting for the same key gets; only a 200 is kept. fill runs
// detached from the request that started it, so a visitor going away
// does not fail the others' answer.
func (c *windowCache) serve(w http.ResponseWriter, r *http.Request, key string,
	fill func(w http.ResponseWriter, r *http.Request)) {
	if c.ttl <= 0 {
		fill(w, r)
		return
	}
	now := c.now()
	c.mu.Lock()
	e, ok := c.entries[key]
	if ok {
		select {
		case <-e.done:
			if !now.Before(e.expires) {
				ok = false
			}
		default:
		}
	}
	if !ok {
		if len(c.entries) >= windowCacheMax {
			c.sweep(now)
		}
		if len(c.entries) >= windowCacheMax {
			c.mu.Unlock()
			fill(w, r)
			return
		}
		e = &windowEntry{done: make(chan struct{})}
		c.entries[key] = e
		c.mu.Unlock()
		c.fill(e, key, r, fill)
	} else {
		c.mu.Unlock()
	}

	select {
	case <-e.done:
	case <-r.Context().Done():
		return
	}
	h := w.Header()
	for k, v := range e.ans.header {
		h[k] = v
	}
	w.WriteHeader(e.ans.status)
	_, _ = w.Write(e.ans.body)
}

func (c *windowCache) fill(e *windowEntry, key string, r *http.Request,
	fill func(w http.ResponseWriter, r *http.Request)) {
	c.reads.Add(1)
	rec := &recorder{h: http.Header{}}
	fill(rec, r.WithContext(context.WithoutCancel(r.Context())))
	if rec.status == 0 {
		rec.status = http.StatusOK
	}
	e.ans = windowAnswer{status: rec.status, header: rec.h, body: rec.buf.Bytes()}
	e.expires = c.now().Add(c.ttl)
	c.mu.Lock()
	if rec.status != http.StatusOK && c.entries[key] == e {
		delete(c.entries, key)
	}
	c.mu.Unlock()
	close(e.done)
}

// sweep drops the expired windows. Called with mu held.
func (c *windowCache) sweep(now time.Time) {
	for k, e := range c.entries {
		select {
		case <-e.done:
			if !now.Before(e.expires) {
				delete(c.entries, k)
			}
		default:
		}
	}
}
