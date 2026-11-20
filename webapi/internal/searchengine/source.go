// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package searchengine

import (
	"context"
	"errors"

	"tgloggerd/webapi/internal/live"
)

// HubSource feeds the indexer from the service's live Hub, as one more
// subscriber of the one subscription to the daemon's event stream the
// service holds, so the indexer takes none of the daemon's own
// subscriber slots, nor any of the browsers' (Hub.SubscribeInternal).
// Without a Hub (WEBAPI_LIVE=0), the indexer reads the daemon's stream
// itself (live.DaemonSource).
type HubSource struct {
	Hub *live.Hub
	// Want is the kinds of events the indexer acts on; nil is the
	// messages' (ProfileEvent for a profile indexer).
	Want func(kind string) bool
}

var errHubEnded = errors.New("the live hub ended the subscription")

// isMessageEvent is whether the indexer acts on the event.
func isMessageEvent(kind string) bool {
	switch kind {
	case live.KindMessageNew, live.KindMessageEdited, live.KindMessageDeleted:
		return true
	}
	return false
}

// Stream implements live.Source.
func (h HubSource) Stream(ctx context.Context, since uint64, emit func(live.Event)) error {
	want := h.Want
	if want == nil {
		want = isMessageEvent
	}
	sub, st, err := h.Hub.SubscribeInternal(since, func(e live.Event) (live.Event, bool) {
		return e, want(e.Kind)
	})
	if err != nil {
		return err
	}
	defer sub.Close()
	emit(live.Event{Kind: st.Kind, Seq: st.Seq})
	for _, e := range st.Replay {
		emit(e)
	}
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-sub.Done():
			return errHubEnded
		case <-sub.Lost():
			emit(sub.Resync())
		case e := <-sub.Events():
			emit(e)
		}
	}
}
