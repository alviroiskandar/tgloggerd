// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

// Package config reads the service's settings from the environment.
//
// The database settings are the Drogon web's (web/src/Config.hpp), with
// the same names, defaults and fallbacks, so one .env serves both web
// services while they run side by side; so are the key, session, login
// and daemon settings, which must agree for sessions and tokens to work
// in both. Only the listening address is the service's own (WEBAPI_*),
// since both listen at the same time.
package config

import (
	"fmt"
	"log/slog"
	"net/netip"
	"os"
	"strconv"
	"strings"
	"time"

	"tgloggerd/webapi/internal/auth"
	"tgloggerd/webapi/internal/daemon"
	"tgloggerd/webapi/internal/mail"
	"tgloggerd/webapi/internal/settings"
)

// DB is one database pool's settings.
type DB struct {
	Name     string // pool name in logs and health output: "ro" or "app"
	Host     string
	Port     uint16
	DBName   string
	User     string
	Password string
	Conns    int // most open connections in the pool
}

// Config is the whole service configuration.
type Config struct {
	ListenAddr string
	ListenPort uint16
	LogLevel   slog.Level

	RO  DB // SELECT-only over the logger's schema
	App DB // read-write over the web's own schema (accounts, audit)

	// AppKey is WEB_APP_KEY, decoded: it signs sessions and CSRF
	// tokens, encrypts file tokens and keys the daemon API.
	AppKey []byte
	// SessionTimeout is WEB_SESSION_TIMEOUT in seconds; <= 0 = never.
	SessionTimeout int
	// SecureCookie is WEB_SECURE_COOKIE != "0".
	SecureCookie bool
	// LoginMaxAttempts failed logins per (ip, username) are allowed
	// within LoginWindow (WEB_LOGIN_MAX_ATTEMPTS, WEB_LOGIN_WINDOW).
	LoginMaxAttempts int
	LoginWindow      time.Duration
	// LoginMaxPerAccount failed logins per account, from all addresses
	// together, are allowed within LoginWindow
	// (WEB_LOGIN_MAX_ATTEMPTS_PER_ACCOUNT); 0 turns the cap off.
	LoginMaxPerAccount int

	// DaemonAPI is WEB_TGLOGGERD_API, "" when the daemon API is off;
	// FetchTimeout is how long a fetch through it may take.
	DaemonAPI    string
	FetchTimeout time.Duration

	// StorageDir is WEB_STORAGE_DIR, the file store's root; it must be
	// the daemon's TG_STORAGE_DIR.
	StorageDir string
	// DataDir is the daemon's TG_DATA_DIR, whose tdlib/ holds the
	// partial files of big fetches; "" when unset.
	DataDir string
	// StoreFileCap is the daemon's TG_MAX_STORE_FILE_SIZE: files under
	// it are fetched whole into the store. 0 = no cap.
	StoreFileCap uint64

	// TrustedProxies is WEBAPI_TRUSTED_PROXIES: the reverse proxies
	// (and the networks of the ones in front of them) whose
	// X-Forwarded-For hops are skipped to find a request's client.
	// Empty: the connection's peer is the client.
	TrustedProxies []netip.Prefix

	// Live is WEBAPI_LIVE != "0": GET /api/events, fed by the daemon's
	// event stream (so only with DaemonAPI set).
	Live bool
	// LiveMaxSubscribers caps the open event streams
	// (WEBAPI_LIVE_MAX_SUBSCRIBERS); LiveBuffer is how many events may
	// wait for one (WEBAPI_LIVE_BUFFER); LiveHistory how many recent
	// events are kept for streams that reconnect (WEBAPI_LIVE_HISTORY).
	LiveMaxSubscribers int
	LiveBuffer         int
	LiveHistory        int
	// LiveRecheck is how often an open stream checks its session again
	// (WEBAPI_LIVE_RECHECK, seconds).
	LiveRecheck time.Duration
	// LivePublic limits the signed-out viewers' event streams
	// (WEBAPI_LIVE_PUBLIC_*; webapi/README, "Live updates").
	LivePublic LivePublic
	// Recheck limits the chat rechecks asked of the daemon
	// (WEBAPI_RECHECK_*; webapi/README, "Recheck").
	Recheck Recheck
	// CountCache is how long whole-table and list counts are kept
	// (WEBAPI_COUNT_CACHE_SECS, default 60; 0 counts on every request).
	CountCache time.Duration
	// RenderSocket is the render service's Unix socket (RENDER_SOCKET);
	// RenderTimeout bounds a render, connect and answer together
	// (RENDER_TIMEOUT), and RenderBackoff is how long the service is
	// skipped after it failed to answer (RENDER_BACKOFF).
	RenderSocket  string
	RenderTimeout time.Duration
	RenderBackoff time.Duration
	// AppDist is the client bundle vite build wrote (WEBAPI_APP_DIST),
	// with .vite/manifest.json and assets/; AppBuild is its build id
	// (WEBAPI_APP_BUILD), empty for the one the build recorded in the
	// bundle (render.ReadBuild).
	AppDist  string
	AppBuild string
	// Defaults are the site's settings for accounts that did not choose
	// their own (WEBAPI_DEFAULT_TIMEZONE, _DATE_FORMAT, _LANG, _THEME).
	Defaults settings.Defaults

	// PublicURL is the site's public base URL, for absolute
	// /files/<token> links in MCP answers (WEB_PUBLIC_URL, falling back
	// to the daemon's TG_DISCORD_PUBLIC_URL, which builds the same
	// links), trailing slashes and blanks dropped; "" when neither is
	// set.
	PublicURL string
	// MCPThreads is how many MCP tool calls run at once (MCP_THREADS,
	// default 2, 1 to 16).
	MCPThreads int
	// MCPAllowedOrigins is MCP_ALLOWED_ORIGINS: browser origins allowed
	// to call /mcp, comma-separated, or "*"; empty allows any.
	MCPAllowedOrigins string
	// MCPLogRequests is MCP_LOG_REQUESTS == "1": log the shape of the
	// requests to /mcp.
	MCPLogRequests bool
	// Mail is the SMTP relay (WEBAPI_SMTP_*), nil when
	// WEBAPI_SMTP_HOST is unset: no mail is sent (email is optional).
	// The links in mail point to PublicURL.
	Mail *mail.Config
	// SearchEngine is the optional search engine (WEBAPI_SEARCH_ENGINE_*,
	// webapi/README "Search engine"); its URL is "" when it is off.
	SearchEngine SearchEngine
	// InviteLinkTTL, ResetLinkTTL and VerifyLinkTTL are how long the
	// links sent work (WEBAPI_INVITE_LINK_TTL, _RESET_, _VERIFY_,
	// seconds).
	InviteLinkTTL time.Duration
	ResetLinkTTL  time.Duration
	VerifyLinkTTL time.Duration
}

