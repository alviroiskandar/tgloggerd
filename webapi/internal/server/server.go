// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

// Package server holds the HTTP routes of the Go web service.
//
// Routing uses the standard library's ServeMux with method and path
// patterns ("GET /api/health"), which is all the API needs.
package server

import (
	"context"
	"database/sql"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"os"
	"sync"
	"time"

	"tgloggerd/webapi/internal/auth"
	"tgloggerd/webapi/internal/daemon"
	"tgloggerd/webapi/internal/live"
	"tgloggerd/webapi/internal/logdb"
	"tgloggerd/webapi/internal/mail"
	"tgloggerd/webapi/internal/mcp"
	"tgloggerd/webapi/internal/render"
	"tgloggerd/webapi/internal/scope"
	"tgloggerd/webapi/internal/settings"
)

// Deps is what the service is built from.
type Deps struct {
	Log     *slog.Logger
	RO      *sql.DB // SELECT-only over the logger's schema
	App     *sql.DB // read-write over the web's own schema
	Signer  *auth.Signer
	Cookies *auth.Cookies
	Limiter *auth.RateLimiter
	// AcctLimiter caps failed logins per account from all addresses;
	// nil leaves only Limiter, per address and name.
	AcctLimiter *auth.RateLimiter
	Daemon      *daemon.Client // nil when the daemon API is off
	// Tokens opens the /files/<token> tokens.
	Tokens *auth.FileTokens
	// Access collects the ids of the files served for the daemon; nil
	// when the daemon API is off.
	Access *daemon.AccessReporter
	Media  MediaConfig
	// Proxies are the reverse proxies whose X-Forwarded-For hops are
	// skipped to find a request's client (WEBAPI_TRUSTED_PROXIES).
	Proxies TrustedProxies
	// Live fans the daemon's events out to GET /api/events; nil when
	// live updates are off.
	Live    *live.Hub
	LiveCfg LiveConfig
	// Render is the render service's client; nil renders every page
	// in the browser (the fallback).
	Render *render.Client
	// Counts keeps whole-table and list counts for a short time
	// (logdb.CountCache, WEBAPI_COUNT_CACHE_SECS); nil counts on every
	// request.
	Counts *logdb.CountCache
	// Assets are the client bundle's files pages link, and its build.
	Assets render.Assets
	// AssetFiles is the bundle's assets/ directory, served under
	// /app/assets/; nil answers 404 there.
	AssetFiles *os.Root
	// Defaults are the site's settings for what an account did not
	// set; empty fields are settings.SiteDefaults.
	Defaults settings.Defaults
	// MCP is the /mcp endpoint's settings.
	MCP MCPConfig
	// Mail sends the account links and notices; nil turns mail off,
	// which is allowed: nothing is sent then (email.go). MailCfg is the
	// site's URL for the links and their lifetimes; zero lifetimes are
	// the defaults.
	Mail    *mail.Mailer
	MailCfg MailConfig
	// ForgotLimiter caps forgotten-password requests per client
	// address, ForgotAcctLimiter the links sent per account; nil is
	// the built-in caps.
	ForgotLimiter     *auth.RateLimiter
	ForgotAcctLimiter *auth.RateLimiter
	// SearchEngine answers the message searches' full-text conditions
	// (internal/searchengine); nil leaves them on MySQL, as before.
	SearchEngine SearchEngine
	// Bots resolves the Telegram bots' own users (BotResolver), nudged
	// when a bot is created or given a new token; nil nudges nothing.
	Bots *BotResolver
	// Recheck limits the chat rechecks asked of the daemon when chats
	// and messages are served (recheck.go); the zero value is
	// DefaultRecheck.
	Recheck RecheckConfig

	// pages replaces the page table, state the database's account
	// state behind a session, clock the time, prefs the accounts'
	// stored preferences and pins their pinned groups, for tests.
	pages []pageRoute
	state auth.StateFunc
	clock func() time.Time
	prefs func(ctx context.Context, uid uint64) (settings.Prefs, error)
	pins  func(ctx context.Context, uid uint64, sc scope.Scope) ([]logdb.PinnedGroup, error)
	// discord replaces the client that sends webhook test messages.
	discord *http.Client
	// statfs replaces fsSize, the size of the store's file system.
	statfs func(path string) (uint64, error)
	// search replaces the list pages' search (logdb.Search), for
	// tests of a search that runs out of time.
	search searchFunc
	// photoAsks replaces the photo lists' background asks
	// (photoRefresher).
	photoAsks *photoRefresher
	// logger replaces the keeper of the logging account's id
	// (loggerAccount).
	logger *loggerAccount
	// emoji replaces the pages' custom emoji resolver
	// (emojiResolver).
	emoji *emojiResolver
	// rechecks replaces the chat rechecker (rechecker).
	rechecks *rechecker
}

