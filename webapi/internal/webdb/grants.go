// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package webdb

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"slices"
	"strings"
)

// A member's grants (docs/design/web-accounts.md section 5, web
// migration 000012): the groups granted to it in each federation it is
// filed in (web_member_grant_groups), and its abilities
// (web_member_abilities). A grant row hangs off the member's filing
// (a foreign key to web_federation_members), so leaving a federation,
// deleting it or stopping being a member drops the federation's
// grants; the writes below drop a grant whose group leaves its
// federation. The reads count only grants whose group is still in the
// federation, so a grant left behind by a change made elsewhere never
// shows a group the member's federations do not hold.

// The member abilities, as web_member_abilities stores them (scope's
// Forward, FetchFiles and MCPTokens).
const (
	AbilityForward    = "forward"
	AbilityFetchFiles = "fetch_files"
	AbilityMCPTokens  = "mcp_tokens"
)

// ValidAbility is whether a is a member ability.
func ValidAbility(a string) bool {
	return a == AbilityForward || a == AbilityFetchFiles || a == AbilityMCPTokens
}

// grantInFederation is the condition, over a web_member_grant_groups
// row aliased gg, that its group is still in its federation: stored in
// it, or for Default (id 0) stored in no federation at all. Both halves
// are primary-key or group_id index lookups.
const grantInFederation = "(EXISTS (SELECT 1 FROM web_federation_groups xg " +
	"WHERE xg.federation_id = gg.federation_id AND xg.group_id = gg.group_id) OR " +
	"(gg.federation_id = 0 AND NOT EXISTS (SELECT 1 FROM web_federation_groups xd " +
	"WHERE xd.group_id = gg.group_id)))"

// The grants of an account aliased u, for a member only, as JSON
// (NULL for none); they follow a column list and end with a space.
const (
	// grantedGroupsColumn is the union of a member's granted groups
	// over its federations (groupList).
	grantedGroupsColumn = "(SELECT JSON_ARRAYAGG(gg.group_id) FROM web_member_grant_groups gg " +
		"WHERE gg.web_user_id = u.id AND u.role = 'member' AND " + grantInFederation + ") "
	// grantsColumn is a member's grants by federation, as
	// [[federation, group], ...] (grantMap).
	grantsColumn = "(SELECT JSON_ARRAYAGG(JSON_ARRAY(gg.federation_id, gg.group_id)) " +
		"FROM web_member_grant_groups gg WHERE gg.web_user_id = u.id AND u.role = 'member' AND " +
		grantInFederation + ") "
	// abilitiesColumn is a member's abilities (abilityList).
	abilitiesColumn = "(SELECT JSON_ARRAYAGG(ab.ability) FROM web_member_abilities ab " +
		"WHERE ab.web_user_id = u.id AND u.role = 'member') "
)

// grantMap reads grantsColumn: the groups of each federation,
// ascending; nil for NULL.
func grantMap(s sql.NullString) (map[uint64][]int64, error) {
	if !s.Valid {
		return nil, nil
	}
	var pairs [][2]int64
	if err := json.Unmarshal([]byte(s.String), &pairs); err != nil {
		return nil, fmt.Errorf("grants %q: %w", s.String, err)
	}
	if len(pairs) == 0 {
		return nil, nil
	}
	out := map[uint64][]int64{}
	for _, p := range pairs {
		f := uint64(p[0])
		out[f] = append(out[f], p[1])
	}
	for f := range out {
		out[f] = uniq(out[f])
	}
	return out, nil
}

// abilityList reads abilitiesColumn: ascending, nil for NULL.
func abilityList(s sql.NullString) ([]string, error) {
	if !s.Valid {
		return nil, nil
	}
	var out []string
	if err := json.Unmarshal([]byte(s.String), &out); err != nil {
		return nil, fmt.Errorf("abilities %q: %w", s.String, err)
	}
	slices.Sort(out)
	out = slices.Compact(out)
	if len(out) == 0 {
		return nil, nil
	}
	return out, nil
}

// JoinAbilities writes abilities for a Session ("" for none).
func JoinAbilities(as []string) string {
	return strings.Join(as, ",")
}

// SplitAbilities reads a JoinAbilities list back, nil for "".
func SplitAbilities(s string) []string {
	if s == "" {
		return nil
	}
	return strings.Split(s, ",")
}

// GrantChange replaces a member's granted groups in the federations
// named, and only those: a federation not in Groups keeps its grants
// (another admin's to change). Every federation named must be one the
// member ends filed in; the caller has checked that the groups are the
// federation's.
type GrantChange struct {
	Groups map[uint64][]int64
}

