// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"slices"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"tgloggerd/webapi/internal/logdb"
	"tgloggerd/webapi/internal/mcp"
	"tgloggerd/webapi/internal/scope"
	"tgloggerd/webapi/internal/webdb"
)

// The MCP administration (web/docs/mcp.md), the Drogon
// McpAdminController as JSON operations: which groups the MCP endpoint
// exposes (telegram_public_groups, which also makes their chats
// public), and its bearer tokens (web_mcp_tokens). The groups need the
// mcp ability, the tokens mcp_tokens (see below); a group the scope
// hides can be neither exposed nor withdrawn, as if it did not exist
// (web-accounts.md section 4).

// Column lengths, in characters.
const (
	mcpNoteMax      = 255
	mcpTokenNameMax = 64
)

// mintMCPToken is a new token and the SHA-256 it is stored as, made
// by the MCP endpoint's own rules (mcp.NewToken), so every token the
// pages mint is one the endpoint accepts.
func mintMCPToken() (string, []byte, error) {
	tok, sum, err := mcp.NewToken()
	return tok, sum[:], err
}

// exposeMcpGroup answers POST /api/mcp/groups.
func (s *Server) exposeMcpGroup(w http.ResponseWriter, r *http.Request) {
	sc, v, ok := s.adminWrite(w, r, scope.ManageMCP)
	if !ok {
		return
	}
	var in struct {
		GroupID *int64  `json:"group_id"`
		Note    *string `json:"note"`
	}
	if !decodeBody(w, r, &in) {
		return
	}
	if in.GroupID == nil || *in.GroupID >= 0 {
		s.refuse(w, r, v, http.StatusBadRequest, "bad_request", "mcp.group_required")
		return
	}
	ctx := r.Context()
	visible, err := logdb.GroupVisible(ctx, s.ro, sc, *in.GroupID)
	if err != nil {
		s.internalError(w, "group", err)
		return
	}
	if !visible {
		notFound(w)
		return
	}
	note := ""
	if in.Note != nil {
		note = clip(*in.Note, mcpNoteMax)
	}
	added, err := logdb.ExposeGroup(ctx, s.ro, *in.GroupID, note, v.ID)
	if err != nil {
		s.internalError(w, "expose group", err)
		return
	}
	if !added {
		// Its note changes by PATCH, which keeps who exposed it and
		// when.
		s.refuse(w, r, v, http.StatusConflict, "already_exposed", "mcp.already_exposed")
		return
	}
	s.auditAdmin(r, v.ID, "mcp_expose", "group "+strconv.FormatInt(*in.GroupID, 10)+" "+note)
	s.recordOwner(r, webdb.Resource{Type: webdb.ResMCPGroup, ID: *in.GroupID}, v.ID)
	writeJSON(w, http.StatusCreated, map[string]int64{"group_id": *in.GroupID})
}

// unexposeMcpGroup answers DELETE /api/mcp/groups/{group_id}. Taking a
// group off can only reduce what the endpoint shows, so it asks for
// nothing more than the page's confirmation.
func (s *Server) unexposeMcpGroup(w http.ResponseWriter, r *http.Request) {
	sc, v, ok := s.adminWrite(w, r, scope.ManageMCP)
	if !ok {
		return
	}
	id, err := strconv.ParseInt(r.PathValue("group_id"), 10, 64)
	if err != nil || id >= 0 {
		notFound(w)
		return
	}
	ctx := r.Context()
	visible, err := logdb.GroupVisible(ctx, s.ro, sc, id)
	if err != nil {
		s.internalError(w, "group", err)
		return
	}
	if !visible {
		notFound(w)
		return
	}
	removed, err := logdb.UnexposeGroup(ctx, s.ro, id)
	if err != nil {
		s.internalError(w, "unexpose group", err)
		return
	}
	if !removed {
		notFound(w)
		return
	}
	s.auditAdmin(r, v.ID, "mcp_unexpose", "group "+strconv.FormatInt(id, 10))
	s.dropOwner(r, webdb.Resource{Type: webdb.ResMCPGroup, ID: id}, v.ID)
	w.WriteHeader(http.StatusNoContent)
}