// searchFunc is logdb.Search's signature.
type searchFunc func(ctx context.Context, db *sql.DB, sc scope.Scope, e *logdb.Entity,
	req logdb.SearchRequest) (*logdb.SearchResult, error)

// Server carries what the handlers share.
type Server struct {
	log         *slog.Logger
	ro          *sql.DB
	app         *sql.DB
	signer      *auth.Signer
	cookies     *auth.Cookies
	limiter     *auth.RateLimiter
	acctLimiter *auth.RateLimiter
	verifier    *auth.Verifier
	daemon      *daemon.Client
	tokens      *auth.FileTokens
	access      *daemon.AccessReporter
	media       MediaConfig
	proxies     TrustedProxies
	live        *live.Hub
	liveCfg     LiveConfig
	// liveFilter replaces eventInScope in tests.
	liveFilter func(scope.Scope, live.Event) (live.Event, bool)
	render     *render.Client
	assets     render.Assets
	assetFiles *os.Root
	defaults   settings.Defaults
	// viewerPrefs reads an account's preferences (settings.Load),
	// viewerPins its pinned groups in the scope (pinnedGroups).
	viewerPrefs func(ctx context.Context, uid uint64) (settings.Prefs, error)
	viewerPins  func(ctx context.Context, uid uint64, sc scope.Scope) ([]logdb.PinnedGroup, error)
	pages       []pageRoute
	clock       func() time.Time
	search      searchFunc
	// discord sends the webhook test messages.
	discord *http.Client
	// statfs is the size of the file system holding a path (fsSize).
	statfs func(path string) (uint64, error)
	// mcp answers /mcp; mcpSlots holds one token per tool call that
	// may run at once (MCP_THREADS).
	mcp      *mcp.Server
	mcpCfg   MCPConfig
	mcpSlots chan struct{}
	// mail, mailCfg, forgotLimiter and forgotAcct: see Deps.
	mail          *mail.Mailer
	mailCfg       MailConfig
	forgotLimiter *auth.RateLimiter
	forgotAcct    *auth.RateLimiter
	// engine is Deps.SearchEngine.
	engine SearchEngine
	// bots is Deps.Bots.
	bots *BotResolver
	// members refreshes the member counts the pages show and caches
	// the first pages of member lists (groupmembers.go).
	members memberState
	// photoAsks has the daemon list a user's or group's photos when
	// its page opens.
	photoAsks *photoRefresher
	// logger keeps the logging account's id (Viewer.logger).
	logger *loggerAccount
	// emoji resolves the custom emoji and message effects of a page
	// (emoji.go).
	emoji *emojiResolver
	// series keeps the dashboard series' finished periods.
	series *logdb.SeriesCache
	// rechecks has the daemon recheck the chat stretches served.
	rechecks *rechecker
	// publicState is the signed-out viewers' live state
	// (Server.public), made once by publicOnce.
	publicOnce  sync.Once
	publicState *publicLive
}