// SearchEngine is the search engine's settings.
type SearchEngine struct {
	// URL is WEBAPI_SEARCH_ENGINE_URL, the engine's base URL; "" turns
	// the engine off and nothing changes.
	URL string
	// Index is the messages index's name (WEBAPI_SEARCH_ENGINE_INDEX);
	// UsersIndex and GroupsIndex the profile indices' of the users'
	// bios and the groups' descriptions
	// (WEBAPI_SEARCH_ENGINE_USERS_INDEX, _GROUPS_INDEX).
	Index       string
	UsersIndex  string
	GroupsIndex string
	// Indexer is WEBAPI_SEARCH_ENGINE_INDEXER != "0": this instance
	// keeps the index in step. One instance per index may.
	Indexer bool
	// Batch is the rows read per query of the backfill and the
	// reconcile (WEBAPI_SEARCH_ENGINE_BATCH), Pause the wait between
	// two (WEBAPI_SEARCH_ENGINE_PAUSE, milliseconds), Reconcile the
	// time between two reconciles (WEBAPI_SEARCH_ENGINE_RECONCILE,
	// seconds).
	Batch     int
	Pause     time.Duration
	Reconcile time.Duration
	// Timeout bounds one query of a search
	// (WEBAPI_SEARCH_ENGINE_TIMEOUT, milliseconds); MaxLag is how long
	// the index may go without being known in step before searches
	// use MySQL (WEBAPI_SEARCH_ENGINE_MAX_LAG, seconds).
	Timeout time.Duration
	MaxLag  time.Duration
}

