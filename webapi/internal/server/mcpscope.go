// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"cmp"
	"context"
	"errors"
	"net/http"
	"slices"
	"strconv"

	"tgloggerd/webapi/internal/logdb"
	"tgloggerd/webapi/internal/scope"
	"tgloggerd/webapi/internal/webdb"
)

// The scope of an MCP token (decisions/2026-11-20-mcp-token-scope.md),
// as the token administration reads and writes it. The /mcp endpoint
// applies it (internal/mcp, scope.go and gate.go); this checks what
// may be written and says what each entry still reaches.

// mcpScopeIn is one entry of a written scope (McpScopeWrite).
type mcpScopeIn struct {
	Kind string `json:"kind"`
	ID   *int64 `json:"id"`
}

// mcpScopeJSON is one entry of a token's scope (McpScopeEntry).
type mcpScopeJSON struct {
	Kind   string `json:"kind"`
	ID     int64  `json:"id"`
	Name   string `json:"name"`
	Active bool   `json:"active"`
}

// mcpScopeOptions is what the token form may offer.
type mcpScopeOptions struct {
	Federations []fedRef      `json:"federations"`
	Groups      []mcpGroupRef `json:"groups"`
	Private     bool          `json:"private"`
}

type mcpGroupRef struct {
	ID   int64  `json:"id"`
	Name string `json:"name"`
}

// privateOwner is whether a token owned by account owner (0 for none:
// the superadmins') may hold private chat entries.
func (s *Server) privateOwner(ctx context.Context, owner uint64) (bool, error) {
	if owner == 0 {
		return true, nil
	}
	a, err := webdb.GetAccount(ctx, s.app, owner)
	if err != nil {
		return false, err
	}
	return a.Role == string(scope.Superadmin), nil
}

// tokenScope checks a written scope for a token owned by owner (0 for
// none) and returns it sorted, or answers 400 (a bad entry) or 403 (a
// private chat the viewer may not give this token) itself.
func (s *Server) tokenScope(w http.ResponseWriter, r *http.Request, sc scope.Scope, v scope.Viewer,
	owner uint64, in []mcpScopeIn) ([]webdb.MCPScopeEntry, bool) {
	ctx := r.Context()
	bad := func(key string, kv ...string) ([]webdb.MCPScopeEntry, bool) {
		s.refuse(w, r, v, http.StatusBadRequest, "bad_request", key, kv...)
		return nil, false
	}
	var out []webdb.MCPScopeEntry
	for _, e := range in {
		if e.ID == nil {
			return bad("mcp.scope_bad_entry", "kind", e.Kind, "id", "")
		}
		out = append(out, webdb.MCPScopeEntry{Kind: webdb.MCPScopeKind(e.Kind), ID: *e.ID})
	}
	out = webdb.SortMCPScope(out)
	if len(out) > webdb.MaxMCPScopeEntries {
		return bad("mcp.scope_too_long", "max", strconv.Itoa(webdb.MaxMCPScopeEntries))
	}
	var users []int64
	for _, e := range out {
		id := strconv.FormatInt(e.ID, 10)
		switch e.Kind {
		case webdb.ScopeFederation:
			if e.ID < 0 {
				return bad("mcp.scope_bad_entry", "kind", string(e.Kind), "id", id)
			}
			_, err := webdb.GetFederation(ctx, s.app, uint64(e.ID))
			if errors.Is(err, webdb.ErrNoFederation) {
				return bad("mcp.scope_bad_entry", "kind", string(e.Kind), "id", id)
			}
			if err != nil {
				s.internalError(w, "scope federation", err)
				return nil, false
			}
		case webdb.ScopeGroup:
			if e.ID >= 0 {
				return bad("mcp.scope_bad_entry", "kind", string(e.Kind), "id", id)
			}
			visible, err := logdb.GroupVisible(ctx, s.ro, sc, e.ID)
			if err != nil {
				s.internalError(w, "scope group", err)
				return nil, false
			}
			if !visible {
				return bad("mcp.scope_bad_entry", "kind", string(e.Kind), "id", id)
			}
		case webdb.ScopePrivate:
			if e.ID <= 0 {
				return bad("mcp.scope_bad_entry", "kind", string(e.Kind), "id", id)
			}
			users = append(users, e.ID)
		default:
			return bad("mcp.scope_bad_entry", "kind", string(e.Kind), "id", id)
		}
	}
	if len(users) == 0 {
		return out, true
	}
	// Private chats: who may give one, before whether it exists, so
	// the refusal says nothing about the archive.
	if v.Role != scope.Superadmin {
		s.refuse(w, r, v, http.StatusForbidden, "forbidden", "mcp.scope_private_superadmins")
		return nil, false
	}
	ok, err := s.privateOwner(ctx, owner)
	if err != nil {
		s.internalError(w, "scope owner", err)
		return nil, false
	}
	if !ok {
		s.refuse(w, r, v, http.StatusForbidden, "forbidden", "mcp.scope_private_owner")
		return nil, false
	}
	known, err := logdb.UserNames(ctx, s.ro, users)
	if err != nil {
		s.internalError(w, "scope users", err)
		return nil, false
	}
	for _, u := range users {
		if _, ok := known[u]; !ok {
			return bad("mcp.scope_bad_entry", "kind", string(webdb.ScopePrivate), "id", strconv.FormatInt(u, 10))
		}
	}
	return out, true
}

