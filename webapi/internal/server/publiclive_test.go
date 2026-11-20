// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"strconv"
	"strings"
	"testing"
	"time"

	"tgloggerd/webapi/internal/live"
	"tgloggerd/webapi/internal/testdb"
)

// GET /api/events without a session: the live chat events of exposed
// groups for signed-out visitors (publiclive.go), over the "sample"
// fixture, where gnuWeeb and rustChat are exposed and kernelChat is
// not.

func newPublicEnv(t *testing.T, o liveOpts) (*liveEnv, *testdb.DB) {
	t.Helper()
	lg := testdb.New(t, testdb.Logger, "sample")
	o.ro = lg.DB
	e := newLiveEnv(t, o)
	e.cookie = ""
	return e, lg
}

// from is the X-Forwarded-For header of a request from addr; the env
// must trust loopback.
func from(addr string) []string { return []string{"X-Forwarded-For", addr} }

var loopback = TrustedProxies{netip.MustParsePrefix("127.0.0.0/8"),
	netip.MustParsePrefix("::1/128")}

// firstFrames reads the retry line and the first frame of a stream.
func firstFrames(t *testing.T, st *stream) (retry int, f frame) {
	t.Helper()
	for {
		select {
		case fr, ok := <-st.frames:
			if !ok {
				t.Fatal("the stream ended")
			}
			if fr.retry != "" {
				n, err := strconv.Atoi(fr.retry)
				if err != nil {
					t.Fatalf("retry %q", fr.retry)
				}
				retry = n
				continue
			}
			if fr.comment {
				continue
			}
			return retry, fr
		case <-time.After(5 * time.Second):
			t.Fatal("no frame")
		}
	}
}

// A visitor's stream gets the message and file events of the exposed
// chat it follows and nothing else, without seqs or file ids, and
// starts as a resync naming the refetch interval whatever it resumes
// from.
func TestPublicEventsExposedChat(t *testing.T) {
	t.Parallel()
	e, _ := newPublicEnv(t, liveOpts{})
	st := e.open(fmt.Sprintf("topics=chat:%d&last=x", gnuWeeb), "Last-Event-ID", "100")
	retry, f := firstFrames(t, st)
	if retry < publicRetryMin || retry >= publicRetryMin+publicRetrySpan {
		t.Errorf("retry %d", retry)
	}
	if f.id != "" || f.event != "resync" || f.data != `{"kind":"resync","refetch_interval":5}` {
		t.Fatalf("first frame %+v", f)
	}

	e.conn.Send(live.Event{Seq: 101, Kind: live.KindMessageNew, ChatID: kernelChat, MessageID: 1})
	e.conn.Send(live.Event{Seq: 102, Kind: live.KindMessageNew, ChatID: rustChat, MessageID: 1})
	e.conn.Send(live.Event{Seq: 103, Kind: live.KindGroupUpdated, ChatID: gnuWeeb})
	e.conn.Send(live.Event{Seq: 104, Kind: live.KindStatsChanged, ChatIDs: []int64{gnuWeeb}})
	e.conn.Send(live.Event{Seq: 105, Kind: live.KindUserUpdated, UserID: dewi})
	e.conn.Send(live.Event{Seq: 106, Kind: live.KindMessageNew, ChatID: gnuWeeb, MessageID: 2})
	e.conn.Send(live.Event{Seq: 107, Kind: live.KindMessageEdited, ChatID: gnuWeeb, MessageID: 2})
	e.conn.Send(live.Event{Seq: 108, Kind: live.KindFileStored, ChatID: gnuWeeb, MessageID: 2, FileID: 9})
	e.conn.Send(live.Event{Seq: 109, Kind: live.KindMessageDeleted, ChatID: gnuWeeb, MessageID: 2})
	e.conn.Send(live.Event{Seq: 110, Kind: live.KindFileStored, FileID: 10})

	want := []string{
		`{"kind":"message.new","chat_id":-1001000000001,"message_id":2}`,
		`{"kind":"message.edited","chat_id":-1001000000001,"message_id":2}`,
		`{"kind":"file.stored","chat_id":-1001000000001,"message_id":2}`,
		`{"kind":"message.deleted","chat_id":-1001000000001,"message_id":2}`,
	}
	for _, w := range want {
		if f := st.next(); f.id != "" || f.data != w {
			t.Fatalf("frame %+v, want data %s and no id", f, w)
		}
	}
	// Nothing else came: the next one is the next event of the chat.
	e.conn.Send(live.Event{Seq: 111, Kind: live.KindMessageNew, ChatID: gnuWeeb, MessageID: 3})
	if f := st.next(); f.data != `{"kind":"message.new","chat_id":-1001000000001,"message_id":3}` {
		t.Fatalf("frame %+v", f)
	}
}

