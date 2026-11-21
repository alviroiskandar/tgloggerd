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
	"tgloggerd/webapi/internal/mcp"
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
	Kind string `json:"kind"`
	ID   int64  `json:"id"`
	Name string `json:"name"`
	// Active is nil for an entry hidden from the viewer, which is not
	// told whether it reaches anything.
	Active *bool `json:"active"`
	// InactiveReason says why an entry reaches nothing
	// (McpScopeInactiveReason), nil when it is active.
	InactiveReason *string `json:"inactive_reason"`
}

// Why a scope entry is inactive, the first that applies.
const (
	reasonNotExposed     = "not_exposed"
	reasonFederationGone = "federation_deleted"
	reasonPrivateOwner   = "private_not_superadmin"
	reasonUserUnknown    = "user_unknown"
	reasonOutsideGrants  = "outside_owner_grants"
	// Not a reason the entry is inactive but why the viewer is not
	// told (decisions/2026-11-22-mcp-scope-hidden-entries.md).
	reasonHiddenViewer = "hidden_from_viewer"
)

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
// private chat the viewer may not give this token) itself. held is the
// token's entries as stored (nil when minting): a group entry it
// already holds is kept even when the viewer cannot see the group, so
// a form that sends the whole scope back saves; it stays as it is,
// inactive where it reaches nothing. A new entry for such a group is
// 400 like an unknown group.
func (s *Server) tokenScope(w http.ResponseWriter, r *http.Request, sc scope.Scope, v scope.Viewer,
	owner uint64, held []webdb.MCPScopeEntry, in []mcpScopeIn) ([]webdb.MCPScopeEntry, bool) {
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
			if slices.Contains(held, e) {
				continue
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
	// fedSight is over the federations the entries name; its
	// fedGroups and federated also tell whether a federation entry is
	// within what its token's owner sees (fedSeen).
	fedSight
}

// fedSight tells which federations a viewer may be told about, by one
// rule for the token form's options, the entries of the token list and
// the writing of new entries
// (decisions/2026-11-22-mcp-scope-hidden-entries.md,
// 2026-11-22-mcp-scope-federation-options.md).
type fedSight struct {
	// viewer is the scope of the request: an entry for a group it does
	// not see, or for a federation that has groups and none it sees,
	// tells it nothing (sees).
	viewer scope.Scope
	// fedGroups are the groups of each federation named, and
	// federated the groups in any federation (for Default, loaded only
	// when Default is named).
	fedGroups map[int64][]int64
	federated map[int64]bool
	// defaultHidden is whether Default has groups and the viewer sees
	// none of them, worked out only when Default is named and the
	// viewer does not see every group.
	defaultHidden bool
}

// newFedSight loads what fedSight.sees needs for the federations
// named, existing or not.
func (s *Server) newFedSight(ctx context.Context, sc scope.Scope, named map[int64]bool) (fedSight, error) {
	fs := fedSight{viewer: sc, fedGroups: map[int64][]int64{}}
	for id := range named {
		if uint64(id) == webdb.DefaultFederation {
			continue
		}
		gs, err := webdb.FederationGroups(ctx, s.app, uint64(id))
		if err != nil && !errors.Is(err, webdb.ErrNoFederation) {
			return fs, err
		}
		fs.fedGroups[id] = gs
	}
	if !named[int64(webdb.DefaultFederation)] {
		return fs, nil
	}
	fg, err := webdb.FederatedGroups(ctx, s.app)
	if err != nil {
		return fs, err
	}
	fs.federated = map[int64]bool{}
	for _, g := range fg {
		fs.federated[g] = true
	}
	if sc.SeesAllGroups() {
		return fs, nil
	}
	set := logdb.GroupSet{IDs: fg, Not: true}
	seen, err := logdb.CountGroups(ctx, s.ro, sc, set)
	if err != nil || seen > 0 {
		return fs, err
	}
	has, err := logdb.CountGroups(ctx, s.ro, scope.Everything(), set)
	fs.defaultHidden = has > 0
	return fs, err
}

func (s *Server) newScopeView(ctx context.Context, sc scope.Scope,
	scopes map[uint64][]webdb.MCPScopeEntry) (scopeView, error) {
	var sv scopeView
	var groups, users []int64
	named := map[int64]bool{}
	for _, es := range scopes {
		for _, e := range es {
			switch e.Kind {
			case webdb.ScopeGroup:
				groups = append(groups, e.ID)
			case webdb.ScopePrivate:
				users = append(users, e.ID)
			case webdb.ScopeFederation:
				named[e.ID] = true
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
	if sv.fedSight, err = s.newFedSight(ctx, sc, named); err != nil {
		return sv, err
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
// hold private chats, and owner, when not nil, the web scope of a
// member or admin owner (ownerBound), outside which an entry reaches
// nothing (decisions/2026-11-21-mcp-token-owner-grants.md,
// 2026-11-22-mcp-token-owner-view.md). An entry hidden from the viewer
// goes out with no name, active nil and reason hidden_from_viewer,
// whatever it reaches (2026-11-22-mcp-scope-hidden-entries.md).
func (sv scopeView) entries(es []webdb.MCPScopeEntry, privateOK bool, owner *scope.Scope) []mcpScopeJSON {
	out := []mcpScopeJSON{}
	for _, e := range es {
		j := mcpScopeJSON{Kind: string(e.Kind), ID: e.ID}
		var why string
		switch e.Kind {
		case webdb.ScopeFederation:
			var exists bool
			j.Name, exists = sv.feds[e.ID]
			switch {
			case !exists:
				why = reasonFederationGone
			case !sv.sees(e.ID):
				why = reasonHiddenViewer
			case owner != nil && !sv.fedSeen(e.ID, *owner):
				why = reasonOutsideGrants
			}
		case webdb.ScopeGroup:
			// titles holds only the groups the viewer sees.
			j.Name = sv.titles[e.ID]
			switch {
			case !sv.viewer.SeesGroup(e.ID):
				why = reasonHiddenViewer
			case !sv.exposed[e.ID]:
				why = reasonNotExposed
			case owner != nil && !owner.SeesGroup(e.ID):
				why = reasonOutsideGrants
			}
		case webdb.ScopePrivate:
			var known bool
			j.Name, known = sv.users[e.ID]
			switch {
			case !privateOK:
				why = reasonPrivateOwner
			case !known:
				why = reasonUserUnknown
			}
		}
		if why == reasonHiddenViewer {
			j.Name = ""
		} else {
			active := why == ""
			j.Active = &active
		}
		if why != "" {
			j.InactiveReason = &why
		}
		out = append(out, j)
	}
	return out
}

// fedSeen is whether federation fed's entry is within what owner sees
// on the web. For a member, whether the federation holds a granted
// group (for Default, a granted group in no other federation). For an
// admin, unless every exposed group the entry would reach is hidden
// from the admin: an entry reaching no exposed group at all is not out
// of reach because of the admin's hidden groups.
func (sv scopeView) fedSeen(fed int64, owner scope.Scope) bool {
	isDefault := uint64(fed) == webdb.DefaultFederation
	if owner.Role() == scope.Member {
		if isDefault {
			for _, g := range owner.GrantedGroups() {
				if !sv.federated[g] {
					return true
				}
			}
			return false
		}
		for _, g := range sv.fedGroups[fed] {
			if owner.SeesGroup(g) {
				return true
			}
		}
		return false
	}
	return sv.fedVisible(fed, owner)
}

// sees is whether the viewer may be told about federation fed: offered
// it, told what its entry reaches, and let write a new entry for it.
// It may unless the federation has groups, exposed or not, and the
// viewer sees none of them (for Default, every group in no other
// federation). It does not depend on exposure, so nothing changes for
// the viewer when a hidden group is exposed or withdrawn.
func (fs fedSight) sees(fed int64) bool {
	if uint64(fed) == webdb.DefaultFederation {
		return !fs.defaultHidden
	}
	gs := fs.fedGroups[fed]
	if len(gs) == 0 {
		return true
	}
	for _, g := range gs {
		if fs.viewer.SeesGroup(g) {
			return true
		}
	}
	return false
}

// fedVisible is whether sc sees an exposed group federation fed's
// entry would reach, or the entry reaches no exposed group at all: a
// federation is out of sight only when every exposed group it reaches
// is hidden from sc.
func (sv scopeView) fedVisible(fed int64, sc scope.Scope) bool {
	var reached []int64
	if uint64(fed) == webdb.DefaultFederation {
		for g := range sv.exposed {
			if !sv.federated[g] {
				reached = append(reached, g)
			}
		}
	} else {
		for _, g := range sv.fedGroups[fed] {
			if sv.exposed[g] {
				reached = append(reached, g)
			}
		}
	}
	if len(reached) == 0 {
		return true
	}
	for _, g := range reached {
		if sc.SeesGroup(g) {
			return true
		}
	}
	return false
}

// ownerBound is the web scope that bounds the tokens a owns
// (mcp.OwnerScope, the rule /mcp applies): a member's or an admin's;
// nil for a superadmin's.
func ownerBound(a webdb.Account) *scope.Scope {
	sc, bounded := mcp.OwnerScope(a)
	if !bounded {
		return nil
	}
	return &sc
}

// scopeOptions is what the viewer's token form may offer: the
// federations it may be told about (fedSight.sees), the exposed groups
// it sees, and private chats for a superadmin.
func (s *Server) scopeOptions(ctx context.Context, sc scope.Scope, v scope.Viewer) (mcpScopeOptions, error) {
	o := mcpScopeOptions{Federations: []fedRef{}, Groups: []mcpGroupRef{},
		Private: v.Role == scope.Superadmin}
	fs, err := webdb.ListFederations(ctx, s.app)
	if err != nil {
		return o, err
	}
	var sight fedSight
	if !sc.SeesAllGroups() {
		named := map[int64]bool{}
		for _, f := range fs {
			named[int64(f.ID)] = true
		}
		if sight, err = s.newFedSight(ctx, sc, named); err != nil {
			return o, err
		}
	}
	for _, f := range fs {
		// A viewer that sees every group may be told about every
		// federation: the zero sight hides none.
		if sight.sees(int64(f.ID)) {
			o.Federations = append(o.Federations, fedRef{ID: f.ID, Name: f.Name})
		}
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