// setMcpGroupNote answers PATCH /api/mcp/groups/{group_id}: the note
// of an exposed group is reworded in place, under the same permission
// as exposing it. Who exposed the group and when stay as they are; the
// audit row keeps the old and the new note, so the earlier reason is
// not lost. An unchanged note answers 204 and audits nothing.
func (s *Server) setMcpGroupNote(w http.ResponseWriter, r *http.Request) {
	sc, v, ok := s.adminWrite(w, r, scope.ManageMCP)
	if !ok {
		return
	}
	id, err := strconv.ParseInt(r.PathValue("group_id"), 10, 64)
	if err != nil || id >= 0 {
		notFound(w)
		return
	}
	var in struct {
		Note *string `json:"note"`
	}
	if !decodeBody(w, r, &in) {
		return
	}
	if in.Note == nil {
		s.refuse(w, r, v, http.StatusBadRequest, "bad_request", "mcp.note_required")
		return
	}
	ctx := r.Context()
	visible, err := logdb.GroupVisible(ctx, s.ro, sc, id)
	if err != nil {
		s.internalError(w, "group", err)
		return
	}
	if !visible {
		notFound(w)
		return
	}
	note := clip(*in.Note, mcpNoteMax)
	old, found, err := logdb.SetExposedNote(ctx, s.ro, id, note)
	if err != nil {
		s.internalError(w, "group note", err)
		return
	}
	if !found {
		notFound(w)
		return
	}
	if old != note {
		s.auditAdmin(r, v.ID, "mcp_note", noteChange("group "+strconv.FormatInt(id, 10)+" ", old, note))
	}
	w.WriteHeader(http.StatusNoContent)
}

// noteChange is the audit detail of a reworded note, prefix then
// "old -> new". The detail column holds webdb.MaxAuditDetail
// characters; when both notes do not fit, each is cut to its share
// (a short one leaves the rest to the other), so neither is lost.
func noteChange(prefix, old, note string) string {
	const sep = " -> "
	room := webdb.MaxAuditDetail - utf8.RuneCountInString(prefix) - len(sep)
	no, nn := utf8.RuneCountInString(old), utf8.RuneCountInString(note)
	if no+nn > room {
		half := room / 2
		switch {
		case no <= half:
			nn = room - no
		case nn <= half:
			no = room - nn
		default:
			no, nn = half, room-half
		}
	}
	return prefix + clip(old, no) + sep + clip(note, nn)
}

// The tokens (decisions/2026-11-20-mcp-tokens-to-superadmin.md) need
// mcp_tokens, which superadmins and admins hold by role and a member
// when given it. Each is managed by whoever Scope.ManagesMCPToken says,
// by its owner record; a token the viewer does not manage is 404 like
// an unknown one. Only superadmins transfer one.

// mayHoldMCPToken is whether account a may own a token: an active
// superadmin or admin, or an active member holding mcp_tokens.
func mayHoldMCPToken(a webdb.Account) bool {
	return a.Active && (a.Role != string(scope.Member) ||
		slices.Contains(a.Abilities, webdb.AbilityMCPTokens))
}

// managesTokenOf is whether the viewer manages a token owned by
// account owner (0 for none), with accts holding the accounts by id.
func managesTokenOf(sc scope.Scope, owner uint64, accts map[uint64]webdb.Account) bool {
	if owner == 0 {
		return sc.ManagesMCPToken(nil)
	}
	a, ok := accts[owner]
	if !ok {
		return sc.ManagesMCPToken(nil)
	}
	sa := scopeAccount(a)
	return sc.ManagesMCPToken(&sa)
}

// accountsByID is every account, by id.
func (s *Server) accountsByID(ctx context.Context) (map[uint64]webdb.Account, error) {
	all, err := webdb.ListAccounts(ctx, s.app)
	if err != nil {
		return nil, err
	}
	out := make(map[uint64]webdb.Account, len(all))
	for _, a := range all {
		out[a.ID] = a
	}
	return out, nil
}