// New returns the service's handler: the routes wrapped in request
// logging.
func New(d Deps) http.Handler {
	s := &Server{
		log:         d.Log,
		ro:          d.RO,
		app:         d.App,
		signer:      d.Signer,
		cookies:     d.Cookies,
		limiter:     d.Limiter,
		acctLimiter: d.AcctLimiter,
		verifier:    &auth.Verifier{Cookies: d.Cookies, State: auth.DBState(d.App)},
		daemon:      d.Daemon,
		tokens:      d.Tokens,
		access:      d.Access,
		media:       d.Media,
		proxies:     d.Proxies,
		live:        d.Live,
		liveCfg:     d.LiveCfg,
		render:      d.Render,
		assets:      d.Assets,
		assetFiles:  d.AssetFiles,
		defaults:    d.Defaults.OrSite(),
		pages:       d.pages,
		clock:       time.Now,
	}
	// Account mail (email.go); zero lifetimes and nil limiters are the
	// defaults.
	s.mail, s.mailCfg = d.Mail, d.MailCfg
	s.forgotLimiter, s.forgotAcct = d.ForgotLimiter, d.ForgotAcctLimiter
	s.engine = d.SearchEngine
	s.bots = d.Bots
	if s.mailCfg.InviteTTL <= 0 {
		s.mailCfg.InviteTTL = DefaultInviteTTL
	}
	if s.mailCfg.ResetTTL <= 0 {
		s.mailCfg.ResetTTL = DefaultResetTTL
	}
	if s.mailCfg.VerifyTTL <= 0 {
		s.mailCfg.VerifyTTL = DefaultVerifyTTL
	}
	if s.forgotLimiter == nil {
		s.forgotLimiter = auth.NewRateLimiter(forgotPerAddress, forgotWindow)
	}
	if s.forgotAcct == nil {
		s.forgotAcct = auth.NewRateLimiter(forgotPerAccount, forgotWindow)
	}
	if s.pages == nil {
		s.pages = defaultPages()
	}
	s.mcpCfg = d.MCP
	s.mcpSlots = make(chan struct{}, max(d.MCP.Threads, 1))
	s.mcp = mcp.NewTelegramServer(d.RO, s.mcpFileURL)
	if d.state != nil {
		s.verifier.State = d.state
	}
	if d.clock != nil {
		s.clock = d.clock
	}
	s.photoAsks = d.photoAsks
	if s.photoAsks == nil {
		s.photoAsks = newPhotoRefresher(d.Log, d.Daemon, s.clock)
	}
	s.logger = d.logger
	if s.logger == nil {
		s.logger = newLoggerAccount(d.Log, d.Daemon, s.clock)
	}
	s.emoji = d.emoji
	if s.emoji == nil {
		s.emoji = newEmojiResolver(d.Log, d.Daemon, s.clock)
	}
	s.rechecks = d.rechecks
	if s.rechecks == nil {
		s.rechecks = newRechecker(d.Log, d.Daemon, s.clock, d.Recheck)
	}
	s.series = logdb.NewSeriesCache(logdb.SeriesCachePeriods, logdb.SeriesCacheTTL)
	s.search = logdb.Search
	if d.search != nil {
		s.search = d.search
	}
	s.viewerPrefs = func(ctx context.Context, uid uint64) (settings.Prefs, error) {
		return settings.Load(ctx, s.app, uid)
	}
	if d.prefs != nil {
		s.viewerPrefs = d.prefs
	}
	s.viewerPins = s.pinnedGroups
	if d.pins != nil {
		s.viewerPins = d.pins
	}
	s.statfs = d.statfs
	if s.statfs == nil {
		s.statfs = fsSize
	}
	s.discord = d.discord
	if s.discord == nil {
		s.discord = &http.Client{
			Timeout: discordTimeout,
			// A redirect could lead away from Discord's hosts.
			CheckRedirect: func(*http.Request, []*http.Request) error {
				return http.ErrUseLastResponse
			},
		}
	}

	mux := http.NewServeMux()
	for _, rt := range s.routes() {
		mux.HandleFunc(rt.pattern, rt.handler)
	}
	s.registerPages(mux)

	return s.withClient(s.logRequests(withCounts(d.Counts, mux)))
}

// route is one entry of the route table.
type route struct {
	pattern string // ServeMux pattern, "METHOD /path"
	handler http.HandlerFunc
}

