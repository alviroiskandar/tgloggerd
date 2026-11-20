// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"maps"
	"net/http"
	"reflect"
	"strconv"
	"strings"
	"time"

	"tgloggerd/webapi/internal/auth"
	"tgloggerd/webapi/internal/live"
	"tgloggerd/webapi/internal/logdb"
	"tgloggerd/webapi/internal/scope"
)

// GET /api/events: the live updates of docs/design/web-live.md section
// 4, as Server-Sent Events, fanned out from the daemon's stream by a
// live.Hub. Each event passes the subscriber's topics and then the
// request scope, so a viewer never learns of activity in a chat hidden
// from them. The contract is in webapi/README and openapi.yaml.

// LiveConfig tunes GET /api/events. A zero field takes its default.
type LiveConfig struct {
	// Heartbeat is how often a comment line is sent on a quiet stream,
	// so nginx and the browser notice a dead connection. Default 15 s.
	Heartbeat time.Duration
	// Recheck is how often an open stream checks its session again: a
	// revoked or expired session, or a changed scope, ends the stream
	// with a session event. Default 30 s (WEBAPI_LIVE_RECHECK).
	Recheck time.Duration
	// WriteTimeout bounds each write of a frame or heartbeat: a client
	// that stops reading ends its stream then, instead of holding the
	// handler until the connection gives up. Default 30 s.
	WriteTimeout time.Duration
}

const (
	defaultHeartbeat = 15 * time.Second
	defaultRecheck   = 30 * time.Second
	defaultWriteTime = 30 * time.Second
	// liveRetry is the reconnection delay, in milliseconds, the stream
	// asks the browser's EventSource for.
	liveRetry = 3000
	// maxTopics bounds the topics of one stream. A list page subscribes
	// to every user and group on screen; this leaves room for long
	// pages while bounding the work per event.
	maxTopics = 2000
	// recheckTimeout bounds one session check.
	recheckTimeout = 5 * time.Second
)

// topics is what one stream asked for (web-live-client.md section 3).
type topics struct {
	messages, stats, bots bool
	chats, users          map[int64]struct{}
	groups                map[int64]struct{}
}

// parseTopics reads "chat:<id>,user:<id>,group:<id>,messages,stats,bots".
// Repeats count once.
func parseTopics(s string) (topics, error) {
	t := topics{chats: map[int64]struct{}{}, users: map[int64]struct{}{},
		groups: map[int64]struct{}{}}
	n := 0
	for _, f := range strings.Split(s, ",") {
		f = strings.TrimSpace(f)
		if f == "" {
			continue
		}
		n++
		if n > maxTopics {
			return t, fmt.Errorf("more than %d topics", maxTopics)
		}
		switch f {
		case "messages":
			t.messages = true
			continue
		case "stats":
			t.stats = true
			continue
		case "bots":
			t.bots = true
			continue
		}
		name, id, ok := strings.Cut(f, ":")
		var set map[int64]struct{}
		switch name {
		case "chat":
			set = t.chats
		case "user":
			set = t.users
		case "group":
			set = t.groups
		}
		v, err := strconv.ParseInt(id, 10, 64)
		if !ok || set == nil || err != nil || v == 0 {
			return t, fmt.Errorf("unknown topic %q", f)
		}
		set[v] = struct{}{}
	}
	if n == 0 {
		return t, errors.New("no topics")
	}
	return t, nil
}

func has(set map[int64]struct{}, id int64) bool {
	_, ok := set[id]
	return ok
}

// wants reports whether an event belongs to one of the topics, as
// topicsOf in web/app/src/live/events.ts maps them.
func (t *topics) wants(e live.Event) bool {
	switch e.Kind {
	case live.KindMessageNew, live.KindMessageEdited, live.KindMessageDeleted:
		return t.messages || has(t.chats, e.ChatID)
	case live.KindFileStored:
		// A user's or group's photo carries file_id alone; the
		// user.updated or group.updated beside it is what a page uses.
		return e.ChatID != 0 && (t.messages || has(t.chats, e.ChatID))
	case live.KindUserUpdated:
		return has(t.users, e.UserID)
	case live.KindGroupUpdated:
		return has(t.groups, e.ChatID)
	case live.KindStatsChanged:
		return t.stats
	case live.KindBotUpdated:
		return t.bots
	}
	return false
}

