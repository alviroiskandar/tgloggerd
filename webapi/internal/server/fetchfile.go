// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"bytes"
	"encoding/json"
	"errors"
	"net/http"
	"strconv"

	"tgloggerd/webapi/internal/logdb"
	"tgloggerd/webapi/internal/media"
)

// "Fetch from Telegram" (web/docs/media.md): the explicit action that
// has the daemon fetch a file the store does not hold. A page view
// links a small unstored file itself, and opening that link fetches
// it; a big one costs Telegram traffic and TDLib disk, so it is
// fetched only when someone asks for it here (mayFetch, ViaAction).
//
// The path takes the file token, not the id: the token is what grants
// a file (logdb.GetFile), and a member only ever gets tokens of files
// it sees, so the action reaches no file the page views do not show.

// fileFetchJSON is the action's answer (FileFetch).
type fileFetchJSON struct {
	State string `json:"state"`
	URL   string `json:"url"`
	Size  uint64 `json:"size"`
}

// fetchFile answers POST /api/files/{token}/fetch.
func (s *Server) fetchFile(w http.ResponseWriter, r *http.Request) {
	noStore(w)
	if !s.checkCSRF(w, r) {
		return
	}
	ctx := r.Context()
	sc := ScopeFrom(ctx)
	v, _ := sc.Viewer()
	tok := r.PathValue("token")
	id, ok := s.tokens.Decrypt(tok)
	if !ok {
		notFound(w)
		return
	}
	meta, err := logdb.GetFile(ctx, s.ro, sc, id)
	if errors.Is(err, logdb.ErrNotFound) {
		notFound(w)
		return
	}
	if err != nil {
		s.internalError(w, "file lookup", err)
		return
	}
	answer := func(state string) {
		writeJSON(w, http.StatusOK, fileFetchJSON{State: state, URL: "/files/" + tok, Size: meta.Size})
	}

	// Already in the store, directly or through alias_of: nothing to
	// ask Telegram for.
	blob := meta
	if meta.AliasOf != 0 {
		t, err := logdb.GetBlob(ctx, s.ro, meta.AliasOf)
		switch {
		case err == nil:
			blob = t
		case !errors.Is(err, logdb.ErrNotFound):
			s.internalError(w, "alias lookup", err)
			return
		}
	}
	if blob.OnDisk && media.IsRegular(media.StorePath(s.media.StorageDir, blob.Hex, blob.Ext)) {
		answer("stored")
		return
	}

	// A still generated from a stored video is not on Telegram: the
	// daemon would answer 410 "generated" without asking it.
	if meta.Generated {
		s.refuse(w, r, v, http.StatusGone, "gone", "fetch.gone")
		return
	}

	switch s.mayFetch(sc, meta.Size, true, media.ViaAction) {
	case media.Allowed:
	case media.Denied:
		s.refuse(w, r, v, http.StatusForbidden, "fetch_not_allowed", "fetch.not_allowed")
		return
	default:
		// Off: no daemon API to ask. NeedsSession and OnRequest are
		// never answered to a signed-in action.
		s.refuse(w, r, v, http.StatusBadGateway, "daemon", "daemon.off")
		return
	}

	// The first chunk, from byte 0: a small file is stored whole, a big
	// one starts downloading into TDLib's cache, and the player asks
	// /files/<token> for the ranges it needs from then on.
	rep := s.daemon.Fetch(ctx, id, 0, 0)
	outcome := "ok"
	defer func() {
		s.auditAdmin(r, v.ID, "file_fetch", "file "+strconv.FormatUint(id, 10)+": "+outcome)
	}()
	if !rep.Sent {
		outcome = "unreachable"
		if ctx.Err() == nil {
			s.log.Warn("fetch: daemon not reached", "file", id, "err", rep.Error)
		}
		s.refuse(w, r, v, http.StatusBadGateway, "daemon", "daemon.unreachable")
		return
	}
	if !rep.OK() {
		switch rep.Status {
		case http.StatusNotFound:
			outcome = "not_found"
			notFound(w)
		case http.StatusGone:
			outcome = "gone"
			s.refuse(w, r, v, http.StatusGone, "gone", "fetch.gone")
		case http.StatusServiceUnavailable:
			outcome = "unavailable"
			ra := rep.RetryAfter
			if _, err := strconv.ParseUint(ra, 10, 64); err != nil {
				ra = "5"
			}
			w.Header().Set("Retry-After", ra)
			s.refuse(w, r, v, http.StatusServiceUnavailable, "unavailable", "fetch.unavailable")
		case http.StatusGatewayTimeout:
			outcome = "timeout"
			s.refuse(w, r, v, http.StatusGatewayTimeout, "timeout", "fetch.timeout")
		default:
			outcome = "error"
			s.log.Warn("fetch failed", "file", id, "status", rep.Status, "err", rep.Error)
			s.refuse(w, r, v, http.StatusBadGateway, "daemon", "fetch.bad_answer")
		}
		return
	}

	var ans fetchAnswer
	dec := json.NewDecoder(bytes.NewReader(rep.Body))
	dec.UseNumber()
	if rep.Body == nil || dec.Decode(&ans) != nil || (ans.State != "stored" && ans.State != "partial") {
		outcome = "error"
		s.log.Warn("fetch: malformed reply", "file", id)
		s.refuse(w, r, v, http.StatusBadGateway, "daemon", "fetch.bad_answer")
		return
	}
	answer(ans.State)
}
