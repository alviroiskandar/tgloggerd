// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

// Command webapi is the Go web service of tgloggerd. It serves the JSON
// API under /api/ beside the Drogon web (docs/design/web-go.md).
//
// With arguments it runs a one-off command instead and exits:
//
//	webapi assign-owner <username>
//	webapi import-discord-bot <username>
//
// give every resource without an owner to that account (assign.go),
// and store the Discord bot of DISCORD_BOT_TOKEN, owned by that
// account, for the routes that name none (importbot.go).
package main

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"sync"
	"syscall"
	"time"

	"github.com/go-sql-driver/mysql"

	"tgloggerd/webapi/internal/auth"
	"tgloggerd/webapi/internal/config"
	"tgloggerd/webapi/internal/daemon"
	"tgloggerd/webapi/internal/live"
	"tgloggerd/webapi/internal/logdb"
	"tgloggerd/webapi/internal/mail"
	"tgloggerd/webapi/internal/media"
	"tgloggerd/webapi/internal/render"
	"tgloggerd/webapi/internal/searchengine"
	"tgloggerd/webapi/internal/server"
)

// shutdownTimeout bounds how long requests in flight may run after
// SIGTERM. It stays under the 10 seconds docker stop waits before it
// kills the container.
const shutdownTimeout = 8 * time.Second

// openPool opens one database pool. sql.Open connects lazily, so the
// service starts even while MySQL is still coming up; the health
// endpoint reports it until it answers.
func openPool(log *slog.Logger, c config.DB) (*sql.DB, error) {
	mc := mysql.NewConfig()
	// The driver's own messages (a connection dropped mid-query, ...)
	// go to the service log, tagged with the pool.
	mc.Logger = slog.NewLogLogger(log.With("pool", c.Name).Handler(), slog.LevelWarn)
	mc.Net = "tcp"
	mc.Addr = net.JoinHostPort(c.Host, strconv.Itoa(int(c.Port)))
	mc.User = c.User
	mc.Passwd = c.Password
	mc.DBName = c.DBName
	// Values are put into the statement by the driver, as the Drogon
	// web's client does, instead of preparing it on the server. The
	// search's relevance sort needs that: MySQL orders by the fulltext
	// index only when the MATCH of ORDER BY is the one of WHERE, which
	// two placeholders of a prepared statement are not, so it would
	// sort every hit instead. It also saves the prepare and close round
	// trips of every query with values.
	mc.InterpolateParams = true

	connector, err := mysql.NewConnector(mc)
	if err != nil {
		return nil, err
	}
	db := sql.OpenDB(connector)
	db.SetMaxOpenConns(c.Conns)
	db.SetMaxIdleConns(c.Conns)
	// Retire connections before the server's wait_timeout drops them,
	// so a request rarely meets a dead connection after a quiet spell.
	db.SetConnMaxIdleTime(5 * time.Minute)
	db.SetConnMaxLifetime(time.Hour)
	return db, nil
}