// eventInScope is the one place the request scope decides which live
// events a viewer gets; GET /api/events calls it for every event. A
// chat's events reach only viewers who see that chat (scope.SeesChat),
// a user's only those who see that user. They run for every event and
// subscriber under the hub's lock, so they answer from memory: users
// is the set of users the stream may hear of when the viewer does not
// see users in full (a member: liveUsers), nil for one that does.
// exposed is the stream's chat:<id> topics of exposed groups its scope
// does not see (liveExposed): their public chat is open to anyone, so
// their message and file events pass too. Nothing else is widened:
// `messages`, `stats`, `group:` and user topics stay as the scope has
// them.
func eventInScope(sc scope.Scope, e live.Event) (live.Event, bool) {
	return eventFor(sc, nil, nil, e)
}

func eventFor(sc scope.Scope, users, exposed map[int64]bool, e live.Event) (live.Event, bool) {
	if e.Kind == live.KindBotUpdated {
		// A bot is the forwarding pages' and, with its owner, the
		// admins' and superadmins' (bots.go). A member with forward
		// sees its own bots only, which takes the ownership records:
		// it gets no bot events, and its page refetches after its
		// own changes.
		return e, e.BotID != 0 && sc.Can(scope.Forward) && sc.SeesAllBots()
	}
	userOK := func(id int64) bool { return sc.SeesAllUsers() || users[id] }
	switch e.Kind {
	case live.KindMessageNew, live.KindMessageEdited, live.KindMessageDeleted,
		live.KindFileStored:
		if exposed[e.ChatID] {
			return e, true
		}
	}
	return filterEvent(e, sc.SeesChat, userOK)
}

// filterEvent applies visibility to one event: chatOK for a chat id
// (groups negative, private chats by the other user's id), userOK for
// a user id. A stats.changed keeps only the visible ids and is dropped
// when none is left. Kinds it does not know are dropped.
func filterEvent(e live.Event, chatOK, userOK func(int64) bool) (live.Event, bool) {
	switch e.Kind {
	case live.KindMessageNew, live.KindMessageEdited, live.KindMessageDeleted,
		live.KindFileStored, live.KindGroupUpdated:
		return e, e.ChatID != 0 && chatOK(e.ChatID)
	case live.KindUserUpdated:
		return e, e.UserID != 0 && userOK(e.UserID)
	case live.KindStatsChanged:
		chats := keep(e.ChatIDs, chatOK)
		users := keep(e.UserIDs, userOK)
		if len(chats) == 0 && len(users) == 0 && !e.More {
			return e, false
		}
		e.ChatIDs, e.UserIDs = chats, users
		return e, true
	}
	return e, false
}

// keep returns the ids that pass, in a new slice: the event is shared
// by every subscriber.
func keep(ids []int64, ok func(int64) bool) []int64 {
	out := make([]int64, 0, len(ids))
	for _, id := range ids {
		if ok(id) {
			out = append(out, id)
		}
	}
	return out
}

// lastSeen is the seq to resume after: Last-Event-ID, which the
// browser sends on its own reconnect and which is newer than the URL's
// last=, else last=. A Last-Event-ID that is not a seq resumes nothing
// (a resync); a malformed last= is the caller's mistake.
func lastSeen(r *http.Request) (uint64, error) {
	if h := r.Header.Get("Last-Event-ID"); h != "" {
		v, err := strconv.ParseUint(strings.TrimSpace(h), 10, 64)
		if err != nil {
			return ^uint64(0), nil
		}
		return v, nil
	}
	q := r.URL.Query().Get("last")
	if q == "" {
		return 0, nil
	}
	return strconv.ParseUint(q, 10, 64)
}

