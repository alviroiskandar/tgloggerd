// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

// Package live carries the daemon's events to the browsers' live
// updates (docs/design/web-live.md). One subscription to the daemon's
// GET /v1/events feeds a Hub, which keeps the recent events for resuming
// and fans them out to the subscribers of GET /api/events, each through
// a bounded buffer. Where the events come from is a Source, so a broker
// can replace the daemon's stream without touching the fan-out.
//
// An event says what changed, never the new content: a page refetches
// what it names through the API, which applies the viewer's access.
package live

import "encoding/json"

// The kinds of event. The daemon may add kinds; the Hub drops the ones
// it does not know, since it cannot tell who may see them.
const (
	KindHello          = "hello"
	KindResync         = "resync"
	KindSession        = "session"
	KindMessageNew     = "message.new"
	KindMessageEdited  = "message.edited"
	KindMessageDeleted = "message.deleted"
	KindUserUpdated    = "user.updated"
	KindGroupUpdated   = "group.updated"
	KindFileStored     = "file.stored"
	KindStatsChanged   = "stats.changed"
	KindBotUpdated     = "bot.updated"
)

// Reasons of a session event, after which the stream ends.
const (
	ReasonRevoked      = "revoked"
	ReasonScopeChanged = "scope_changed"
)

// Event is one event of the daemon's stream (docs/internal-api.md,
// GET /v1/events), or one this service makes: resync, session.
type Event struct {
	Seq       uint64   `json:"seq,omitempty"`
	Kind      string   `json:"kind"`
	ChatID    int64    `json:"chat_id,omitempty"`
	MessageID int64    `json:"message_id,omitempty"`
	UserID    int64    `json:"user_id,omitempty"`
	FileID    uint64   `json:"file_id,omitempty"`
	BotID     uint64   `json:"bot_id,omitempty"`
	ChatIDs   []int64  `json:"chat_ids,omitempty"`
	UserIDs   []int64  `json:"user_ids,omitempty"`
	More      bool     `json:"more,omitempty"`
	Fields    []string `json:"fields,omitempty"`
	Reason    string   `json:"reason,omitempty"`
	// RefetchInterval is set on the first frame of a signed-out
	// stream only: the least seconds between two window refetches.
	RefetchInterval int `json:"refetch_interval,omitempty"`
}

// Known reports whether the kind is one the Hub passes on.
func Known(kind string) bool {
	switch kind {
	case KindMessageNew, KindMessageEdited, KindMessageDeleted,
		KindUserUpdated, KindGroupUpdated, KindFileStored,
		KindStatsChanged, KindBotUpdated:
		return true
	}
	return false
}

// MarshalJSON writes the event as the daemon does; a stats.changed
// always carries both lists, empty or not.
func (e Event) MarshalJSON() ([]byte, error) {
	type plain Event
	if e.Kind != KindStatsChanged {
		return json.Marshal(plain(e))
	}
	chats, users := e.ChatIDs, e.UserIDs
	if chats == nil {
		chats = []int64{}
	}
	if users == nil {
		users = []int64{}
	}
	return json.Marshal(struct {
		Seq     uint64  `json:"seq"`
		Kind    string  `json:"kind"`
		ChatIDs []int64 `json:"chat_ids"`
		UserIDs []int64 `json:"user_ids"`
		More    bool    `json:"more,omitempty"`
	}{e.Seq, e.Kind, chats, users, e.More})
}