// setGrants applies c to member id's grants inside tx and returns the
// audit details of what changed ("grants 0: none->-100...,-100...").
// Grants of a federation the member is no longer filed in are gone by
// then (the foreign key), so the federations of c that it is not filed
// in are skipped.
func setGrants(ctx context.Context, tx *sql.Tx, id uint64, c GrantChange) ([]string, error) {
	feds := make([]uint64, 0, len(c.Groups))
	for f := range c.Groups {
		feds = append(feds, f)
	}
	slices.Sort(feds)
	var detail []string
	for _, f := range feds {
		var filed int
		if err := tx.QueryRowContext(ctx, "SELECT COUNT(*) FROM web_federation_members "+
			"WHERE federation_id = ? AND web_user_id = ? FOR SHARE", f, id).Scan(&filed); err != nil {
			return nil, err
		}
		if filed == 0 {
			continue
		}
		had, err := int64s(ctx, tx, "SELECT group_id FROM web_member_grant_groups "+
			"WHERE web_user_id = ? AND federation_id = ? ORDER BY group_id FOR UPDATE", id, f)
		if err != nil {
			return nil, err
		}
		want := uniq(c.Groups[f])
		if slices.Equal(had, want) {
			continue
		}
		for _, g := range had {
			if !slices.Contains(want, g) {
				if _, err := tx.ExecContext(ctx, "DELETE FROM web_member_grant_groups "+
					"WHERE web_user_id = ? AND federation_id = ? AND group_id = ?", id, f, g); err != nil {
					return nil, err
				}
			}
		}
		for _, g := range want {
			if !slices.Contains(had, g) {
				if _, err := tx.ExecContext(ctx, "INSERT INTO web_member_grant_groups "+
					"(web_user_id, federation_id, group_id) VALUES (?, ?, ?)", id, f, g); err != nil {
					return nil, err
				}
			}
		}
		detail = append(detail, fmt.Sprintf("grants %d: %s->%s", f, groupsText(had), groupsText(want)))
	}
	return detail, nil
}

// setAbilities replaces member id's abilities with want (nil drops
// them all) and returns the audit detail, "" when nothing changed.
func setAbilities(ctx context.Context, tx *sql.Tx, id uint64, want []string) (string, error) {
	rows, err := tx.QueryContext(ctx, "SELECT ability FROM web_member_abilities "+
		"WHERE web_user_id = ? ORDER BY ability FOR UPDATE", id)
	if err != nil {
		return "", err
	}
	var had []string
	for rows.Next() {
		var a string
		if err := rows.Scan(&a); err != nil {
			rows.Close()
			return "", err
		}
		had = append(had, a)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return "", err
	}
	want = slices.Compact(slices.Sorted(slices.Values(want)))
	slices.Sort(had)
	if slices.Equal(had, want) {
		return "", nil
	}
	if _, err := tx.ExecContext(ctx, "DELETE FROM web_member_abilities WHERE web_user_id = ?", id); err != nil {
		return "", err
	}
	for _, a := range want {
		if _, err := tx.ExecContext(ctx, "INSERT INTO web_member_abilities (web_user_id, ability) "+
			"VALUES (?, ?)", id, a); err != nil {
			return "", err
		}
	}
	return "abilities " + abilitiesText(had) + "->" + abilitiesText(want), nil
}

func abilitiesText(as []string) string {
	if len(as) == 0 {
		return "none"
	}
	return strings.Join(as, ",")
}

// dropGrantsOfGroups removes the grants of groups that left federation
// fed, and, when groups joined a federation other than Default, the
// Default grants of those groups, which are no longer in Default.
func dropGrantsOfGroups(ctx context.Context, tx *sql.Tx, fed uint64, added, removed []int64) error {
	if len(removed) > 0 {
		ph, args := placeholders(removed)
		if _, err := tx.ExecContext(ctx, "DELETE FROM web_member_grant_groups "+
			"WHERE federation_id = ? AND group_id IN ("+ph+")", append([]any{fed}, args...)...); err != nil {
			return err
		}
	}
	if len(added) > 0 && fed != DefaultFederation {
		ph, args := placeholders(added)
		if _, err := tx.ExecContext(ctx, "DELETE FROM web_member_grant_groups "+
			"WHERE federation_id = ? AND group_id IN ("+ph+")",
			append([]any{DefaultFederation}, args...)...); err != nil {
			return err
		}
	}
	return nil
}
