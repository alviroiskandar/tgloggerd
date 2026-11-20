// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/go-sql-driver/mysql"

	"tgloggerd/webapi/internal/auth"
	"tgloggerd/webapi/internal/daemon"
	"tgloggerd/webapi/internal/media"
	"tgloggerd/webapi/internal/testdb"
)

// The photo lists as the page's carousel reads them, with a daemon:
// the current photo first even when the history lacks it, and a photo
// the store no longer holds linked all the same, its link fetched from
// Telegram through the daemon (logger fixtures "sample" and
// "photo_lists").

const (
	fotoBaru = 9299410001
	fotoGrup = -1001000041001
)

// photoDaemon is a fake daemon: it answers the photo lists and the
// fetches, and records every request.
type photoDaemon struct {
	mu    sync.Mutex
	asked []string // "METHOD /path?query"
	// block, when set, holds the photo lists until it is closed.
	block chan struct{}
	// list is the photo lists' answer.
	list string
}

func (d *photoDaemon) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	d.mu.Lock()
	d.asked = append(d.asked, r.Method+" "+r.URL.RequestURI())
	block, list := d.block, d.list
	d.mu.Unlock()
	w.Header().Set("Content-Type", "application/json")
	switch {
	case strings.HasSuffix(r.URL.Path, "/photos"):
		if block != nil {
			// Bounded, so a failed test cannot hang on it.
			select {
			case <-block:
			case <-time.After(10 * time.Second):
			}
		}
		if list == "" {
			list = `{"outcome":"ok","message":"","total":0,"photos":[]}`
		}
		io.WriteString(w, list)
	case r.URL.Path == "/v1/files/851002/fetch", r.URL.Path == "/v1/files/851004/fetch",
		r.URL.Path == "/v1/files/851101/fetch":
		// Fetched now; its bytes turn out to be 851001's.
		io.WriteString(w, `{"state":"stored","id":851001}`)
	case r.URL.Path == "/v1/files/access":
		io.WriteString(w, `{"ids":0,"updated":0}`)
	default:
		w.WriteHeader(http.StatusNotFound)
		io.WriteString(w, `{"error":"not_found"}`)
	}
}

// requests are the requests whose path starts with prefix.
// requests are the requests asked for that start with prefix, but
// for the status asks every page may start in the background
// (loggerAccount).
func (d *photoDaemon) requests(prefix string) []string {
	d.mu.Lock()
	defer d.mu.Unlock()
	var out []string
	for _, a := range d.asked {
		if strings.HasPrefix(a, prefix) && !strings.HasPrefix(a, "GET /v1/status") {
			out = append(out, a)
		}
	}
	return out
}

type photoListEnv struct {
	*chatEnv
	app   *sql.DB
	fake  *photoDaemon
	now   *time.Time
	nowMu *sync.Mutex
	// files counts the service's statements on telegram_files.
	files *atomic.Int64
	asks  *photoRefresher
}

// countingDB is a pool on ro's database that counts the statements
// naming table (countingConnector).
func countingDB(t *testing.T, ro *sql.DB, table string) (*sql.DB, *atomic.Int64) {
	t.Helper()
	var name string
	if err := ro.QueryRow("SELECT DATABASE()").Scan(&name); err != nil {
		t.Fatal(err)
	}
	cfg, err := mysql.ParseDSN(os.Getenv(testdb.EnvDSN))
	if err != nil {
		t.Fatal(err)
	}
	cfg.DBName = name
	mc, err := mysql.NewConnector(cfg)
	if err != nil {
		t.Fatal(err)
	}
	n := &atomic.Int64{}
	db := sql.OpenDB(countingConnector{Connector: mc, table: table, n: n})
	t.Cleanup(func() { db.Close() })
	return db, n
}