// scopeView answers the scopes of a token list: each entry with its
// name and whether it reaches anything now.
type scopeView struct {
	feds    map[int64]string
	titles  map[int64]string
	exposed map[int64]bool
	users   map[int64]string
}

func (s *Server) newScopeView(ctx context.Context, sc scope.Scope,
	scopes map[uint64][]webdb.MCPScopeEntry) (scopeView, error) {
	var sv scopeView
	var groups, users []int64
	for _, es := range scopes {
		for _, e := range es {
			switch e.Kind {
			case webdb.ScopeGroup:
				groups = append(groups, e.ID)
			case webdb.ScopePrivate:
				users = append(users, e.ID)
			}
		}
	}
	fs, err := webdb.ListFederations(ctx, s.app)
	if err != nil {
		return sv, err
	}
	sv.feds = map[int64]string{}
	for _, f := range fs {
		sv.feds[int64(f.ID)] = f.Name
	}
	if sv.exposed, err = logdb.ExposedGroupIDs(ctx, s.ro); err != nil {
		return sv, err
	}
	slices.Sort(groups)
	pg, err := logdb.PinnedGroups(ctx, s.ro, sc, slices.Compact(groups))
	if err != nil {
		return sv, err
	}
	sv.titles = map[int64]string{}
	for _, g := range pg {
		sv.titles[g.ID] = g.Title
	}
	slices.Sort(users)
	sv.users, err = logdb.UserNames(ctx, s.ro, slices.Compact(users))
	return sv, err
}

// entries is one token's scope; privateOK is whether its owner may
// hold private chats.
func (sv scopeView) entries(es []webdb.MCPScopeEntry, privateOK bool) []mcpScopeJSON {
	out := []mcpScopeJSON{}
	for _, e := range es {
		j := mcpScopeJSON{Kind: string(e.Kind), ID: e.ID}
		switch e.Kind {
		case webdb.ScopeFederation:
			j.Name, j.Active = sv.feds[e.ID]
		case webdb.ScopeGroup:
			j.Name, j.Active = sv.titles[e.ID], sv.exposed[e.ID]
		case webdb.ScopePrivate:
			var known bool
			j.Name, known = sv.users[e.ID]
			j.Active = known && privateOK
		}
		out = append(out, j)
	}
	return out
}

// scopeOptions is what the viewer's token form may offer: every
// federation, the exposed groups it sees, and private chats for a
// superadmin.
func (s *Server) scopeOptions(ctx context.Context, sc scope.Scope, v scope.Viewer) (mcpScopeOptions, error) {
	o := mcpScopeOptions{Federations: []fedRef{}, Groups: []mcpGroupRef{},
		Private: v.Role == scope.Superadmin}
	fs, err := webdb.ListFederations(ctx, s.app)
	if err != nil {
		return o, err
	}
	for _, f := range fs {
		o.Federations = append(o.Federations, fedRef{ID: f.ID, Name: f.Name})
	}
	gs, err := logdb.ExposedGroups(ctx, s.ro, sc)
	if err != nil {
		return o, err
	}
	for _, g := range gs {
		o.Groups = append(o.Groups, mcpGroupRef{ID: g.Group.ID, Name: g.Group.Title})
	}
	slices.SortFunc(o.Groups, func(a, b mcpGroupRef) int {
		return cmp.Or(cmp.Compare(a.Name, b.Name), cmp.Compare(a.ID, b.ID))
	})
	return o, nil
}