// Every other topic, a group that is not exposed or does not exist,
// and more chats than allowed are refused alike, so a visitor cannot
// tell which ids exist.
func TestPublicEventsRefused(t *testing.T) {
	t.Parallel()
	e, _ := newPublicEnv(t, liveOpts{})
	five := []string{}
	for _, id := range []int64{gnuWeeb, rustChat, -1001000000010, gnuWeeb - 100, gnuWeeb - 101} {
		five = append(five, fmt.Sprintf("chat:%d", id))
	}
	var first string
	for _, q := range []string{
		fmt.Sprintf("chat:%d", kernelChat),
		"chat:-1009999999999",
		fmt.Sprintf("chat:%d", dewi),
		"messages", "stats", "bots", "user:9299000001",
		fmt.Sprintf("group:%d", gnuWeeb),
		fmt.Sprintf("chat:%d,group:%d", gnuWeeb, gnuWeeb),
		fmt.Sprintf("chat:%d,messages", gnuWeeb),
		fmt.Sprintf("chat:%d,chat:%d", gnuWeeb, kernelChat),
		strings.Join(five, ","),
	} {
		resp, cancel := e.request("topics=" + q)
		b, _ := io.ReadAll(resp.Body)
		cancel()
		if resp.StatusCode != http.StatusUnauthorized {
			t.Errorf("%s: %d %s", q, resp.StatusCode, b)
			continue
		}
		if first == "" {
			first = string(b)
		} else if string(b) != first {
			t.Errorf("%s: %s, want the same answer as the others: %s", q, b, first)
		}
	}
	if !strings.Contains(first, "unauthenticated") {
		t.Errorf("answer %s", first)
	}
	if n := e.hub.Subscribers(); n != 0 {
		t.Errorf("%d subscribers after refusals", n)
	}
	// Four exposed chats are allowed.
	st := e.open("topics=" + strings.Join(five[:3], ",") + "," + five[0])
	if _, f := firstFrames(t, st); f.event != "resync" {
		t.Fatalf("%+v", f)
	}
}

// A group withdrawn from exposure stops passing events and ends its
// visitors' streams with scope_changed: at once when the MCP pages
// withdraw it (refresh), else at the next reading of the snapshot.
func TestPublicEventsWithdrawn(t *testing.T) {
	t.Parallel()
	for _, tc := range []struct {
		name    string
		every   time.Duration
		refresh bool
	}{
		{"refresh", time.Hour, true},
		{"ticker", 50 * time.Millisecond, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			e, lg := newPublicEnv(t, liveOpts{cfg: LiveConfig{
				Public: PublicLiveConfig{Exposure: tc.every}}})
			st := e.open(fmt.Sprintf("topics=chat:%d,chat:%d", gnuWeeb, rustChat))
			firstFrames(t, st)
			if _, err := lg.DB.Exec("DELETE FROM telegram_public_groups WHERE group_id = ?",
				rustChat); err != nil {
				t.Fatal(err)
			}
			if tc.refresh {
				e.s.public().expo.refresh(context.Background())
				e.conn.Send(live.Event{Seq: 101, Kind: live.KindMessageNew, ChatID: rustChat, MessageID: 1})
			}
			f := st.next()
			if f.event != "session" || f.data != `{"kind":"session","reason":"scope_changed"}` {
				t.Fatalf("after the withdrawal: %+v", f)
			}
			st.ended()
			waitFor(t, "the slot", func() bool { return e.hub.Subscribers() == 0 })
			// The refresher stops with the last stream.
			x := e.s.public().expo
			x.mu.Lock()
			users := x.users
			x.mu.Unlock()
			if users != 0 {
				t.Errorf("%d users of the snapshot", users)
			}
		})
	}
}

func expectRefused(t *testing.T, e *liveEnv, q, retry string, hdr ...string) {
	t.Helper()
	resp, cancel := e.request(q, hdr...)
	b, _ := io.ReadAll(resp.Body)
	cancel()
	if resp.StatusCode != http.StatusServiceUnavailable || !strings.Contains(string(b), "unavailable") ||
		resp.Header.Get("Retry-After") != retry {
		t.Fatalf("%v: %d %q %s, want 503 with Retry-After %s", hdr, resp.StatusCode,
			resp.Header.Get("Retry-After"), b, retry)
	}
}