func newPhotoListEnv(t *testing.T) *photoListEnv {
	t.Helper()
	ro := testdb.New(t, testdb.Logger, "sample", "photo_lists").DB
	counted, files := countingDB(t, ro, "telegram_files")
	app := testdb.New(t, testdb.Web, "accounts").DB
	fake := &photoDaemon{}
	srv := httptest.NewServer(fake)
	t.Cleanup(srv.Close)
	dc, err := daemon.New(srv.URL, testKey, 5*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	store := filepath.Join(t.TempDir(), "storage")
	// 851001's blob, which the fetches answer with.
	sum := sha256.Sum256([]byte("photo lists fixture 851001"))
	p := media.StorePath(store, hex.EncodeToString(sum[:]), "jpg")
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, []byte("photo 851001"), 0o644); err != nil {
		t.Fatal(err)
	}
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	signer := auth.NewSigner(testKey)
	now, mu := chatNow, &sync.Mutex{}
	e := &photoListEnv{chatEnv: &chatEnv{t: t, ro: ro,
		cookies: &auth.Cookies{Signer: signer, Timeout: 3600, Secure: true},
		tokens:  auth.NewFileTokens(testKey)}, app: app, fake: fake, now: &now, nowMu: mu,
		files: files}
	clock := func() time.Time {
		mu.Lock()
		defer mu.Unlock()
		return now
	}
	e.asks = newPhotoRefresher(log, dc, clock)
	e.h = New(Deps{
		Log:       log,
		RO:        counted,
		App:       app,
		Signer:    signer,
		Cookies:   e.cookies,
		Limiter:   auth.NewRateLimiter(5, time.Hour),
		Daemon:    dc,
		Tokens:    e.tokens,
		Access:    daemon.NewAccessReporter(dc, log),
		Media:     MediaConfig{StorageDir: store},
		clock:     clock,
		photoAsks: e.asks,
	})
	// No ask outlives the test's daemon.
	t.Cleanup(e.asks.wait)
	return e
}

// photoItems are a photos answer's items as "id:at:file:url?".
func photoItemsOf(e *photoListEnv, p tPhotos) []string {
	var out []string
	for _, it := range p.Items {
		if it.URL != nil && *it.URL != "/files/"+e.tokens.Encrypt(*it.FileID) {
			e.t.Errorf("item %d: url %s", it.ID, *it.URL)
		}
		out = append(out, fmt.Sprintf("%d:%s:%d:%v", it.ID, it.At, *it.FileID, it.URL != nil))
	}
	return out
}

func TestPhotoListsCurrentFirst(t *testing.T) {
	t.Parallel()
	e := newPhotoListEnv(t)

	// The current photo, missing from the history, comes first as item
	// 0 at the time its file was recorded, though the history's newest
	// row is later; every photo has a link, the evicted one too, as
	// the daemon may fetch it.
	var p tPhotos
	e.decode(photosPath(fotoBaru), e.as("admin"), 200, &p)
	want := "[0:2026-09-01T16:38:00Z:851003:true 1102:2026-09-01T16:39:11Z:851002:true " +
		"1101:2026-08-01T10:00:00Z:851001:true]"
	if got := fmt.Sprint(photoItemsOf(e, p)); got != want {
		t.Errorf("user items %s, want %s", got, want)
	}
	if p.CurrentID == nil || *p.CurrentID != 0 {
		t.Errorf("user current_id %v, want 0", p.CurrentID)
	}

	// The same for a group, whose current photo is evicted too.
	e.decode(fmt.Sprintf("/api/groups/%d/photos", fotoGrup), e.as("admin"), 200, &p)
	want = "[0:2026-10-05T10:00:00Z:851004:true 1104:2026-08-01T10:00:00Z:851001:true " +
		"1103:2026-06-01T10:00:00Z:851005:true]"
	if got := fmt.Sprint(photoItemsOf(e, p)); got != want {
		t.Errorf("group items %s, want %s", got, want)
	}
	if p.CurrentID == nil || *p.CurrentID != 0 {
		t.Errorf("group current_id %v, want 0", p.CurrentID)
	}

	// The link of an evicted photo has the daemon fetch it: a profile
	// photo's and a group photo's file, not only a message's.
	for _, f := range []uint64{851002, 851004} {
		code, _, body := e.get("/files/"+e.tokens.Encrypt(f), e.as("admin"))
		if code != 200 || string(body) != "photo 851001" {
			t.Errorf("file %d: %d %q", f, code, body)
		}
		if got := e.fake.requests(fmt.Sprintf("POST /v1/files/%d/fetch", f)); len(got) != 1 {
			t.Errorf("file %d: fetches %q", f, got)
		}
	}
}