func run(log *slog.Logger, cfg *config.Config) error {
	ro, err := openPool(log, cfg.RO)
	if err != nil {
		return err
	}
	defer ro.Close()
	app, err := openPool(log, cfg.App)
	if err != nil {
		return err
	}
	defer app.Close()

	dc, err := daemon.New(cfg.DaemonAPI, cfg.AppKey, cfg.FetchTimeout)
	switch {
	case err != nil:
		log.Warn("the daemon API stays off", "err", err)
	case dc.Enabled():
		log.Info("daemon API", "url", dc.BaseURL())
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, os.Interrupt)
	defer stop()

	// Served files are reported to the daemon once a minute, for its
	// eviction order; the last minute's are dropped on shutdown, as a
	// failed report's are.
	access := daemon.NewAccessReporter(dc, log)
	go access.Run(ctx)

	tdlib := media.TDLibDir(cfg.DataDir)
	if tdlib == "" {
		log.Warn("TG_DATA_DIR is not set; big files cannot be streamed from TDLib")
	}

	// One subscription to the daemon's events feeds every browser's
	// GET /api/events. The hub stops with ctx, before the server's
	// shutdown, so open streams do not hold the shutdown up.
	var hub *live.Hub
	switch {
	case !cfg.Live:
		log.Info("live updates are off (WEBAPI_LIVE=0)")
	case !dc.Enabled():
		log.Warn("live updates are off: they need WEB_TGLOGGERD_API")
	default:
		hub = live.NewHub(live.DaemonSource{Client: dc, Log: log}, live.Options{
			History:        cfg.LiveHistory,
			Buffer:         cfg.LiveBuffer,
			MaxSubscribers: cfg.LiveMaxSubscribers,
			Log:            log,
		})
		go hub.Run(ctx)
	}
	engine, err := startSearchEngine(ctx, log, cfg, hub, dc)
	if err != nil {
		return err
	}

	// Pages link the client bundle vite build wrote; without it they
	// still answer, with their data, but no script to show it.
	assets, err := render.LoadAssets(cfg.AppDist, cfg.AppBuild)
	if err != nil {
		log.Warn("no client bundle; pages link no assets", "err", err)
	}
	var assetFiles *os.Root
	if root, err := os.OpenRoot(filepath.Join(cfg.AppDist, "assets")); err == nil {
		defer root.Close()
		assetFiles = root
	}
	renderer := render.NewClient(cfg.RenderSocket, cfg.RenderTimeout, cfg.RenderBackoff)
	log.Info("render service", "socket", renderer.Socket(), "build", assets.Build)

	// Account mail: links and notices go out from a queue of their own,
	// so no request waits for the relay. Messages still queued at
	// shutdown are lost (internal/mail says why).
	var mailer *mail.Mailer
	if cfg.Mail == nil {
		log.Warn("mail is off (no WEBAPI_SMTP_HOST): no links or notices are sent; " +
			"new accounts and password resets use temporary passwords, and invitations by email are refused")
	} else {
		if mailer, err = mail.New(*cfg.Mail, log.With("component", "mail")); err != nil {
			return err
		}
		go mailer.Run(ctx)
		log.Info("mail", "relay", net.JoinHostPort(cfg.Mail.Host, strconv.Itoa(cfg.Mail.Port)),
			"security", cfg.Mail.Security, "site", cfg.PublicURL)
	}

	// The Telegram bots' own users are resolved through the daemon in
	// the background, so the bots can link to their user pages; with
	// the daemon API off nothing runs.
	bots := server.NewBotResolver(log.With("component", "bot-users"), ro, dc)
	go bots.Run(ctx)

	// Groups joined pending approval are filed into their chosen
	// federations once the account is a member; the sweep stops with
	// ctx.
	go server.NewJoinFiler(log.With("component", "join-filings"), ro, app).Run(ctx)

	signer := auth.NewSigner(cfg.AppKey)
	var acctLimiter *auth.RateLimiter
	if cfg.LoginMaxPerAccount > 0 {
		acctLimiter = auth.NewRateLimiter(cfg.LoginMaxPerAccount, cfg.LoginWindow)
	}
	handler := server.New(server.Deps{
		Log:    log,
		RO:     ro,
		App:    app,
		Signer: signer,
		Cookies: &auth.Cookies{
			Signer:  signer,
			Timeout: cfg.SessionTimeout,
			Secure:  cfg.SecureCookie,
		},
		Limiter:     auth.NewRateLimiter(cfg.LoginMaxAttempts, cfg.LoginWindow),
		AcctLimiter: acctLimiter,
		Daemon:      dc,
		Tokens:      auth.NewFileTokens(cfg.AppKey),
		Access:      access,
		Media: server.MediaConfig{
			StorageDir:   cfg.StorageDir,
			TDLibDir:     tdlib,
			StoreFileCap: cfg.StoreFileCap,
		},
		Proxies:    server.TrustedProxies(cfg.TrustedProxies),
		Live:       hub,
		LiveCfg:    server.LiveConfig{Recheck: cfg.LiveRecheck, Public: server.PublicLiveConfig(cfg.LivePublic)},
		Counts:     logdb.NewCountCache(cfg.CountCache),
		Render:     renderer,
		Assets:     assets,
		AssetFiles: assetFiles,
		Defaults:   cfg.Defaults,
		MCP: server.MCPConfig{
			PublicURL:      cfg.PublicURL,
			Threads:        cfg.MCPThreads,
			AllowedOrigins: cfg.MCPAllowedOrigins,
			LogRequests:    cfg.MCPLogRequests,
		},
		SearchEngine: engine,
		Bots:         bots,
		Mail:         mailer,
		MailCfg: server.MailConfig{
			Site:      cfg.PublicURL,
			InviteTTL: cfg.InviteLinkTTL,
			ResetTTL:  cfg.ResetLinkTTL,
			VerifyTTL: cfg.VerifyLinkTTL,
		},
		Recheck: server.RecheckConfig(cfg.Recheck),
	})

	addr := net.JoinHostPort(cfg.ListenAddr, strconv.Itoa(int(cfg.ListenPort)))
	srv := &http.Server{
		Addr:              addr,
		Handler:           handler,
		ReadHeaderTimeout: 10 * time.Second,
		IdleTimeout:       2 * time.Minute,
		ErrorLog:          slog.NewLogLogger(log.Handler(), slog.LevelWarn),
	}

	errc := make(chan error, 1)
	go func() {
		log.Info("listening", "addr", addr)
		errc <- srv.ListenAndServe()
	}()

	select {
	case err := <-errc:
		// The listener failed (address in use, ...) before any signal.
		return err
	case <-ctx.Done():
	}

	log.Info("shutting down")
	sctx, cancel := context.WithTimeout(context.Background(), shutdownTimeout)
	defer cancel()
	if err := srv.Shutdown(sctx); err != nil {
		return err
	}
	if err := <-errc; !errors.Is(err, http.ErrServerClosed) {
		return err
	}
	log.Info("stopped")
	return nil
}