// tokenOwners is who the viewer may mint a token for and, when it
// transfers tokens, give one to: the accounts that may hold a token
// and whose tokens it manages, by username.
func tokenOwners(sc scope.Scope, accts map[uint64]webdb.Account) []webdb.Account {
	var out []webdb.Account
	for _, a := range accts {
		sa := scopeAccount(a)
		if mayHoldMCPToken(a) && sc.ManagesMCPToken(&sa) {
			out = append(out, a)
		}
	}
	slices.SortFunc(out, func(a, b webdb.Account) int { return strings.Compare(a.Username, b.Username) })
	return out
}

// tokenOwner checks owner as the new owner of a token: one of
// tokenOwners. It answers 400 itself otherwise.
func (s *Server) tokenOwner(w http.ResponseWriter, r *http.Request, sc scope.Scope, v scope.Viewer,
	owner uint64) bool {
	a, err := webdb.GetAccount(r.Context(), s.app, owner)
	if errors.Is(err, webdb.ErrNoUser) {
		s.refuse(w, r, v, http.StatusBadRequest, "bad_request", "mcp.bad_owner")
		return false
	}
	if err != nil {
		s.internalError(w, "token owner", err)
		return false
	}
	sa := scopeAccount(a)
	if !mayHoldMCPToken(a) || !sc.ManagesMCPToken(&sa) {
		s.refuse(w, r, v, http.StatusBadRequest, "bad_request", "mcp.bad_owner")
		return false
	}
	return true
}

// managedToken loads the token in the path, answering 404 itself for
// one that does not exist or that the viewer does not manage.
func (s *Server) managedToken(w http.ResponseWriter, r *http.Request, sc scope.Scope) (webdb.MCPToken, bool) {
	id, ok := pathID(r, "id")
	if !ok {
		notFound(w)
		return webdb.MCPToken{}, false
	}
	ctx := r.Context()
	t, err := webdb.GetMCPToken(ctx, s.app, id)
	if errors.Is(err, webdb.ErrNoToken) {
		notFound(w)
		return t, false
	}
	if err != nil {
		s.internalError(w, "token", err)
		return t, false
	}
	accts := map[uint64]webdb.Account{}
	if t.OwnerID != 0 {
		a, err := webdb.GetAccount(ctx, s.app, t.OwnerID)
		if err != nil {
			s.internalError(w, "token owner", err)
			return t, false
		}
		accts[a.ID] = a
	}
	if !managesTokenOf(sc, t.OwnerID, accts) {
		notFound(w)
		return t, false
	}
	return t, true
}

// tokenName reads a token's name from a request body: trimmed, cut to
// mcpTokenNameMax, not empty. It answers 400 itself otherwise.
func (s *Server) tokenName(w http.ResponseWriter, r *http.Request, v scope.Viewer, name *string) (string, bool) {
	n := ""
	if name != nil {
		n = clip(*name, mcpTokenNameMax)
	}
	if n == "" {
		s.refuse(w, r, v, http.StatusBadRequest, "bad_request", "mcp.name_required")
		return "", false
	}
	return n, true
}

// createMcpToken answers POST /api/mcp/tokens. The token is in this
// answer only: it is stored as its SHA-256, and neither the log nor the
// audit row sees it. The viewer mints it, for itself or for owner_id.
func (s *Server) createMcpToken(w http.ResponseWriter, r *http.Request) {
	sc, v, ok := s.adminWrite(w, r, scope.MCPTokens)
	if !ok {
		return
	}
	var in struct {
		Name    *string      `json:"name"`
		OwnerID *uint64      `json:"owner_id"`
		Scope   []mcpScopeIn `json:"scope"`
	}
	if !decodeBody(w, r, &in) {
		return
	}
	name, ok := s.tokenName(w, r, v, in.Name)
	if !ok {
		return
	}
	owner := v.ID
	if in.OwnerID != nil {
		owner = *in.OwnerID
	}
	if !s.tokenOwner(w, r, sc, v, owner) {
		return
	}
	tscope, ok := s.tokenScope(w, r, sc, v, owner, in.Scope)
	if !ok {
		return
	}
	tok, sum, err := mintMCPToken()
	if err != nil {
		s.internalError(w, "mint token", err)
		return
	}
	id, err := webdb.CreateMCPToken(r.Context(), s.app, v.ID, owner, name, sum, tscope, webdb.By(v.ID, clientIP(r)))
	if err != nil {
		s.internalError(w, "store token", err)
		return
	}
	detail := "token " + strconv.FormatUint(id, 10) + " " + name
	if owner != v.ID {
		detail = "token " + strconv.FormatUint(id, 10) + " for " + strconv.FormatUint(owner, 10) + " " + name
	}
	s.auditAdmin(r, v.ID, "mcp_token_create", detail)
	if len(tscope) > 0 {
		s.auditAdmin(r, v.ID, "mcp_token_scope", scopeDetail(id, tscope))
	}
	writeJSON(w, http.StatusCreated, map[string]any{"id": id, "token": tok})
}