// writeFrame writes one event: id is its seq (none for a session
// event, so the browser's Last-Event-ID stays the last real one),
// event its kind, data its JSON.
func writeFrame(w http.ResponseWriter, e live.Event) error {
	b, err := json.Marshal(e)
	if err != nil {
		return err
	}
	var sb strings.Builder
	if e.Seq != 0 {
		sb.WriteString("id: ")
		sb.WriteString(strconv.FormatUint(e.Seq, 10))
		sb.WriteByte('\n')
	}
	sb.WriteString("event: ")
	sb.WriteString(e.Kind)
	sb.WriteString("\ndata: ")
	sb.Write(b)
	sb.WriteString("\n\n")
	_, err = w.Write([]byte(sb.String()))
	return err
}

func liveOff(w http.ResponseWriter, message string) {
	w.Header().Set("Retry-After", "5")
	writeError(w, http.StatusServiceUnavailable, "live_off",
		map[string]any{"message": message})
}

// events serves GET /api/events.
func (s *Server) events(w http.ResponseWriter, r *http.Request) {
	sess, _ := SessionFrom(r.Context())
	noStore(w)
	t, err := parseTopics(r.URL.Query().Get("topics"))
	if err != nil {
		writeError(w, http.StatusBadRequest, "bad_request",
			map[string]any{"message": err.Error()})
		return
	}
	last, err := lastSeen(r)
	if err != nil {
		writeError(w, http.StatusBadRequest, "bad_request",
			map[string]any{"message": "last must be a seq"})
		return
	}
	if s.live == nil {
		liveOff(w, "Live updates are off.")
		return
	}

	sc := s.requestScope(r, &sess)
	filter := s.liveFilter
	var exposed map[int64]bool
	if filter == nil {
		users, err := s.liveUsers(r, sc, t)
		if err != nil {
			s.internalError(w, "live users", err)
			return
		}
		if exposed, err = s.liveExposed(r.Context(), sc, t); err != nil {
			s.internalError(w, "live exposure", err)
			return
		}
		filter = func(sc scope.Scope, e live.Event) (live.Event, bool) {
			return eventFor(sc, users, exposed, e)
		}
	}
	sub, st, err := s.live.Subscribe(last, func(e live.Event) (live.Event, bool) {
		if !t.wants(e) {
			return e, false
		}
		return filter(sc, e)
	})
	switch {
	case errors.Is(err, live.ErrFull):
		w.Header().Set("Retry-After", "30")
		writeError(w, http.StatusServiceUnavailable, "unavailable",
			map[string]any{"message": "Too many live connections."})
		return
	case err != nil:
		liveOff(w, "Live updates are not available right now.")
		return
	}
	defer sub.Close()

	h := w.Header()
	h.Set("Content-Type", "text/event-stream")
	// nginx honours this per answer, besides the location's settings.
	h.Set("X-Accel-Buffering", "no")
	w.WriteHeader(http.StatusOK)
	rc := http.NewResponseController(w)
	// The server sets no write timeout, as streams are long; each write
	// gets its own deadline instead, so a client that stopped reading
	// is let go even when no proxy in front of the service bounds it.
	writeTimeout := s.liveCfg.WriteTimeout
	if writeTimeout <= 0 {
		writeTimeout = defaultWriteTime
	}
	deadline := func() { _ = rc.SetWriteDeadline(time.Now().Add(writeTimeout)) }

	deadline()
	if _, err := fmt.Fprintf(w, "retry: %d\n\n", liveRetry); err != nil {
		return
	}
	if err := writeFrame(w, live.Event{Seq: st.Seq, Kind: st.Kind}); err != nil {
		return
	}
	for _, e := range st.Replay {
		if err := writeFrame(w, e); err != nil {
			return
		}
	}
	if rc.Flush() != nil {
		return
	}

	heartbeat := s.liveCfg.Heartbeat
	if heartbeat <= 0 {
		heartbeat = defaultHeartbeat
	}
	recheck := s.liveCfg.Recheck
	if recheck <= 0 {
		recheck = defaultRecheck
	}
	hb := time.NewTicker(heartbeat)
	defer hb.Stop()
	rk := time.NewTicker(recheck)
	defer rk.Stop()

	for {
		var err error
		select {
		case <-r.Context().Done():
			return
		case <-sub.Done():
			// The daemon stayed away or the service is stopping: end
			// the stream, so the client polls or reconnects elsewhere.
			return
		case e := <-sub.Events():
			deadline()
			err = writeFrame(w, e)
		case <-sub.Lost():
			deadline()
			err = writeFrame(w, sub.Resync())
		case <-hb.C:
			deadline()
			_, err = w.Write([]byte(": keepalive\n\n"))
		case <-rk.C:
			if reason := s.liveRecheck(r, sc, t, exposed); reason != "" {
				deadline()
				_ = writeFrame(w, live.Event{Kind: live.KindSession, Reason: reason})
				_ = rc.Flush()
				return
			}
			continue
		}
		if err != nil || rc.Flush() != nil {
			return
		}
	}
}