// A member sees a group's photos only for a group granted to it, with
// links it may follow only with fetch_files: without it, the evicted
// photos have none.
func TestPhotoListsMemberScope(t *testing.T) {
	t.Parallel()
	e := newPhotoListEnv(t)
	path := fmt.Sprintf("/api/groups/%d/photos", fotoGrup)
	e.decode(path, e.as("researcher"), 404, nil)

	if _, err := e.app.Exec("INSERT INTO web_member_grant_groups (web_user_id, federation_id, "+
		"group_id) VALUES (5, 0, ?)", fotoGrup); err != nil {
		t.Fatal(err)
	}
	var p tPhotos
	e.decode(path, e.as("researcher"), 200, &p)
	want := "[0:2026-10-05T10:00:00Z:851004:false 1104:2026-08-01T10:00:00Z:851001:true " +
		"1103:2026-06-01T10:00:00Z:851005:false]"
	if got := fmt.Sprint(photoItemsOf(e, p)); got != want {
		t.Errorf("member items %s, want %s", got, want)
	}
	if _, err := e.app.Exec("INSERT INTO web_member_abilities (web_user_id, ability) " +
		"VALUES (5, 'fetch_files')"); err != nil {
		t.Fatal(err)
	}
	e.decode(path, e.as("researcher"), 200, &p)
	want = strings.ReplaceAll(want, "false", "true")
	if got := fmt.Sprint(photoItemsOf(e, p)); got != want {
		t.Errorf("member with fetch_files items %s, want %s", got, want)
	}
	// Still no user's photos for a member.
	e.decode(photosPath(fotoBaru), e.as("researcher"), 404, nil)
}

// The list reads the files in two statements whatever its length (the
// current photo's and the history's), never one per photo.
func TestPhotoListsQueries(t *testing.T) {
	t.Parallel()
	e := newPhotoListEnv(t)
	for i := 0; i < 50; i++ {
		f := 851100 + i
		if _, err := e.ro.Exec("INSERT INTO telegram_files (id, tg_file_id, file_type, file_size, "+
			"on_disk) VALUES (?, ?, 'photo', 100, 0)", f, fmt.Sprint("tg-", f)); err != nil {
			t.Fatal(err)
		}
		if _, err := e.ro.Exec("INSERT INTO telegram_user_hist_profile_photo (user_id, file_id, "+
			"created_at) VALUES (?, ?, '2025-01-01 00:00:00' + INTERVAL ? DAY)", fotoBaru, f,
			i); err != nil {
			t.Fatal(err)
		}
	}
	cost := func(path string, items int) int64 {
		t.Helper()
		before := e.files.Load()
		var p tPhotos
		e.decode(path, e.as("admin"), 200, &p)
		if len(p.Items) != items {
			t.Errorf("%s: %d items, want %d", path, len(p.Items), items)
		}
		return e.files.Load() - before
	}
	few := cost(fmt.Sprintf("/api/groups/%d/photos", fotoGrup), 3)
	many := cost(photosPath(fotoBaru), 53)
	if few == 0 || many != few {
		t.Errorf("statements on telegram_files: %d for 3 photos, %d for 53", few, many)
	}
}

// advance moves the service's clock on by d.
func (e *photoListEnv) advance(d time.Duration) {
	e.nowMu.Lock()
	defer e.nowMu.Unlock()
	*e.now = e.now.Add(d)
}

// openPage loads a page's data as who, failing the test if the answer
// takes longer than a few seconds: the ask must not hold it up.
func (e *photoListEnv) openPage(path, who, route string) {
	e.t.Helper()
	cookie := e.as(who)
	done := make(chan int, 1)
	go func() {
		code, _, _ := e.get("/api/page"+path, cookie)
		done <- code
	}()
	select {
	case code := <-done:
		if code != 200 {
			e.t.Fatalf("%s as %s: %d", path, who, code)
		}
	case <-time.After(3 * time.Second):
		e.t.Fatalf("%s: the page waited for the daemon", path)
	}
	page[struct{}](e.chatEnv, path, who, 200, route)
}

