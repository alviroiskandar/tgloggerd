// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"errors"
	"io"
	"net/http"
	"strings"

	"tgloggerd/webapi/internal/mcp"
)

// The MCP endpoint, /mcp: the Streamable HTTP transport of
// internal/mcp, as the Drogon web's McpController serves it
// (web/docs/mcp.md).
//
// The transport is the minimal legal form: a server that never
// initiates messages answers every POST with application/json and
// declines GET, so there is no SSE stream, no session store and no
// streaming. The route carries no session: an MCP client has no cookie,
// and a redirect to /login is useless to it. It authenticates with a
// bearer token instead and answers a plain 401 without one.
//
// The 401 carries NO WWW-Authenticate header, on purpose. To an MCP
// client that header means "this is an OAuth protected resource", and
// it then goes looking for discovery endpoints and dynamic client
// registration this server does not have, and fails without ever using
// the token it holds. Adding OAuth means adding the discovery endpoints
// and the header together, never the header alone.

// MCPConfig is the endpoint's settings.
type MCPConfig struct {
	// PublicURL is the base of the absolute /files/<token> links in
	// tool answers (WEB_PUBLIC_URL, else TG_DISCORD_PUBLIC_URL), with
	// no trailing slash; "" leaves the links out.
	PublicURL string
	// Threads is how many tool calls run at once (MCP_THREADS); more
	// wait their turn, so a slow query delays only another MCP call.
	Threads int
	// AllowedOrigins is MCP_ALLOWED_ORIGINS: a comma-separated list of
	// browser origins, or "*"; empty allows any (see mcpOriginAllowed).
	AllowedOrigins string
	// LogRequests is MCP_LOG_REQUESTS: log the shape of each request
	// (never a credential's value) and its status.
	LogRequests bool
}

// mcpMaxBody bounds a request body, as Drogon's default
// client_max_body_size does.
const mcpMaxBody = 1 << 20

const mcpJSON = "application/json; charset=utf-8"

func writeMCP(w http.ResponseWriter, status int, body []byte) {
	w.Header().Set("Content-Type", mcpJSON)
	w.WriteHeader(status)
	_, _ = w.Write(body)
}

func writeMCPError(w http.ResponseWriter, status, code int, msg string) {
	writeMCP(w, status, mcp.RPCErrorBody(code, msg))
}

// mcpOriginAllowed is the Origin policy. The spec asks for an Origin
// check against DNS rebinding, written for an unauthenticated server
// on localhost. This one requires a bearer token, which a hostile page
// does not have, and sends no CORS headers, so such a page can neither
// read an answer nor, with a JSON body that needs a preflight, usually
// send a request. So the default allows: refusing every Origin turned
// away browser-based and Electron clients with a 403 before their
// token was even looked at. MCP_ALLOWED_ORIGINS restores the check.
func (s *Server) mcpOriginAllowed(r *http.Request) bool {
	origin := r.Header.Get("Origin")
	if origin == "" || s.mcpCfg.AllowedOrigins == "" {
		return true
	}
	for _, item := range strings.Split(s.mcpCfg.AllowedOrigins, ",") {
		item = strings.TrimRight(strings.TrimLeft(item, " "), " \r")
		if item == "*" || item == origin {
			return true
		}
	}
	return false
}

// mcpVersionError checks MCP-Protocol-Version for shape only: a
// YYYY-MM-DD date. A well-formed version this build does not know goes
// on to initialize, which negotiates; refusing it here would fail
// closed against every newer client. No header is a client from before
// the header (2025-03-26).
func mcpVersionError(r *http.Request) string {
	v := r.Header.Get("MCP-Protocol-Version")
	if v == "" {
		return ""
	}
	if len(v) == 10 && v[4] == '-' && v[7] == '-' {
		ok := true
		for i := 0; i < len(v); i++ {
			if i != 4 && i != 7 && (v[i] < '0' || v[i] > '9') {
				ok = false
				break
			}
		}
		if ok {
			return ""
		}
	}
	return "Malformed MCP-Protocol-Version: " + v
}

// mcpCredential is the presented token: the Authorization header
// first, then ?key=, for clients that accept only a URL. The query
// string is weaker (it lands in proxy logs and browser history), so
// web/docs/mcp.md advises a separate token for it.
func mcpCredential(r *http.Request) string {
	if h := mcp.FromAuthorizationHeader(r.Header.Get("Authorization")); h != "" {
		return h
	}
	return r.URL.Query().Get(mcp.TokenQueryParam)
}

// traceMCP logs what a client sent, when MCP_LOG_REQUESTS is on:
// whether a credential came, never its value.
func (s *Server) traceMCP(r *http.Request) {
	if !s.mcpCfg.LogRequests {
		return
	}
	yes := func(b bool) string {
		if b {
			return "yes"
		}
		return "no"
	}
	s.log.Info("mcp-trace",
		"method", r.Method,
		"path", r.URL.Path,
		"auth_header", yes(r.Header.Get("Authorization") != ""),
		"key_param", yes(r.URL.Query().Get(mcp.TokenQueryParam) != ""),
		"origin", r.Header.Get("Origin"),
		"mcpver", r.Header.Get("MCP-Protocol-Version"),
		"ua", r.UserAgent())
}