// liveUsers is the users a stream of a viewer that does not see users
// in full may hear of: those of its user topics it sees
// (logdb.VisibleUserIDs), checked once when the stream opens, as the
// hub's filter cannot query. A user that appears in a member's groups
// only later is heard of after the next reconnect; the ids of other
// users in a stats.changed are dropped. nil for a viewer that sees
// every user.
func (s *Server) liveUsers(r *http.Request, sc scope.Scope, t topics) (map[int64]bool, error) {
	if sc.SeesAllUsers() {
		return nil, nil
	}
	ids := make([]int64, 0, len(t.users))
	for id := range t.users {
		ids = append(ids, id)
	}
	return logdb.VisibleUserIDs(r.Context(), s.ro, sc, ids)
}

// liveExposed is the stream's chat:<id> topics of groups the scope
// does not see but that are exposed, checked once when the stream
// opens, as the hub's filter cannot query (eventFor). A signed-in
// viewer may read such a group's public chat (logdb.OpenChat), and the
// chat view subscribes to it, so its events reach the viewer as the
// page would show them; only an explicit chat topic widens, never
// `messages`. Rechecked with the session (liveRecheck).
func (s *Server) liveExposed(ctx context.Context, sc scope.Scope, t topics) (map[int64]bool, error) {
	var ids []int64
	for id := range t.chats {
		if id < 0 && !sc.SeesChat(id) {
			ids = append(ids, id)
		}
	}
	if len(ids) == 0 {
		return nil, nil
	}
	return logdb.ExposedGroupsAmong(ctx, s.ro, ids)
}

// liveRecheck checks an open stream's session again: "revoked" when it
// was ended (signed out everywhere, password changed, account gone) or
// expired, "scope_changed" when the request scope is no longer the one
// the stream was opened with, or when the exposed groups it follows
// outside that scope (liveExposed) changed, "" otherwise. A session or
// exposure that cannot be checked (a database error) keeps the stream.
func (s *Server) liveRecheck(r *http.Request, sc scope.Scope, t topics, exposed map[int64]bool) string {
	ctx, cancel := context.WithTimeout(r.Context(), recheckTimeout)
	defer cancel()
	sess, c := s.verifier.Verify(ctx, r)
	switch c {
	case auth.Valid:
	case auth.Unverified:
		return ""
	default:
		return live.ReasonRevoked
	}
	if !reflect.DeepEqual(s.requestScope(r, &sess), sc) {
		return live.ReasonScopeChanged
	}
	if s.liveFilter == nil {
		now, err := s.liveExposed(ctx, sc, t)
		if err == nil && !maps.Equal(now, exposed) {
			return live.ReasonScopeChanged
		}
	}
	return ""
}