// scopeDetail is the audit detail of a token's scope as written.
func scopeDetail(id uint64, es []webdb.MCPScopeEntry) string {
	return clip("token "+strconv.FormatUint(id, 10)+": "+webdb.MCPScopeText(es), webdb.MaxAuditDetail)
}

// renameMcpToken answers PATCH /api/mcp/tokens/{id}: a new name, a new
// scope, or both, each checked before either is written. An unchanged
// name or scope answers 204 and audits nothing.
func (s *Server) renameMcpToken(w http.ResponseWriter, r *http.Request) {
	sc, v, ok := s.adminWrite(w, r, scope.MCPTokens)
	if !ok {
		return
	}
	t, ok := s.managedToken(w, r, sc)
	if !ok {
		return
	}
	var in struct {
		Name  *string       `json:"name"`
		Scope *[]mcpScopeIn `json:"scope"`
	}
	if !decodeBody(w, r, &in) {
		return
	}
	var tscope []webdb.MCPScopeEntry
	if in.Scope != nil {
		if tscope, ok = s.tokenScope(w, r, sc, v, t.OwnerID, *in.Scope); !ok {
			return
		}
	}
	var name string
	if in.Name != nil || in.Scope == nil {
		if name, ok = s.tokenName(w, r, v, in.Name); !ok {
			return
		}
	}
	if in.Scope != nil {
		changed, err := webdb.SetMCPTokenScope(r.Context(), s.app, t.ID, tscope)
		if errors.Is(err, webdb.ErrNoToken) {
			notFound(w)
			return
		}
		if err != nil {
			s.internalError(w, "token scope", err)
			return
		}
		if changed {
			s.auditAdmin(r, v.ID, "mcp_token_scope", scopeDetail(t.ID, tscope))
		}
		if in.Name == nil {
			w.WriteHeader(http.StatusNoContent)
			return
		}
	}
	old, err := webdb.RenameMCPToken(r.Context(), s.app, t.ID, name)
	if errors.Is(err, webdb.ErrNoToken) {
		notFound(w)
		return
	}
	if err != nil {
		s.internalError(w, "rename token", err)
		return
	}
	if old != name {
		s.auditAdmin(r, v.ID, "mcp_token_rename", noteChange("token "+strconv.FormatUint(t.ID, 10)+" ", old, name))
	}
	w.WriteHeader(http.StatusNoContent)
}

// revokeMcpToken answers POST /api/mcp/tokens/{id}/revoke: revoked_at
// is stamped, the row stays. Revoking a revoked token changes nothing
// and answers 204 as well.
func (s *Server) revokeMcpToken(w http.ResponseWriter, r *http.Request) {
	sc, v, ok := s.adminWrite(w, r, scope.MCPTokens)
	if !ok {
		return
	}
	t, ok := s.managedToken(w, r, sc)
	if !ok {
		return
	}
	revoked, err := webdb.RevokeMCPToken(r.Context(), s.app, t.ID)
	if err != nil {
		s.internalError(w, "revoke token", err)
		return
	}
	if revoked {
		s.auditAdmin(r, v.ID, "mcp_token_revoke", "token "+strconv.FormatUint(t.ID, 10)+" "+t.Name)
	}
	w.WriteHeader(http.StatusNoContent)
}