// postMCP serves one JSON-RPC message.
func (s *Server) postMCP(w http.ResponseWriter, r *http.Request) {
	s.traceMCP(r)
	if !s.mcpOriginAllowed(r) {
		writeMCPError(w, http.StatusForbidden, mcp.InvalidRequest, "Origin not allowed")
		return
	}
	if msg := mcpVersionError(r); msg != "" {
		writeMCPError(w, http.StatusBadRequest, mcp.InvalidRequest, msg)
		return
	}

	bearer := mcpCredential(r)
	if bearer == "" || !mcp.LooksLikeToken(bearer) {
		writeMCPError(w, http.StatusUnauthorized, mcp.InvalidRequest,
			"Missing or malformed token. Send \"Authorization: Bearer <token>\", "+
				"or append ?key=<token> if your client cannot set headers.")
		return
	}
	owner, err := mcp.ResolveToken(r.Context(), s.app, mcp.TokenDigest(bearer))
	if errors.Is(err, mcp.ErrNoToken) {
		writeMCPError(w, http.StatusUnauthorized, mcp.InvalidRequest, "Invalid or revoked token")
		return
	}
	if err != nil {
		s.log.Error("mcp token lookup", "err", err)
		writeMCPError(w, http.StatusInternalServerError, mcp.InternalError,
			"auth backend error: "+err.Error())
		return
	}
	// Best effort: a missed stamp must not fail the request.
	if err := mcp.TouchToken(r.Context(), s.app, owner.TokenID); err != nil {
		s.log.Warn("mcp token last use", "token", owner.TokenID, "err", err)
	}
	// The token's scope, read on every request, so a change to it, to
	// a federation in it, to the token's owner or to a member owner's
	// grants counts at once. A scope that cannot be read fails the
	// request rather than read unnarrowed.
	reach, err := mcp.ResolveScope(r.Context(), s.app, owner)
	if err != nil {
		s.log.Error("mcp token scope", "token", owner.TokenID, "err", err)
		writeMCPError(w, http.StatusInternalServerError, mcp.InternalError,
			"auth backend error: "+err.Error())
		return
	}

	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, mcpMaxBody))
	if err != nil {
		var tooBig *http.MaxBytesError
		if errors.As(err, &tooBig) {
			writeMCPError(w, http.StatusRequestEntityTooLarge, mcp.InvalidRequest,
				"Request body too large")
			return
		}
		writeMCPError(w, http.StatusBadRequest, mcp.ParseError, "Parse error: "+err.Error())
		return
	}
	if len(body) == 0 {
		// An empty POST is a reachability probe: 200 with a JSON-RPC
		// error, as for any unusable payload. A 4xx here sent a
		// client into OAuth discovery while it held a valid token.
		writeMCPError(w, http.StatusOK, mcp.ParseError, "Empty request body")
		return
	}

	// At most MCP_THREADS calls at once; the rest wait their turn.
	select {
	case s.mcpSlots <- struct{}{}:
	case <-r.Context().Done():
		return
	}
	reply, ok := s.mcp.HandleRaw(mcp.WithReach(r.Context(), reach), body)
	<-s.mcpSlots

	s.log.Debug("mcp call", "token", owner.TokenID, "account", owner.Username)
	if !ok {
		// A notification, or a response from the client: 202, no body.
		w.WriteHeader(http.StatusAccepted)
		return
	}
	writeMCP(w, http.StatusOK, reply)
}

// getMCP declines the server-to-client SSE stream, which a server
// that never initiates messages may do with a 405.
func (s *Server) getMCP(w http.ResponseWriter, r *http.Request) {
	s.traceMCP(r)
	mcpNotAllowed(w, "This MCP endpoint does not offer an SSE stream.\n")
}

// deleteMCP: the endpoint keeps no sessions, so none can be ended.
func (s *Server) deleteMCP(w http.ResponseWriter, r *http.Request) {
	s.traceMCP(r)
	mcpNotAllowed(w, "This MCP endpoint is stateless; there is no session to terminate.\n")
}

func mcpNotAllowed(w http.ResponseWriter, text string) {
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.Header().Set("Allow", "POST")
	w.WriteHeader(http.StatusMethodNotAllowed)
	_, _ = io.WriteString(w, text)
}

// mcpFileURL is the absolute /files/<token> link of a stored file, the
// one the pages use; "" without a public base URL, since a relative
// link is useless to a client that is not a browser on this site. The
// link is a bearer credential for that one file, independent of the
// MCP token that produced it.
func (s *Server) mcpFileURL(id uint64) string {
	if s.mcpCfg.PublicURL == "" || s.tokens == nil || id == 0 {
		return ""
	}
	return s.mcpCfg.PublicURL + "/files/" + s.tokens.Encrypt(id)
}