// startSearchEngine returns the search engine for the searches, nil
// when it is not configured, and starts its indexers when this instance
// is the indexer: the messages', the users' and the groups'. They read
// the tables through a pool of their own with one connection, so a
// backfill never takes a connection the pages need, and take their
// events from the live hub; without one, from a hub of their own over
// the daemon's stream, so the three hold one subscription of the
// daemon's. Without the daemon's API the reconciles alone keep the
// indices in step.
func startSearchEngine(ctx context.Context, log *slog.Logger, cfg *config.Config,
	hub *live.Hub, dc *daemon.Client) (server.SearchEngine, error) {
	se := cfg.SearchEngine
	if se.URL == "" {
		log.Info("search engine: off (no WEBAPI_SEARCH_ENGINE_URL); search uses MySQL")
		return nil, nil
	}
	c, err := searchengine.NewClient(se.URL)
	if err != nil {
		return nil, fmt.Errorf("WEBAPI_SEARCH_ENGINE_URL: %w", err)
	}
	x, err := c.Index(se.Index)
	if err != nil {
		return nil, fmt.Errorf("WEBAPI_SEARCH_ENGINE_INDEX: %w", err)
	}
	ux, err := c.ProfileIndex(se.UsersIndex, searchengine.ProfileUsers)
	if err != nil {
		return nil, fmt.Errorf("WEBAPI_SEARCH_ENGINE_USERS_INDEX: %w", err)
	}
	gx, err := c.ProfileIndex(se.GroupsIndex, searchengine.ProfileGroups)
	if err != nil {
		return nil, fmt.Errorf("WEBAPI_SEARCH_ENGINE_GROUPS_INDEX: %w", err)
	}
	engine := searchengine.NewEngine(x, searchengine.EngineOptions{
		Timeout: se.Timeout,
		MaxLag:  se.MaxLag,
		Users:   ux,
		Groups:  gx,
		Log:     log.With("component", "search-engine"),
	})
	// A cold engine answers its first searches slowly: warm it up
	// before the searches use it.
	engine.StartWarmUp(ctx)
	if !se.Indexer {
		log.Info("search engine: another instance indexes (WEBAPI_SEARCH_ENGINE_INDEXER=0)",
			"url", c.URL(), "index", x.Name(), "users_index", ux.Name(),
			"groups_index", gx.Name())
		return engine, nil
	}
	pc := cfg.RO
	pc.Name, pc.Conns = "index", 1
	pool, err := openPool(log, pc)
	if err != nil {
		return nil, err
	}
	if hub == nil && dc.Enabled() {
		// Live updates are off: the indexers' own hub, which stops
		// with ctx.
		hub = live.NewHub(live.DaemonSource{Client: dc, Log: log}, live.Options{Log: log})
		go hub.Run(ctx)
	}
	var src, psrc live.Source
	if hub != nil {
		src = searchengine.HubSource{Hub: hub}
		psrc = searchengine.HubSource{Hub: hub, Want: searchengine.ProfileEvent}
	} else {
		log.Warn("search engine: no event stream (WEB_TGLOGGERD_API); " +
			"the indices follow the archive by their reconciles alone")
	}
	o := searchengine.IndexerOptions{
		Batch:     se.Batch,
		Pause:     se.Pause,
		Reconcile: se.Reconcile,
		Log:       log.With("component", "search-index"),
	}
	ix := searchengine.NewIndexer(pool, x, src, o)
	uix, err := searchengine.NewProfileIndexer(pool, ux, psrc, o)
	if err != nil {
		pool.Close()
		return nil, err
	}
	gix, err := searchengine.NewProfileIndexer(pool, gx, psrc, o)
	if err != nil {
		pool.Close()
		return nil, err
	}
	var wg sync.WaitGroup
	for _, run := range []func(context.Context){ix.Run, uix.Run, gix.Run} {
		wg.Add(1)
		go func() {
			defer wg.Done()
			run(ctx)
		}()
	}
	go func() {
		wg.Wait()
		pool.Close()
	}()
	log.Info("search engine: indexing", "url", c.URL(), "index", x.Name(),
		"users_index", ux.Name(), "groups_index", gx.Name())
	return engine, nil
}