// transferMcpToken answers POST /api/mcp/tokens/{id}/transfer:
// superadmins give a token to another account that may hold one. To
// anyone else it does not exist. The token keeps its minter.
func (s *Server) transferMcpToken(w http.ResponseWriter, r *http.Request) {
	sc, v, ok := s.adminWrite(w, r, scope.MCPTokens)
	if !ok {
		return
	}
	if !sc.TransfersMCPTokens() {
		notFound(w)
		return
	}
	t, ok := s.managedToken(w, r, sc)
	if !ok {
		return
	}
	var in struct {
		OwnerID *uint64 `json:"owner_id"`
	}
	if !decodeBody(w, r, &in) {
		return
	}
	if in.OwnerID == nil {
		s.refuse(w, r, v, http.StatusBadRequest, "bad_request", "mcp.bad_owner")
		return
	}
	if !s.tokenOwner(w, r, sc, v, *in.OwnerID) {
		return
	}
	ctx := r.Context()
	res := webdb.Resource{Type: webdb.ResMCPToken, ID: int64(t.ID)}
	by := webdb.By(v.ID, clientIP(r))
	from, err := webdb.TransferOwner(ctx, s.app, res, *in.OwnerID, by)
	if errors.Is(err, webdb.ErrNotOwned) {
		// It counted as the superadmins': it gets its first owner.
		from, err = 0, webdb.SetOwner(ctx, s.app, res, *in.OwnerID, by)
	}
	if err != nil {
		s.internalError(w, "transfer token", err)
		return
	}
	if from != *in.OwnerID {
		s.auditAdmin(r, v.ID, "mcp_token_transfer", fmt.Sprintf("token %d from %d to %d", t.ID, from, *in.OwnerID))
	}
	w.WriteHeader(http.StatusNoContent)
}

// mcpIndexProps are the MCP page's (McpIndexProps).
type mcpIndexProps struct {
	ExposedGroups   int     `json:"exposed_groups"`
	ExposedMessages int64   `json:"exposed_messages"`
	ActiveTokens    int     `json:"active_tokens"`
	LastUsed        *string `json:"last_used"`
	Endpoint        string  `json:"endpoint"`
}

// mcpEndpoint is the MCP endpoint's URL on the site the page was asked
// on: nginx passes the Host on, and the scheme in X-Forwarded-Proto.
func mcpEndpoint(r *http.Request) string {
	scheme := "http"
	if r.TLS != nil {
		scheme = "https"
	}
	if p := r.Header.Get("X-Forwarded-Proto"); p == "https" || p == "http" {
		scheme = p
	}
	return scheme + "://" + r.Host + "/mcp"
}

func timePtr(t *time.Time) *string {
	if t == nil {
		return nil
	}
	s := t.UTC().Format(time.RFC3339)
	return &s
}

func loadMcp(ctx context.Context, pr *pageRequest) (any, error) {
	if _, err := pageOf(pr, "mcp"); err != nil {
		return nil, err
	}
	s := pr.s
	var p mcpIndexProps
	var err error
	if p.ExposedGroups, p.ExposedMessages, err = logdb.ExposedStats(ctx, s.ro, pr.scope); err != nil {
		return nil, err
	}
	ts, _, err := s.managedTokens(ctx, pr.scope)
	if err != nil {
		return nil, err
	}
	var last *time.Time
	for _, t := range ts {
		if t.Revoked != nil {
			continue
		}
		p.ActiveTokens++
		if t.LastUsed != nil && (last == nil || t.LastUsed.After(*last)) {
			last = t.LastUsed
		}
	}
	p.LastUsed, p.Endpoint = timePtr(last), mcpEndpoint(pr.r)
	return p, nil
}

type mcpGroupJSON struct {
	Group     groupSummaryJSON `json:"group"`
	PublicNow bool             `json:"public_now"`
	Note      string           `json:"note"`
	Added     string           `json:"added"`
}

type mcpGroupsProps struct {
	Groups []mcpGroupJSON `json:"groups"`
}