// routes is the service's route table. It only builds handler values
// and touches none of s's fields, so the OpenAPI test lists it from a
// zero Server; every operation served must be in webapi/openapi.yaml.
func (s *Server) routes() []route {
	return []route{
		{"GET /api/health", s.health},
		{"GET /api/login", s.getLogin},
		{"POST /api/login", s.postLogin},
		{"POST /api/logout", s.postLogout},
		{"GET /api/session", s.RequireSession(s.session, allGates...)},
		{"GET /files/{token}", s.serveFile},
		{"GET /api/events", s.events},
		{"GET /api/page/{path...}", s.getPage},
		{"GET /app/assets/{file...}", s.serveAsset},
		{"POST /api/account/password", s.RequireSession(s.changePassword, scope.PasswordChange)},
		{"GET /api/accounts", s.RequireSession(s.listAccounts)},
		{"POST /api/accounts", s.RequireSession(s.createAccount)},
		{"GET /api/accounts/{id}", s.RequireSession(s.getAccount)},
		{"PATCH /api/accounts/{id}", s.RequireSession(s.updateAccount)},
		{"POST /api/accounts/{id}/disable", s.RequireSession(s.disableAccount)},
		{"POST /api/accounts/{id}/enable", s.RequireSession(s.enableAccount)},
		{"POST /api/accounts/{id}/password-reset", s.RequireSession(s.resetAccountPassword)},
		{"POST /api/accounts/{id}/sign-out-everywhere", s.RequireSession(s.signOutAccountEverywhere)},
		{"GET /api/chats/{scope}/{id}/messages", s.getChatMessages},
		{"GET /api/entities", s.RequireSession(s.getEntities)},
		{"GET /api/search/{entity}", s.RequireSession(s.searchEntity)},
		{"GET /api/search/capabilities", s.RequireSession(s.searchCapabilities)},
		{"GET /api/chats/search", s.RequireSession(s.searchChats)},
		{"PUT /api/account/preferences", s.RequireSession(s.putPreferences)},
		{"PATCH /api/account/preferences", s.RequireSession(s.patchPreferences)},
		{"GET /api/account/preferences", s.RequireSession(s.getPreferences)},
		{"GET /api/dashboard/kpis", s.RequireSession(s.dashboardKPIs)},
		{"GET /api/dashboard/series", s.RequireSession(s.dashboardSeries)},
		{"PUT /api/account/pinned/{group_id}", s.RequireSession(s.pinGroup)},
		{"DELETE /api/account/pinned/{group_id}", s.RequireSession(s.unpinGroup)},
		{"POST /api/account/saved-searches", s.RequireSession(s.saveSearch)},
		{"DELETE /api/account/saved-searches/{id}", s.RequireSession(s.deleteSavedSearch)},
		{"GET /api/account/sessions", s.RequireSession(s.listSessions)},
		{"DELETE /api/account/sessions/{id}", s.RequireSession(s.endSession)},
		{"POST /api/account/sessions/sign-out-others", s.RequireSession(s.endOtherSessions)},
		{"POST /api/federations", s.RequireSession(s.createFederation)},
		{"PATCH /api/federations/{id}", s.RequireSession(s.updateFederation)},
		{"DELETE /api/federations/{id}", s.RequireSession(s.deleteFederation)},
		{"GET /api/federations/{id}", s.RequireSession(s.getFederation)},
		{"GET /api/federations/{id}/groups", s.RequireSession(s.getFederationGroups)},
		{"POST /api/forwarding/telegram-discord", s.RequireSession(s.createT2D)},
		{"POST /api/forwarding/telegram-discord/test", s.RequireSession(s.testT2D)},
		{"PATCH /api/forwarding/telegram-discord/{id}", s.RequireSession(s.updateT2D)},
		{"DELETE /api/forwarding/telegram-discord/{id}", s.RequireSession(s.deleteT2D)},
		{"POST /api/forwarding/telegram-discord/{id}/transfer", s.RequireSession(s.transferT2D)},
		{"POST /api/forwarding/discord-telegram", s.RequireSession(s.createD2T)},
		{"PATCH /api/forwarding/discord-telegram/{id}", s.RequireSession(s.updateD2T)},
		{"DELETE /api/forwarding/discord-telegram/{id}", s.RequireSession(s.deleteD2T)},
		{"POST /api/forwarding/discord-telegram/{id}/transfer", s.RequireSession(s.transferD2T)},
		{"GET /api/forwarding/federations", s.RequireSession(s.fwdFederations)},
		{"GET /api/bots", s.RequireSession(s.listBots)},
		{"POST /api/bots", s.RequireSession(s.createBot)},
		{"PATCH /api/bots/{id}", s.RequireSession(s.updateBot)},
		{"DELETE /api/bots/{id}", s.RequireSession(s.deleteBot)},
		{"POST /api/bots/{id}/transfer", s.RequireSession(s.transferBot)},
		{"POST /api/join-chat/preview", s.RequireSession(s.previewJoin)},
		{"POST /api/join-chat/join", s.RequireSession(s.joinChat)},
		{"DELETE /api/join-chat/pending/{id}", s.RequireSession(s.cancelJoinFiling)},
		{"POST /api/users/{id}/lookup-name", s.RequireSession(s.lookupName)},
		{"POST /api/files/{token}/fetch", s.RequireSession(s.fetchFile)},
		{"POST /api/mcp/groups", s.RequireSession(s.exposeMcpGroup)},
		{"DELETE /api/mcp/groups/{group_id}", s.RequireSession(s.unexposeMcpGroup)},
		{"PATCH /api/mcp/groups/{group_id}", s.RequireSession(s.setMcpGroupNote)},
		{"POST /api/mcp/tokens", s.RequireSession(s.createMcpToken)},
		{"PATCH /api/mcp/tokens/{id}", s.RequireSession(s.renameMcpToken)},
		{"POST /api/mcp/tokens/{id}/revoke", s.RequireSession(s.revokeMcpToken)},
		{"POST /api/mcp/tokens/{id}/transfer", s.RequireSession(s.transferMcpToken)},
		{"POST /mcp", s.postMCP},
		{"GET /mcp", s.getMCP},
		{"DELETE /mcp", s.deleteMCP},
		{"POST /api/account/email", s.RequireSession(s.changeEmail)},
		{"POST /api/account/email/verify", s.verifyEmail},
		{"POST /api/password/set", s.setPassword},
		{"POST /api/password/forgot", s.forgotPassword},
		{"POST /api/accounts/{id}/resend-invite", s.RequireSession(s.resendInvite)},
		{"GET /api/users/{id}/history", s.RequireSession(s.getUserHistory)},
		{"GET /api/users/{id}/photos", s.RequireSession(s.getUserPhotos)},
		{"GET /api/groups/{id}/history", s.RequireSession(s.getGroupHistory)},
		{"GET /api/groups/{id}/photos", s.RequireSession(s.getGroupPhotos)},
		{"PATCH /api/groups/{id}", s.RequireSession(s.updateGroup)},
		{"GET /api/groups/{id}/members", s.getGroupMembers},
		// Public for an exposed group, as its chat is (tabRequest).
		{"GET /api/groups/{id}/files", s.getGroupFiles},
		{"GET /api/groups/{id}/polls", s.getGroupPolls},
	}
}