// The visitors' caps: per address (an IPv6 /64 counts as one), in all,
// and signed-in viewers keep their slots when the visitors' are taken.
func TestPublicEventsCaps(t *testing.T) {
	t.Parallel()
	e, _ := newPublicEnv(t, liveOpts{proxies: loopback, hub: live.Options{MaxSubscribers: 2},
		cfg: LiveConfig{Public: PublicLiveConfig{Max: 3, PerAddr: 2, OpensPerMinute: 100}}})
	q := fmt.Sprintf("topics=chat:%d", gnuWeeb)
	open := func(addr string) *stream {
		st := e.open(q, from(addr)...)
		firstFrames(t, st)
		return st
	}
	open("203.0.113.7")
	open("203.0.113.7")
	expectRefused(t, e, q, "30", from("203.0.113.7")...)
	open("2001:db8:1:2::1")
	// The total is reached: another address is refused too.
	expectRefused(t, e, q, "30", from("198.51.100.1")...)

	// Signed-in streams have the hub's slots to themselves.
	e.cookie = e.cookies.Issue(httptest.NewRecorder(), e.cookies.NewSession(1, "admin", "admin", 0))
	a := e.open("topics=messages")
	a.expect("100", "hello")
	b := e.open("topics=messages")
	b.expect("100", "hello")
	e.cookie = ""

	// Per /64: a second stream of the same prefix fits, a third does
	// not; the total frees a slot when a stream ends.
	e2, _ := newPublicEnv(t, liveOpts{proxies: loopback,
		cfg: LiveConfig{Public: PublicLiveConfig{Max: 10, PerAddr: 2, OpensPerMinute: 100}}})
	st := e2.open(q, from("2001:db8:1:2::1")...)
	firstFrames(t, st)
	st2 := e2.open(q, from("2001:db8:1:2:ffff::9")...)
	firstFrames(t, st2)
	expectRefused(t, e2, q, "30", from("2001:db8:1:2::3")...)
	st.close()
	waitFor(t, "the slot", func() bool { return e2.hub.Subscribers() == 1 })
	st3 := e2.open(q, from("2001:db8:1:2::3")...)
	firstFrames(t, st3)
}

// Opens per address per minute: a client in a reconnect loop is
// refused for the rest of the minute, while other addresses are not.
func TestPublicEventsOpenRate(t *testing.T) {
	t.Parallel()
	e, _ := newPublicEnv(t, liveOpts{proxies: loopback,
		cfg: LiveConfig{Public: PublicLiveConfig{OpensPerMinute: 2}}})
	q := fmt.Sprintf("topics=chat:%d", gnuWeeb)
	for range 2 {
		st := e.open(q, from("192.0.2.1")...)
		firstFrames(t, st)
		st.close()
	}
	expectRefused(t, e, q, "60", from("192.0.2.1")...)
	st := e.open(q, from("192.0.2.2")...)
	firstFrames(t, st)
}

// Signed-out streams can be turned off; a visitor is then refused as
// for a topic it may not have.
func TestPublicEventsOff(t *testing.T) {
	t.Parallel()
	e, _ := newPublicEnv(t, liveOpts{cfg: LiveConfig{Public: PublicLiveConfig{Max: -1}}})
	resp, cancel := e.request(fmt.Sprintf("topics=chat:%d", gnuWeeb))
	cancel()
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("%d", resp.StatusCode)
	}
}

func TestAddrKey(t *testing.T) {
	t.Parallel()
	for in, want := range map[string]string{
		"203.0.113.7":            "203.0.113.7",
		"::ffff:203.0.113.7":     "203.0.113.7",
		"2001:db8:1:2:3:4:5:6":   "2001:db8:1:2::/64",
		"2001:db8:1:2::1":        "2001:db8:1:2::/64",
		"2001:db8:1:3::1":        "2001:db8:1:3::/64",
		"not an address":         "not an address",
		"2001:db8:1:2:ffff::9/x": "2001:db8:1:2:ffff::9/x",
	} {
		if got := addrKey(in); got != want {
			t.Errorf("addrKey(%q) = %q, want %q", in, got, want)
		}
	}
}