// commandTimeout bounds a one-off command.
const commandTimeout = 5 * time.Minute

// command runs the one-off command in args and returns the exit
// status: 0 done, 1 failed or refused, 2 a usage error.
func command(args []string) int {
	usage := func() int {
		fmt.Fprintln(os.Stderr, "usage: webapi assign-owner <username>\n"+
			"       webapi import-discord-bot <username>")
		return 2
	}
	name := args[0]
	if name != "assign-owner" && name != "import-discord-bot" || len(args) != 2 || args[1] == "" {
		return usage()
	}
	cfg, err := config.FromEnv()
	if err != nil {
		fmt.Fprintln(os.Stderr, "configuration:", err)
		return 2
	}
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: cfg.LogLevel}))
	ro, err := openPool(log, cfg.RO)
	if err != nil {
		fmt.Fprintln(os.Stderr, name+":", err)
		return 1
	}
	defer ro.Close()
	app, err := openPool(log, cfg.App)
	if err != nil {
		fmt.Fprintln(os.Stderr, name+":", err)
		return 1
	}
	defer app.Close()
	ctx, cancel := context.WithTimeout(context.Background(), commandTimeout)
	defer cancel()
	if name == "assign-owner" {
		err = assignOwner(ctx, ro, app, args[1], os.Stdout)
	} else {
		err = importDiscordBot(ctx, ro, app, args[1], os.Getenv("DISCORD_BOT_TOKEN"), os.Stdout)
	}
	if err != nil {
		var refused errRefused
		if errors.As(err, &refused) {
			fmt.Fprintln(os.Stderr, name+": refused:", err)
		} else {
			fmt.Fprintln(os.Stderr, name+":", err)
		}
		return 1
	}
	return 0
}

func main() {
	if len(os.Args) > 1 {
		os.Exit(command(os.Args[1:]))
	}
	cfg, err := config.FromEnv()
	if err != nil {
		slog.Error("configuration", "err", err)
		os.Exit(2)
	}
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: cfg.LogLevel}))
	slog.SetDefault(log)

	if err := run(log, cfg); err != nil {
		log.Error("exiting", "err", err)
		os.Exit(1)
	}
}