// healthTimeout bounds each pool's check, so a database that does not
// answer turns into a 503 rather than a hanging probe.
const healthTimeout = 3 * time.Second

// health checks both database pools. The answer says only which pool
// failed; the reason goes to the log, since the endpoint needs no login.
func (s *Server) health(w http.ResponseWriter, r *http.Request) {
	checks := map[string]string{}
	status := http.StatusOK

	for _, p := range []struct {
		name string
		db   *sql.DB
	}{{"ro", s.ro}, {"app", s.app}} {
		ctx, cancel := context.WithTimeout(r.Context(), healthTimeout)
		err := p.db.PingContext(ctx)
		cancel()
		if err != nil {
			s.log.Warn("health check failed", "pool", p.name, "err", err)
			checks[p.name] = "unavailable"
			status = http.StatusServiceUnavailable
			continue
		}
		checks[p.name] = "ok"
	}

	body := map[string]any{"status": "ok", "checks": checks}
	if status != http.StatusOK {
		body["status"] = "unavailable"
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, status, body)
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

// statusWriter remembers the status code a handler sent, for the log.
type statusWriter struct {
	http.ResponseWriter
	status int
}

func (w *statusWriter) WriteHeader(code int) {
	w.status = code
	w.ResponseWriter.WriteHeader(code)
}

// ReadFrom passes io.Copy through to the underlying writer, which
// sends files with sendfile(2); without it every file served would be
// copied through a buffer.
func (w *statusWriter) ReadFrom(r io.Reader) (int64, error) {
	return io.Copy(w.ResponseWriter, r)
}

// Unwrap lets http.ResponseController reach the underlying writer
// (flushing, deadlines) through this wrapper.
func (w *statusWriter) Unwrap() http.ResponseWriter {
	return w.ResponseWriter
}

func (s *Server) logRequests(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		sw := &statusWriter{ResponseWriter: w, status: http.StatusOK}
		next.ServeHTTP(sw, r)
		s.log.Info("request",
			"method", r.Method,
			"path", r.URL.Path,
			"status", sw.status,
			"duration", time.Since(start),
			"client", clientIP(r),
			"remote", r.RemoteAddr)
	})
}

// withCounts gives every request the count cache, for the logdb
// queries it serves.
func withCounts(c *logdb.CountCache, next http.Handler) http.Handler {
	if c == nil {
		return next
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		next.ServeHTTP(w, r.WithContext(logdb.WithCountCache(r.Context(), c)))
	})
}
