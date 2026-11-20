// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"encoding/json"
	"net/url"
	"testing"
)

// Stills the daemon generated from stored videos, on the
// "generated_stills" fixture (its header lists each row): Telegram
// never had one, so one not in the store is not fetchable.

// genRows are the files page's rows of the fixture's files, by id, as
// cookie sees them.
func (e *mediaEnv) genRows(cookie string) map[string]map[string]any {
	e.t.Helper()
	q := url.Values{"f": {`[{"field":"id","op":">","value":"892000"},` +
		`{"field":"id","op":"<","value":"892100"}]`}}
	g := e.get("/api/page/files?"+q.Encode(), cookie)
	expect(e.t, g, 200, "")
	var b struct {
		Props filesPageProps `json:"props"`
	}
	if err := json.Unmarshal([]byte(g.body), &b); err != nil {
		e.t.Fatal(err)
	}
	return byFileID(b.Props.Rows)
}

// TestGeneratedStillNotFetchable: an evicted generated still is listed
// as not stored and not fetchable, with no URL and no fetch action, and
// neither its URL nor the action asks the daemon for it.
func TestGeneratedStillNotFetchable(t *testing.T) {
	t.Parallel()
	e := newMediaEnv(t, mediaOpts{cap: filesStoreCap,
		fixtures: []string{"sample", "generated_stills"}})
	ck, _ := e.signIn("lister")

	r := e.genRows(ck)["892011"]
	if r == nil {
		t.Fatal("evicted generated still not listed")
	}
	if r["stored"] != false || r["fetchable"] != false || r["url"] != nil ||
		r["fetch"] != nil || r["thumb"] != nil {
		t.Errorf("evicted generated still: %v", r)
	}
	// A stored one is linked as any stored file.
	if r := e.genRows(ck)["892012"]; r["stored"] != true || r["url"] != e.fileURL(892012) {
		t.Errorf("stored generated still: %v", r)
	}

	// Its URL and its action, had a client kept them, answer without
	// asking the daemon, which would only say it is generated.
	e.fake.answer(892011, 410, "", `{"error":"generated"}`)
	expect(t, e.get(e.fileURL(892011), ck), 404, "")
	expect(t, e.post(e.fetchPath(892011), ck), 410, "")
	if f := e.fake.lastFetch(); f != "" {
		t.Errorf("the daemon was asked for a generated still: %q", f)
	}
}