// Recheck is how the chat rechecks are limited. Off is WEBAPI_RECHECK
// == "0". Newest and Older are the least time between two asks for a
// chat's newest messages and for the stretch around one anchor
// (WEBAPI_RECHECK_NEWEST_SECS, default 60; _OLDER_SECS, 3600). The
// signed-out visitors share one bucket: PublicPerMinute tokens a minute
// (WEBAPI_RECHECK_PUBLIC_PER_MIN, 20; 0 = they ask for none), at most
// PublicBurst (_PUBLIC_BURST, 20), of which older stretches leave
// PublicReserve to the newest (_PUBLIC_RESERVE, 10). Timeout bounds one
// ask (WEBAPI_RECHECK_TIMEOUT, milliseconds, 5000) and MaxInFlight is
// how many may wait at once (WEBAPI_RECHECK_MAX_IN_FLIGHT, 8). The
// fields are server.RecheckConfig's, in its order, so main converts it.
type Recheck struct {
	Off                                         bool
	Newest, Older                               time.Duration
	PublicPerMinute, PublicBurst, PublicReserve int
	Timeout                                     time.Duration
	MaxInFlight                                 int
}

// LivePublic is the limits of the signed-out viewers' event streams,
// in the shape of server.PublicLiveConfig. A zero setting turns the
// thing off and is read as -1 here: Max (no signed-out streams), Cache
// (no shared window cache).
type LivePublic struct {
	Max, PerAddr, OpensPerMinute, Topics, Buffer int
	Refetch, Cache, Exposure                     time.Duration
}

func livePublic() (LivePublic, error) {
	var p LivePublic
	var err error
	if p.Max, err = integer("WEBAPI_LIVE_PUBLIC_MAX", "200"); err != nil {
		return p, err
	}
	if p.Max < 0 {
		return p, fmt.Errorf("WEBAPI_LIVE_PUBLIC_MAX: %q is negative",
			env("WEBAPI_LIVE_PUBLIC_MAX", "200"))
	}
	if p.Max == 0 {
		p.Max = -1
	}
	for _, v := range []struct {
		key, def string
		dst      *int
	}{
		{"WEBAPI_LIVE_PUBLIC_PER_ADDR", "4", &p.PerAddr},
		{"WEBAPI_LIVE_PUBLIC_OPENS_PER_MINUTE", "10", &p.OpensPerMinute},
		{"WEBAPI_LIVE_PUBLIC_TOPICS", "4", &p.Topics},
		{"WEBAPI_LIVE_PUBLIC_BUFFER", "32", &p.Buffer},
	} {
		if *v.dst, err = positive(v.key, v.def); err != nil {
			return p, err
		}
	}
	n, err := positive("WEBAPI_LIVE_PUBLIC_REFETCH_SECS", "5")
	if err != nil {
		return p, err
	}
	p.Refetch = time.Duration(n) * time.Second
	if n, err = positive("WEBAPI_LIVE_PUBLIC_EXPOSURE_SECS", "10"); err != nil {
		return p, err
	}
	p.Exposure = time.Duration(n) * time.Second
	if n, err = integer("WEBAPI_LIVE_PUBLIC_CACHE_SECS", "2"); err != nil || n < 0 {
		return p, fmt.Errorf("WEBAPI_LIVE_PUBLIC_CACHE_SECS: %q is not a number of seconds",
			env("WEBAPI_LIVE_PUBLIC_CACHE_SECS", "2"))
	}
	p.Cache = time.Duration(n) * time.Second
	if n == 0 {
		p.Cache = -1
	}
	return p, nil
}