// Opening a user or group page has the daemon list its photos in the
// background: never holding the page up, one ask in flight per user
// or group, none again within ten minutes, whatever came of it, and a
// flood wait's wait when longer.
func TestPhotoListsRefresh(t *testing.T) {
	t.Parallel()
	e := newPhotoListEnv(t)
	userList := fmt.Sprintf("GET /v1/users/%d/photos", fotoBaru)
	groupList := fmt.Sprintf("GET /v1/chats/%d/photos", fotoGrup)
	userPage, groupPage := fmt.Sprintf("/users/%d", fotoBaru), fmt.Sprintf("/groups/%d", fotoGrup)
	asked := func(prefix string, want int) {
		t.Helper()
		if got := e.fake.requests(prefix); len(got) != want {
			t.Errorf("%s: asked %d times (%q), want %d", prefix, len(got), got, want)
		}
	}

	// The daemon holds its answer: the pages answer all the same, and
	// a second opening while the first ask runs asks nothing more.
	block := make(chan struct{})
	e.fake.mu.Lock()
	e.fake.block = block
	e.fake.mu.Unlock()
	e.openPage(userPage, "admin", "user")
	e.openPage(groupPage, "admin", "group")
	e.openPage(userPage, "admin", "user")
	e.openPage(fmt.Sprintf("/users/%d/history", fotoBaru), "admin", "user_history")
	e.openPage(groupPage, "gw_admin", "group")
	close(block)
	e.asks.wait()
	asked(userList, 1)
	asked(groupList, 1)
	if got := e.fake.requests(userList); len(got) == 1 &&
		got[0] != userList+"?offset=0&limit=100" {
		t.Errorf("asked %q", got[0])
	}
	e.fake.mu.Lock()
	e.fake.block = nil
	e.fake.mu.Unlock()

	// Within ten minutes nothing is asked again; after, once more.
	e.advance(9 * time.Minute)
	e.openPage(userPage, "admin", "user")
	e.openPage(groupPage, "admin", "group")
	e.asks.wait()
	asked(userList, 1)
	asked(groupList, 1)
	e.advance(2 * time.Minute)
	e.openPage(userPage, "admin", "user")
	e.openPage(groupPage, "admin", "group")
	e.asks.wait()
	asked(userList, 2)
	asked(groupList, 2)

	// A failure is not asked again sooner.
	e.fake.mu.Lock()
	e.fake.list = `{"outcome":"error","message":"boom","total":null,"photos":[]}`
	e.fake.mu.Unlock()
	e.advance(11 * time.Minute)
	e.openPage(userPage, "admin", "user")
	e.asks.wait()
	e.advance(5 * time.Minute)
	e.openPage(userPage, "admin", "user")
	e.asks.wait()
	asked(userList, 3)

	// A flood wait longer than ten minutes is sat out.
	e.fake.mu.Lock()
	e.fake.list = `{"outcome":"flood_wait","message":"","retry_after":3600,"total":null,"photos":[]}`
	e.fake.mu.Unlock()
	e.advance(11 * time.Minute)
	e.openPage(groupPage, "admin", "group")
	e.asks.wait()
	asked(groupList, 3)
	e.advance(30 * time.Minute)
	e.openPage(groupPage, "admin", "group")
	e.asks.wait()
	asked(groupList, 3)
	e.advance(31 * time.Minute)
	e.openPage(groupPage, "admin", "group")
	e.asks.wait()
	asked(groupList, 4)
}

// Only a viewer who sees the photos has them listed: a member gets the
// limited user page and asks nothing, and a group it may not see
// answers 404 without an ask.
func TestPhotoListsRefreshScope(t *testing.T) {
	t.Parallel()
	e := newPhotoListEnv(t)
	type limited struct{ Limited bool }
	if _, err := e.app.Exec("INSERT INTO web_member_grant_groups (web_user_id, federation_id, "+
		"group_id) VALUES (5, 0, ?)", gnuWeeb); err != nil {
		t.Fatal(err)
	}
	// Bima posts in GNU/Weeb, so the member sees him, limited.
	if p := page[limited](e.chatEnv, "/users/9299000002", "researcher", 200, "user"); !p.Limited {
		t.Fatal("the member's user page is not the limited one")
	}
	e.decode(fmt.Sprintf("/api/page/groups/%d", fotoGrup), e.as("researcher"), 404, nil)
	e.asks.wait()
	if got := e.fake.requests("GET /v1/"); len(got) != 0 {
		t.Errorf("asked %q", got)
	}
	// The group it is granted is listed.
	page[struct{}](e.chatEnv, fmt.Sprintf("/groups/%d", gnuWeeb), "researcher", 200, "group")
	e.asks.wait()
	if got := e.fake.requests(fmt.Sprintf("GET /v1/chats/%d/photos", gnuWeeb)); len(got) != 1 {
		t.Errorf("granted group: asked %q", got)
	}
}
