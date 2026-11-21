// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package mcp

import (
	"context"
	"database/sql"
	"errors"
	"slices"

	"tgloggerd/webapi/internal/webdb"
)

// A TOKEN'S SCOPE (decisions/2026-11-20-mcp-token-scope.md).
//
// A token may be narrowed to federations, groups and, on a
// superadmin's token, private chats (web_mcp_token_scopes). The
// transport resolves the token's entries once per request into a Reach
// and puts it in the context (WithReach); the tools never read it
// themselves but only through telegram.readable (gate.go), the one
// place where the exposure list and the scope meet. So:
//
//   - No entries is every exposed group, as before; a Reach that was
//     never set is the same, for callers without a token (tests).
//   - With entries only what they reach: exposed groups named by a
//     group entry or in a federation entry, and the private chats.
//     Whether a token is narrowed is decided by its stored entries, so
//     a token whose entries all went inactive reads nothing.
//   - A federation is read as it is now, on every request: groups
//     added to it or taken out of it count from the next request. A
//     federation that no longer exists reaches nothing.
//   - A token a member owns reads only groups the member's own grants
//     let it see on the web, whatever its entries say
//     (decisions/2026-11-21-mcp-token-owner-grants.md): the entries
//     narrow within the grants, and no entries is every exposed group
//     within them. The grants are read on every request too, so a
//     withdrawn grant counts at once. Tokens of superadmins and admins,
//     and tokens without an owner record, are not bounded so.

// Reach is what one request's token may read, before the exposure list
// is applied.
type Reach struct {
	narrowed bool
	groups   map[int64]bool // group entries and non-Default federations' groups
	// defaultFed: a Default entry, reaching the groups in no other
	// federation, which federated lists.
	defaultFed bool
	federated  map[int64]bool
	private    []int64 // ascending; only on a superadmin's token
	// bounded: a member's token, which reads only the groups in
	// granted, whatever narrowed says.
	bounded bool
	granted map[int64]bool
}

// allowsGroup is whether the scope lets the token read group g, if it
// is exposed.
func (r Reach) allowsGroup(g int64) bool {
	if r.bounded && !r.granted[g] {
		return false
	}
	if !r.narrowed || r.groups[g] {
		return true
	}
	return r.defaultFed && !r.federated[g]
}

type reachKey struct{}

// WithReach is ctx carrying the token's reach for the tools.
func WithReach(ctx context.Context, r Reach) context.Context {
	return context.WithValue(ctx, reachKey{}, r)
}

func reachOf(ctx context.Context) Reach {
	r, _ := ctx.Value(reachKey{}).(Reach)
	return r
}

// ResolveScope reads the scope of o's token from the web's schema into
// a Reach. Its private chat entries work only when o.PrivateOK (its
// owner is a superadmin); otherwise they stay stored and reach nothing.
// A member's token is bounded by the member's grants.
func ResolveScope(ctx context.Context, app *sql.DB, o TokenOwner) (Reach, error) {
	var r Reach
	if o.Member {
		gs, err := webdb.MemberGrantedGroups(ctx, app, o.OwnerID)
		if err != nil {
			return Reach{}, err
		}
		r.bounded, r.granted = true, map[int64]bool{}
		for _, g := range gs {
			r.granted[g] = true
		}
	}
	es, err := webdb.MCPTokenScope(ctx, app, o.TokenID)
	if err != nil {
		return Reach{}, err
	}
	if len(es) == 0 {
		return r, nil
	}
	r.narrowed, r.groups = true, map[int64]bool{}
	for _, e := range es {
		switch e.Kind {
		case webdb.ScopeGroup:
			r.groups[e.ID] = true
		case webdb.ScopePrivate:
			if o.PrivateOK {
				r.private = append(r.private, e.ID)
			}
		case webdb.ScopeFederation:
			if e.ID < 0 {
				continue
			}
			if uint64(e.ID) == webdb.DefaultFederation {
				fg, err := webdb.FederatedGroups(ctx, app)
				if err != nil {
					return Reach{}, err
				}
				r.defaultFed, r.federated = true, map[int64]bool{}
				for _, g := range fg {
					r.federated[g] = true
				}
				continue
			}
			fg, err := webdb.FederationGroups(ctx, app, uint64(e.ID))
			if errors.Is(err, webdb.ErrNoFederation) {
				continue
			}
			if err != nil {
				return Reach{}, err
			}
			for _, g := range fg {
				r.groups[g] = true
			}
		}
	}
	slices.Sort(r.private)
	return r, nil
}