func recheckLimits() (Recheck, error) {
	var (
		r   Recheck
		err error
		n   int
	)
	r.Off = env("WEBAPI_RECHECK", "1") == "0"
	if n, err = positive("WEBAPI_RECHECK_NEWEST_SECS", "60"); err != nil {
		return r, err
	}
	r.Newest = time.Duration(n) * time.Second
	if n, err = positive("WEBAPI_RECHECK_OLDER_SECS", "3600"); err != nil {
		return r, err
	}
	r.Older = time.Duration(n) * time.Second
	for _, v := range []struct {
		key, def string
		dst      *int
	}{
		{"WEBAPI_RECHECK_PUBLIC_PER_MIN", "20", &r.PublicPerMinute},
		{"WEBAPI_RECHECK_PUBLIC_BURST", "20", &r.PublicBurst},
		{"WEBAPI_RECHECK_PUBLIC_RESERVE", "10", &r.PublicReserve},
	} {
		if *v.dst, err = integer(v.key, v.def); err != nil {
			return r, err
		}
		if *v.dst < 0 {
			return r, fmt.Errorf("%s: %q is negative", v.key, env(v.key, v.def))
		}
	}
	if r.PublicPerMinute > 0 && r.PublicReserve >= r.PublicBurst {
		return r, fmt.Errorf("WEBAPI_RECHECK_PUBLIC_RESERVE (%d) must be under "+
			"WEBAPI_RECHECK_PUBLIC_BURST (%d)", r.PublicReserve, r.PublicBurst)
	}
	if r.Timeout, err = millis("WEBAPI_RECHECK_TIMEOUT", "5000"); err != nil {
		return r, err
	}
	if r.MaxInFlight, err = positive("WEBAPI_RECHECK_MAX_IN_FLIGHT", "8"); err != nil {
		return r, err
	}
	return r, nil
}