func loadMcpGroups(ctx context.Context, pr *pageRequest) (any, error) {
	if _, err := pageOf(pr, "mcp"); err != nil {
		return nil, err
	}
	// The menu's mcp entry is a member's too, for its tokens.
	if !pr.scope.Can(scope.ManageMCP) {
		return nil, errPageNotFound
	}
	s := pr.s
	gs, err := logdb.ExposedGroups(ctx, s.ro, pr.scope)
	if err != nil {
		return nil, err
	}
	v := s.viewFor(pr.scope, true)
	p := mcpGroupsProps{Groups: []mcpGroupJSON{}}
	for _, g := range gs {
		j := v.group(g.Group)
		// GroupCounted: the count is always there on this page, which
		// only viewers that see groups in full reach.
		n := g.Group.MsgCount
		j.MsgCount = &n
		p.Groups = append(p.Groups, mcpGroupJSON{Group: j, PublicNow: g.PublicNow,
			Note: g.Note, Added: iso(g.Added)})
	}
	return p, nil
}

type mcpTokenJSON struct {
	ID       uint64    `json:"id"`
	Name     string    `json:"name"`
	Owner    fwdPerson `json:"owner"`
	MintedBy fwdPerson `json:"minted_by"`
	Created  string    `json:"created"`
	LastUsed *string   `json:"last_used"`
	Revoked  *string   `json:"revoked"`
	// Scope is what the token reads, empty for every exposed group
	// (decisions/2026-11-20-mcp-token-scope.md).
	Scope []mcpScopeJSON `json:"scope"`
}

type mcpTokensProps struct {
	Tokens       []mcpTokenJSON  `json:"tokens"`
	Owners       []fwdPerson     `json:"owners"`
	CanTransfer  bool            `json:"can_transfer"`
	ScopeOptions mcpScopeOptions `json:"scope_options"`
}

// managedTokens is the tokens the viewer manages, newest first, and
// every account by id.
func (s *Server) managedTokens(ctx context.Context, sc scope.Scope) ([]webdb.MCPToken,
	map[uint64]webdb.Account, error) {
	ts, err := webdb.ListMCPTokens(ctx, s.app)
	if err != nil {
		return nil, nil, err
	}
	accts, err := s.accountsByID(ctx)
	if err != nil {
		return nil, nil, err
	}
	out := []webdb.MCPToken{}
	for _, t := range ts {
		if managesTokenOf(sc, t.OwnerID, accts) {
			out = append(out, t)
		}
	}
	return out, accts, nil
}

func loadMcpTokens(ctx context.Context, pr *pageRequest) (any, error) {
	v, err := pageOf(pr, "mcp")
	if err != nil {
		return nil, err
	}
	if !pr.scope.Can(scope.MCPTokens) {
		return nil, errPageNotFound
	}
	s := pr.s
	ts, accts, err := s.managedTokens(ctx, pr.scope)
	if err != nil {
		return nil, err
	}
	shared := fwdPerson{ID: 0, Name: s.textFor(ctx, v, "fwd.shared"), Active: true}
	person := func(id uint64) fwdPerson {
		if a, ok := accts[id]; ok {
			return personOf(a)
		}
		return shared
	}
	p := mcpTokensProps{Tokens: []mcpTokenJSON{}, Owners: []fwdPerson{},
		CanTransfer: pr.scope.TransfersMCPTokens()}
	if p.ScopeOptions, err = s.scopeOptions(ctx, pr.scope, v); err != nil {
		return nil, err
	}
	scopes, err := webdb.MCPTokenScopes(ctx, s.app)
	if err != nil {
		return nil, err
	}
	sv, err := s.newScopeView(ctx, pr.scope, scopes)
	if err != nil {
		return nil, err
	}
	for _, t := range ts {
		// The superadmins' when it has no owner record.
		privateOK := t.OwnerID == 0
		if a, ok := accts[t.OwnerID]; ok {
			privateOK = a.Role == string(scope.Superadmin)
		}
		p.Tokens = append(p.Tokens, mcpTokenJSON{ID: t.ID, Name: t.Name, Owner: person(t.OwnerID),
			MintedBy: person(t.MinterID), Created: t.Created.Format(time.RFC3339),
			LastUsed: timePtr(t.LastUsed), Revoked: timePtr(t.Revoked),
			Scope: sv.entries(scopes[t.ID], privateOK)})
	}
	for _, a := range tokenOwners(pr.scope, accts) {
		p.Owners = append(p.Owners, personOf(a))
	}
	return p, nil
}