// env returns the variable's value, or def when it is unset or empty.
// An empty value counts as unset, as in the daemon and the Drogon web.
func env(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func port(key, def string) (uint16, error) {
	s := env(key, def)
	n, err := strconv.ParseUint(s, 10, 16)
	if err != nil || n == 0 {
		return 0, fmt.Errorf("%s: %q is not a TCP port", key, s)
	}
	return uint16(n), nil
}

func integer(key, def string) (int, error) {
	s := env(key, def)
	n, err := strconv.Atoi(s)
	if err != nil {
		return 0, fmt.Errorf("%s: %q is not a number", key, s)
	}
	return n, nil
}

func positive(key, def string) (int, error) {
	s := env(key, def)
	n, err := strconv.Atoi(s)
	if err != nil || n <= 0 {
		return 0, fmt.Errorf("%s: %q is not a positive number", key, s)
	}
	return n, nil
}

// millis reads a duration given in milliseconds.
func millis(key, def string) (time.Duration, error) {
	s := env(key, def)
	n, err := strconv.Atoi(s)
	if err != nil || n <= 0 {
		return 0, fmt.Errorf("%s: %q is not a positive number of milliseconds", key, s)
	}
	return time.Duration(n) * time.Millisecond, nil
}

func level(key, def string) (slog.Level, error) {
	s := env(key, def)
	var l slog.Level
	if err := l.UnmarshalText([]byte(strings.ToUpper(s))); err != nil {
		return 0, fmt.Errorf("%s: %q is not one of debug, info, warn, error", key, s)
	}
	return l, nil
}

// trustedProxies reads a comma- or space-separated list of addresses
// and networks: "10.0.88.1", "173.245.48.0/20", "2400:cb00::/32".
func trustedProxies(key string) ([]netip.Prefix, error) {
	var out []netip.Prefix
	for _, f := range strings.FieldsFunc(os.Getenv(key), func(r rune) bool {
		return r == ',' || r == ' ' || r == '\t' || r == '\n'
	}) {
		if strings.Contains(f, "/") {
			p, err := netip.ParsePrefix(f)
			if err != nil {
				return nil, fmt.Errorf("%s: %q is not an address or network", key, f)
			}
			out = append(out, p.Masked())
			continue
		}
		a, err := netip.ParseAddr(f)
		if err != nil {
			return nil, fmt.Errorf("%s: %q is not an address or network", key, f)
		}
		a = a.Unmap()
		out = append(out, netip.PrefixFrom(a, a.BitLen()))
	}
	return out, nil
}

// FromEnv builds the configuration from the environment. A malformed
// number is an error rather than a silent zero, so a typo in .env stops
// the service at start instead of misconfiguring it.
func FromEnv() (*Config, error) {
	var (
		c   Config
		err error
	)

	c.ListenAddr = env("WEBAPI_LISTEN_ADDR", "127.0.0.1")
	if c.ListenPort, err = port("WEBAPI_LISTEN_PORT", "8081"); err != nil {
		return nil, err
	}
	if c.LogLevel, err = level("WEBAPI_LOG_LEVEL", "info"); err != nil {
		return nil, err
	}

	// WEB_DB_HOST/PORT set the server for both pools and fall back to
	// the daemon's TG_DB_HOST/PORT, as in the Drogon web.
	host := env("WEB_DB_HOST", env("TG_DB_HOST", "127.0.0.1"))
	dbPort, err := port("WEB_DB_PORT", env("TG_DB_PORT", "3306"))
	if err != nil {
		return nil, err
	}

	c.RO = DB{
		Name:     "ro",
		Host:     host,
		Port:     dbPort,
		DBName:   env("WEB_DB_RO_NAME", "tgloggerd"),
		User:     env("WEB_DB_RO_USER", "web_ro"),
		Password: env("WEB_DB_RO_PASSWORD", ""),
	}
	if c.RO.Conns, err = positive("WEB_DB_RO_CONNS", "4"); err != nil {
		return nil, err
	}

	c.App = DB{
		Name:     "app",
		Host:     host,
		Port:     dbPort,
		DBName:   env("WEB_DB_APP_NAME", "tgloggerd_web"),
		User:     env("WEB_DB_APP_USER", "web_app"),
		Password: env("WEB_DB_APP_PASSWORD", ""),
	}
	if c.App.Conns, err = positive("WEB_DB_APP_CONNS", "2"); err != nil {
		return nil, err
	}

	// Without the key nothing can be signed in or served, so a missing
	// or unusable one stops the service, as it stops the Drogon web.
	if c.AppKey, err = auth.DecodeAppKey(os.Getenv("WEB_APP_KEY")); err != nil {
		return nil, err
	}
	if c.SessionTimeout, err = integer("WEB_SESSION_TIMEOUT", "43200"); err != nil {
		return nil, err
	}
	c.SecureCookie = env("WEB_SECURE_COOKIE", "1") != "0"
	if c.LoginMaxAttempts, err = integer("WEB_LOGIN_MAX_ATTEMPTS", "5"); err != nil {
		return nil, err
	}
	window, err := integer("WEB_LOGIN_WINDOW", "900")
	if err != nil {
		return nil, err
	}
	c.LoginWindow = time.Duration(window) * time.Second
	if c.LoginMaxPerAccount, err = integer("WEB_LOGIN_MAX_ATTEMPTS_PER_ACCOUNT", "20"); err != nil {
		return nil, err
	}

	// A malformed WEB_TGLOGGERD_API only turns the daemon client off
	// (cmd/webapi logs it), as in the Drogon web, which works without
	// the daemon.
	c.DaemonAPI = env("WEB_TGLOGGERD_API", "")
	c.FetchTimeout = daemon.FetchTimeout(os.Getenv("TG_FETCH_TIMEOUT_SECS"))

	// The store and TDLib's directory are shared with the daemon, and
	// so are their settings, read from the same .env.
	c.StorageDir = env("WEB_STORAGE_DIR", "data/storage/files")
	c.DataDir = env("TG_DATA_DIR", "")
	capStr := env("TG_MAX_STORE_FILE_SIZE", "1073741824")
	if c.StoreFileCap, err = strconv.ParseUint(capStr, 10, 64); err != nil {
		return nil, fmt.Errorf("TG_MAX_STORE_FILE_SIZE: %q is not a size in bytes", capStr)
	}

	if c.TrustedProxies, err = trustedProxies("WEBAPI_TRUSTED_PROXIES"); err != nil {
		return nil, err
	}

	c.Live = env("WEBAPI_LIVE", "1") != "0"
	if c.LiveMaxSubscribers, err = positive("WEBAPI_LIVE_MAX_SUBSCRIBERS", "256"); err != nil {
		return nil, err
	}
	if c.LiveBuffer, err = positive("WEBAPI_LIVE_BUFFER", "256"); err != nil {
		return nil, err
	}
	if c.LiveHistory, err = positive("WEBAPI_LIVE_HISTORY", "4096"); err != nil {
		return nil, err
	}
	recheck, err := positive("WEBAPI_LIVE_RECHECK", "30")
	if err != nil {
		return nil, err
	}
	c.LiveRecheck = time.Duration(recheck) * time.Second
	if c.LivePublic, err = livePublic(); err != nil {
		return nil, err
	}
	if c.Recheck, err = recheckLimits(); err != nil {
		return nil, err
	}
	counts, err := integer("WEBAPI_COUNT_CACHE_SECS", "60")
	if err != nil || counts < 0 {
		return nil, fmt.Errorf("WEBAPI_COUNT_CACHE_SECS: %q is not a number of seconds",
			env("WEBAPI_COUNT_CACHE_SECS", "60"))
	}
	c.CountCache = time.Duration(counts) * time.Second
	// The render service and the bundle it was built with
	// (docs/design/web-ssr.md sections 4.5 and 6).
	c.RenderSocket = env("RENDER_SOCKET", "/run/tgloggerd-render/render.sock")
	if c.RenderTimeout, err = millis("RENDER_TIMEOUT", "500"); err != nil {
		return nil, err
	}
	if c.RenderBackoff, err = millis("RENDER_BACKOFF", "2000"); err != nil {
		return nil, err
	}
	c.AppDist = env("WEBAPI_APP_DIST", "web/app/dist/client")
	c.AppBuild = env("WEBAPI_APP_BUILD", "")

	c.Defaults = settings.Defaults{
		Timezone:   env("WEBAPI_DEFAULT_TIMEZONE", settings.SiteDefaults.Timezone),
		DateFormat: env("WEBAPI_DEFAULT_DATE_FORMAT", settings.SiteDefaults.DateFormat),
		Lang:       env("WEBAPI_DEFAULT_LANG", settings.SiteDefaults.Lang),
		Theme:      env("WEBAPI_DEFAULT_THEME", settings.SiteDefaults.Theme),
	}
	if err := c.Defaults.Check(); err != nil {
		return nil, fmt.Errorf("WEBAPI_DEFAULT_*: %w", err)
	}

	// The MCP endpoint's settings are the Drogon web's.
	c.PublicURL = strings.TrimRight(env("WEB_PUBLIC_URL", env("TG_DISCORD_PUBLIC_URL", "")), "/ ")
	if c.MCPThreads, err = integer("MCP_THREADS", "2"); err != nil {
		return nil, err
	}
	c.MCPThreads = min(max(c.MCPThreads, 1), 16)
	c.MCPAllowedOrigins = env("MCP_ALLOWED_ORIGINS", "")
	c.MCPLogRequests = env("MCP_LOG_REQUESTS", "0") == "1"

	if err := mailFromEnv(&c); err != nil {
		return nil, err
	}
	if err := searchEngineFromEnv(&c.SearchEngine); err != nil {
		return nil, err
	}

	return &c, nil
}

// mailFromEnv reads the SMTP relay and the links' settings
// (docs/design/web-accounts.md section 8); the links point to
// PublicURL, read before. A relay without the site's URL stops the
// service, since its links would lead nowhere.
func mailFromEnv(c *Config) error {
	for _, l := range []struct {
		key, def string
		to       *time.Duration
	}{
		{"WEBAPI_INVITE_LINK_TTL", "259200", &c.InviteLinkTTL},
		{"WEBAPI_RESET_LINK_TTL", "3600", &c.ResetLinkTTL},
		{"WEBAPI_VERIFY_LINK_TTL", "86400", &c.VerifyLinkTTL},
	} {
		n, err := positive(l.key, l.def)
		if err != nil {
			return err
		}
		*l.to = time.Duration(n) * time.Second
	}

	host := env("WEBAPI_SMTP_HOST", "")
	if host == "" {
		return nil
	}
	if !strings.HasPrefix(c.PublicURL, "https://") && !strings.HasPrefix(c.PublicURL, "http://") {
		return fmt.Errorf("WEBAPI_SMTP_HOST is set, but WEB_PUBLIC_URL (or TG_DISCORD_PUBLIC_URL) "+
			"is not an http(s) URL for the links in mail: %q", c.PublicURL)
	}
	sec, ok := mail.ParseSecurity(env("WEBAPI_SMTP_SECURITY", "starttls"))
	if !ok {
		return fmt.Errorf("WEBAPI_SMTP_SECURITY: %q is not starttls, tls or none",
			os.Getenv("WEBAPI_SMTP_SECURITY"))
	}
	defPort := "587"
	if sec == mail.TLS {
		defPort = "465"
	}
	p, err := port("WEBAPI_SMTP_PORT", defPort)
	if err != nil {
		return err
	}
	from := env("WEBAPI_SMTP_FROM", "")
	if from == "" {
		return fmt.Errorf("WEBAPI_SMTP_HOST is set, but WEBAPI_SMTP_FROM (the sender) is not")
	}
	c.Mail = &mail.Config{
		Host:     host,
		Port:     int(p),
		User:     env("WEBAPI_SMTP_USER", ""),
		Password: os.Getenv("WEBAPI_SMTP_PASSWORD"),
		From:     from,
		Security: sec,
	}
	return nil
}

// searchEngineFromEnv reads the search engine's settings; without
// WEBAPI_SEARCH_ENGINE_URL the others are not read.
func searchEngineFromEnv(se *SearchEngine) error {
	se.URL = env("WEBAPI_SEARCH_ENGINE_URL", "")
	if se.URL == "" {
		return nil
	}
	se.Index = env("WEBAPI_SEARCH_ENGINE_INDEX", "tgloggerd-messages")
	se.UsersIndex = env("WEBAPI_SEARCH_ENGINE_USERS_INDEX", "tgloggerd-users")
	se.GroupsIndex = env("WEBAPI_SEARCH_ENGINE_GROUPS_INDEX", "tgloggerd-groups")
	if se.Index == se.UsersIndex || se.Index == se.GroupsIndex || se.UsersIndex == se.GroupsIndex {
		return fmt.Errorf("WEBAPI_SEARCH_ENGINE_INDEX, _USERS_INDEX and _GROUPS_INDEX " +
			"must name three different indices")
	}
	se.Indexer = env("WEBAPI_SEARCH_ENGINE_INDEXER", "1") != "0"
	var err error
	if se.Batch, err = positive("WEBAPI_SEARCH_ENGINE_BATCH", "1000"); err != nil {
		return err
	}
	if se.Pause, err = millis("WEBAPI_SEARCH_ENGINE_PAUSE", "200"); err != nil {
		return err
	}
	n, err := positive("WEBAPI_SEARCH_ENGINE_RECONCILE", "600")
	if err != nil {
		return err
	}
	se.Reconcile = time.Duration(n) * time.Second
	if se.Timeout, err = millis("WEBAPI_SEARCH_ENGINE_TIMEOUT", "1500"); err != nil {
		return err
	}
	if n, err = positive("WEBAPI_SEARCH_ENGINE_MAX_LAG", "300"); err != nil {
		return err
	}
	se.MaxLag = time.Duration(n) * time.Second
	return nil
}
