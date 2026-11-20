// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"bytes"
	"encoding/json"
	"errors"
	"html/template"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strconv"

	"tgloggerd/webapi/internal/auth"
	"tgloggerd/webapi/internal/logdb"
	"tgloggerd/webapi/internal/media"
	"tgloggerd/webapi/internal/scope"
	"tgloggerd/webapi/internal/webdb"
)

// /files/<token>: a file's bytes, from the store or fetched from
// Telegram through the daemon, as web/src/controllers/MediaController.cpp
// serves them: the same URL, status codes and headers. The behaviour is
// described in web/docs/media.md.

// MediaConfig is where the files are and who may fetch them.
type MediaConfig struct {
	// StorageDir is the store's root (WEB_STORAGE_DIR).
	StorageDir string
	// TDLibDir is the canonical TDLib directory partial files must be
	// in (media.TDLibDir of TG_DATA_DIR); "" refuses every partial.
	TDLibDir string
	// StoreFileCap is TG_MAX_STORE_FILE_SIZE (0 = no cap).
	StoreFileCap uint64
}

// fileCacheControl keeps a file cacheable for a week, without
// "immutable": a token's bytes never change, but a browser that keeps
// showing a file asks for it again, and that request is what keeps the
// file from being evicted as unused.
const fileCacheControl = "public, max-age=604800"

var statusTmpl = template.Must(template.New("status").Parse(`<!DOCTYPE html>
<html lang="en">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>{{.Heading}}</title></head>
<body><h1>{{.Heading}}</h1><p>{{.Message}}</p></body>
</html>
`))

// statusPage answers with a small HTML page, as the Drogon web's
// status pages do; these answers go to a browser following a link or
// loading an image, not to the JSON API.
func statusPage(w http.ResponseWriter, code int, heading, message string) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.WriteHeader(code)
	_ = statusTmpl.Execute(w, struct{ Heading, Message string }{heading, message})
}

func fileNotFound(w http.ResponseWriter) {
	statusPage(w, http.StatusNotFound, "File not found", "No file exists with that id.")
}

func needsSession(w http.ResponseWriter) {
	statusPage(w, http.StatusForbidden, "Sign in to load this file",
		"This file is not in storage. Large files are loaded from "+
			"Telegram for signed-in users only.")
}

// unavailable is a 503 with Retry-After: the daemon is busy, away, or
// not signed in to Telegram; asking again shortly may work. A
// Retry-After that is not a number of seconds becomes 5.
func unavailable(w http.ResponseWriter, retryAfter string) {
	if _, err := strconv.ParseUint(retryAfter, 10, 64); err != nil {
		retryAfter = "5"
	}
	w.Header().Set("Retry-After", retryAfter)
	statusPage(w, http.StatusServiceUnavailable, "Telegram is not reachable right now",
		"This file is not in storage and could not be fetched from "+
			"Telegram at the moment. Try again shortly.")
}

// badGateway refuses an answer the daemon should never give, saying
// why in the log only.
func (s *Server) badGateway(w http.ResponseWriter, id uint64, why string) {
	s.log.Warn("media: fetch failed", "file", id, "why", why)
	statusPage(w, http.StatusBadGateway, "Fetch failed",
		"The file could not be fetched from Telegram.")
}

func (s *Server) mediaError(w http.ResponseWriter, what string, err error) {
	s.log.Error("media: "+what, "err", err)
	statusPage(w, http.StatusInternalServerError, "Something went wrong",
		"The file could not be served.")
}

// requestScope is what the request may see and do: the one place
// where a request's scope is built (docs/design/web-accounts.md
// section 9). sess is the verified session, nil for a request without
// one; its Role, MustChangePassword and AdminOf (the federations an
// admin administers) are the account's as read on this request
// (auth.Verifier), not the cookie's, and so are an admin's
// restrictions and a member's grants and abilities.
func (s *Server) requestScope(_ *http.Request, sess *auth.Session) scope.Scope {
	if sess == nil {
		return scope.Anonymous()
	}
	role, ok := scope.ParseRole(sess.Role)
	if !ok {
		s.log.Warn("account with an unknown role", "uid", sess.UID, "role", sess.Role)
		return scope.Anonymous()
	}
	return scope.For(scope.Viewer{
		ID:                 sess.UID,
		Role:               role,
		MustChangePassword: sess.MustChangePassword,
		AdminOf:            webdb.SplitIDs(sess.AdminOf),
		HidePrivate:        sess.HidePrivate,
		JoinChat:           sess.JoinChat,
		HideGroups:         webdb.SplitGroupIDs(sess.HideGroups),
		GrantedGroups:      webdb.SplitGroupIDs(sess.GrantedGroups),
		Abilities:          abilitiesOf(webdb.SplitAbilities(sess.Abilities)),
	})
}

// abilitiesOf is a member's stored abilities as the scope takes them.
func abilitiesOf(as []string) []scope.Ability {
	out := make([]scope.Ability, 0, len(as))
	for _, a := range as {
		out = append(out, scope.Ability(a))
	}
	return out
}

// mayFetch decides whether a request may have the daemon fetch a file
// the store does not hold, by the file's size (0 when unknown), whether
// the request has a valid session and how the fetch is asked for
// (media.Policy). It is the one place that decides it, as
// media::mayFetch() is in the Drogon web: /files/<token> asks it
// before calling the daemon (ViaLink), every JSON answer that links a
// file (fileLink) asks it whether to give the link (ViaPage), and the
// "Fetch from Telegram" action asks it too (ViaAction), so narrowing
// who may fetch is done here and holds for all of them. A page view
// links a big unstored file not by itself (media.OnRequest): it takes
// the action. A signed-in account may fetch only with the fetch_files
// ability (scope.Can(FetchFiles)): every superadmin and admin, and a
// member given it (web-member-access); a member without it gets no
// link to an unstored file, no fetch through a link it holds and 403
// from the action.
func (s *Server) mayFetch(sc scope.Scope, size uint64, signedIn bool, via media.Via) media.Decision {
	p := media.Policy{Enabled: s.daemon.Enabled(), StoreCap: s.media.StoreFileCap}
	d := p.MayFetch(size, signedIn, via)
	if _, account := sc.Viewer(); (d == media.Allowed || d == media.OnRequest) &&
		account && !sc.Can(scope.FetchFiles) {
		return media.Denied
	}
	return d
}

// fileLink is how a JSON answer links a file (ChatMedia's, FileRow's
// url, fetchable and fetch).
type fileLink struct {
	// URL is the file's /files/<token> URL: given when the bytes are
	// stored, or when they are not and a page view may have them
	// fetched (a small file), so opening the URL fetches them through
	// the daemon; nil otherwise.
	URL *string
	// Fetchable: not stored, and the request may have the file
	// fetched, by a page view or only through the action.
	Fetchable bool
	// Fetch is the "Fetch from Telegram" action's path
	// (/api/files/<token>/fetch) when Fetchable, else nil.
	Fetch *string
}

// fileLink links a file for a request (mayFetch, ViaPage). A still
// the daemon generated from a stored video (generated) is never
// fetchable: Telegram never had it, and the daemon takes it again from
// the video when it is evicted, so one not stored has no link.
func (s *Server) fileLink(sc scope.Scope, signedIn bool, id, size uint64, stored, generated bool) fileLink {
	if id == 0 {
		return fileLink{}
	}
	tok := s.tokens.Encrypt(id)
	u := "/files/" + tok
	if stored {
		return fileLink{URL: &u}
	}
	if generated {
		return fileLink{}
	}
	switch s.mayFetch(sc, size, signedIn, media.ViaPage) {
	case media.Allowed:
		f := "/api/files/" + tok + "/fetch"
		return fileLink{URL: &u, Fetchable: true, Fetch: &f}
	case media.OnRequest:
		f := "/api/files/" + tok + "/fetch"
		return fileLink{Fetchable: true, Fetch: &f}
	}
	return fileLink{}
}

// fileHead is how a file answer presents the file: shown in the page
// (photos, videos, ...), previewed (a document the browser can show,
// by its extension) or offered as a download.
type fileHead struct {
	// disposition is "attachment" or "inline", sent with name; ""
	// sends none (media shown in the page).
	disposition string
	name        string
	// ctype is the Content-Type; "" takes it from the name of the
	// file served (a blob's or TDLib's, which carry the extension).
	ctype string
}

// headOf is how a file answer presents a file of the type and
// extension, with its download name; download asks for the
// attachment, whatever the file (?download=1).
//
// A document the browser can show (media.DocumentRender) is sent
// inline, typed by its extension (text as text/plain, whatever it
// is). Its extension, not the download name, picks the type, as it
// picks the render the pages draw. Any other document is a download,
// typed by its name, as the Drogon web sends it. set adds nosniff and
// the sandbox.
func headOf(fileType, ext, name string, download bool) fileHead {
	render := media.RenderFile
	if fileType == "document" {
		render = media.DocumentRender(ext)
	}
	switch {
	case download || (!media.ViewInline(fileType) && render == media.RenderFile):
		return fileHead{disposition: "attachment", name: name, ctype: media.ContentType(name)}
	case render != media.RenderFile:
		return fileHead{disposition: "inline", name: name, ctype: media.PreviewType(ext)}
	}
	return fileHead{}
}

// set puts the headers every file answer carries, for the file served
// at path.
//
// The bytes are whatever someone sent through Telegram, served from
// this site's origin, so no answer may become a page that runs
// script: every answer has nosniff, so the browser keeps to the type
// sent, and every one shown inline has the sandbox
// (media.InlineType, media.PreviewSandbox), but for video, audio and
// pdf, which run no script of the file's and whose players the
// sandbox would stop. A file shown in the page whose name on disk
// types it as a page (html, xhtml) goes out as text/plain.
func (fh fileHead) set(h http.Header, path string) {
	h.Set("Accept-Ranges", "bytes")
	h.Set("Cache-Control", fileCacheControl)
	ctype := fh.ctype
	if ctype == "" {
		ctype = media.InlineType(media.ContentType(path))
	}
	h.Set("Content-Type", ctype)
	if fh.disposition != "" {
		// Unquoted, as the Drogon web sends it; SafeFilename has
		// removed the quote, backslash and control characters.
		h.Set("Content-Disposition", fh.disposition+"; filename="+fh.name)
	}
	h.Set("X-Content-Type-Options", "nosniff")
	if fh.disposition != "attachment" && media.SandboxedType(ctype) {
		h.Set("Content-Security-Policy", media.PreviewSandbox)
	}
}

// serveStored sends a blob from the store. http.ServeContent does the
// Range (206, 416) and conditional (If-Modified-Since, If-Range)
// handling.
func (s *Server) serveStored(w http.ResponseWriter, r *http.Request, path string, fh fileHead) {
	f, err := os.Open(path)
	if err != nil {
		// Gone between the check and the open (evicted just now).
		unavailable(w, "1")
		return
	}
	defer f.Close()
	fi, err := f.Stat()
	if err != nil {
		s.mediaError(w, "stat", err)
		return
	}
	fh.set(w.Header(), path)
	http.ServeContent(w, r, "", fi.ModTime(), f)
}

// fetchAnswer is the daemon's 200 answer to a fetch.
type fetchAnswer struct {
	State     string       `json:"state"`
	ID        *json.Number `json:"id"`
	Path      *string      `json:"path"`
	Offset    *json.Number `json:"offset"`
	Available *json.Number `json:"available"`
	Size      *json.Number `json:"size"`
}

// u64 reads a JSON number that must be a non-negative integer.
func u64(n *json.Number) (uint64, bool) {
	if n == nil {
		return 0, false
	}
	v, err := strconv.ParseUint(n.String(), 10, 64)
	return v, err == nil
}

// serveFile answers GET /files/{token}.
func (s *Server) serveFile(w http.ResponseWriter, r *http.Request) {
	ctx := r.Context()

	// The token is the encrypted file id; a forged or mistyped one
	// (which a visitor probing the URL space would produce) does not
	// decrypt and is indistinguishable from a missing file.
	id, ok := s.tokens.Decrypt(r.PathValue("token"))
	if !ok {
		fileNotFound(w)
		return
	}

	// Only a valid session counts: signature, expiry and the account's
	// session_epoch, so a session ended by a password change cannot
	// stream big files or write audit rows. One that cannot be checked
	// counts as none; the request is then served as a public one.
	var sess *auth.Session
	if s.verifier != nil {
		if v, c := s.verifier.Verify(ctx, r); c == auth.Valid {
			sess = &v
		}
	}
	sc := s.requestScope(r, sess)

	meta, err := logdb.GetFile(ctx, s.ro, sc, id)
	if errors.Is(err, logdb.ErrNotFound) {
		fileNotFound(w)
		return
	}
	if err != nil {
		s.mediaError(w, "file lookup", err)
		return
	}

	// A row whose bytes turned out to be another row's names that row
	// in alias_of; the bytes come from it (one hop), the name and type
	// shown stay this row's, the one the token was minted for. A
	// target gone since falls back to this row.
	blob, bytesID := meta, id
	if meta.AliasOf != 0 {
		t, err := logdb.GetBlob(ctx, s.ro, meta.AliasOf)
		switch {
		case err == nil:
			blob, bytesID = t, meta.AliasOf
		case !errors.Is(err, logdb.ErrNotFound):
			s.mediaError(w, "alias lookup", err)
			return
		}
	}

	// Documents are offered as a download, or previewed, under their
	// original name, else the blob's name, else one made from the id;
	// so is any file asked for as a download.
	download := r.URL.Query().Get("download") == "1"
	var name string
	if download || !media.ViewInline(meta.Type) {
		name = media.SafeFilename(meta.OrigName)
		if name == "" {
			if p := media.StorePath(s.media.StorageDir, blob.Hex, blob.Ext); p != "" {
				name = filepath.Base(p)
			} else {
				name = "file_" + strconv.FormatUint(id, 10)
				if e := media.SafeExt(meta.Ext); e != "" {
					name += "." + e
				}
			}
		}
	}
	fh := headOf(meta.Type, meta.Ext, name, download)

	// Only signed-in access is audited. The route is public, and
	// anonymous hits (hotlinked images, crawlers) must not flood the
	// audit log.
	audit := func() bool {
		if sess == nil {
			return true
		}
		uid := sess.UID
		err := webdb.Audit(ctx, s.app, &uid, "media", clientIP(r),
			"file "+strconv.FormatUint(id, 10))
		if err != nil {
			s.mediaError(w, "audit", err)
			return false
		}
		return true
	}

	// Stored, and its blob is where the row says: serve it, and count
	// the use for eviction against the row whose bytes are served. A
	// blob gone from disk is treated as not stored: the daemon notices
	// and fetches it again.
	if blob.OnDisk {
		if p := media.StorePath(s.media.StorageDir, blob.Hex, blob.Ext); media.IsRegular(p) {
			s.access.Record(bytesID)
			if audit() {
				s.serveStored(w, r, p, fh)
			}
			return
		}
	}

	// Not in the store: ask the daemon to fetch it from Telegram, if
	// this request may. A generated still is not on Telegram (the
	// daemon would answer 410 "generated"); it is not stored until the
	// daemon takes it again from its video.
	if meta.Generated {
		statusPage(w, http.StatusNotFound, "File not stored",
			"This picture was taken from a stored video and is not in "+
				"storage now; it is made again from the video.")
		return
	}
	switch s.mayFetch(sc, meta.Size, sess != nil, media.ViaLink) {
	case media.Off:
		statusPage(w, http.StatusNotFound, "File not stored",
			"This file's bytes are not in storage, and fetching files "+
				"from Telegram is not configured.")
		return
	case media.NeedsSession:
		needsSession(w)
		return
	case media.Denied:
		statusPage(w, http.StatusNotFound, "File not stored",
			"This file's bytes are not in storage, and this account may "+
				"not fetch files from Telegram.")
		return
	}

	want := media.ParseFetchRange(r.Header.Get("Range"), meta.Size)
	rep := s.daemon.Fetch(ctx, id, want.Offset, want.Length)
	if !rep.Sent {
		if ctx.Err() == nil {
			s.log.Warn("media: daemon not reached", "file", id, "err", rep.Error)
		}
		unavailable(w, "10")
		return
	}
	if !rep.OK() {
		switch rep.Status {
		case http.StatusServiceUnavailable:
			unavailable(w, rep.RetryAfter)
		case http.StatusGone:
			statusPage(w, http.StatusGone, "File no longer available",
				"This file is no longer available on Telegram.")
		case http.StatusGatewayTimeout:
			statusPage(w, http.StatusGatewayTimeout, "Telegram is slow",
				"Fetching this file from Telegram took too long. It goes "+
					"on in the background; try again shortly.")
		case http.StatusNotFound:
			fileNotFound(w)
		case http.StatusRequestedRangeNotSatisfiable:
			if meta.Size != 0 {
				w.Header().Set("Content-Range", "bytes */"+strconv.FormatUint(meta.Size, 10))
			}
			w.WriteHeader(http.StatusRequestedRangeNotSatisfiable)
		default:
			s.badGateway(w, id, rep.Error)
		}
		return
	}

	var ans fetchAnswer
	dec := json.NewDecoder(bytes.NewReader(rep.Body))
	dec.UseNumber()
	if rep.Body == nil || dec.Decode(&ans) != nil {
		s.badGateway(w, id, "malformed fetch reply")
		return
	}

	switch ans.State {
	case "stored":
		// Now in the store, possibly as another row's bytes (same
		// sha256): serve that row's blob exactly as a stored file.
		sid, ok := u64(ans.ID)
		if !ok || sid == 0 {
			s.badGateway(w, id, "stored reply without an id")
			return
		}
		held, err := logdb.GetBlob(ctx, s.ro, sid)
		if err != nil && !errors.Is(err, logdb.ErrNotFound) {
			s.mediaError(w, "stored lookup", err)
			return
		}
		var p string
		if err == nil && held.OnDisk {
			p = media.StorePath(s.media.StorageDir, held.Hex, held.Ext)
		}
		if !media.IsRegular(p) {
			unavailable(w, "1")
			return
		}
		s.access.Record(sid)
		if audit() {
			s.serveStored(w, r, p, fh)
		}
		return
	case "partial":
	default:
		s.badGateway(w, id, "unknown fetch state '"+ans.State+"'")
		return
	}

	// A big file: Available bytes from Offset are in TDLib's cache at
	// Path. Streaming one is for signed-in requests only; a file of
	// unknown size can turn out big only now.
	if sess == nil {
		needsSession(w)
		return
	}
	off, ok1 := u64(ans.Offset)
	avail, ok2 := u64(ans.Available)
	total, ok3 := u64(ans.Size)
	if !ok1 || !ok2 || !ok3 || ans.Path == nil || avail == 0 || off >= total ||
		avail > total-off {
		s.badGateway(w, id, "malformed partial reply")
		return
	}

	// The daemon checks the path too; never serve one from elsewhere.
	tdpath, ok := media.InsideTDLibDir(s.media.TDLibDir, *ans.Path)
	if !ok {
		s.badGateway(w, id, "partial path outside the TDLib directory: "+*ans.Path)
		return
	}

	// Serve what was asked for and is there, never more: past
	// Available the file may be holes.
	n := avail
	if off == want.Offset && want.Length != 0 && want.Length < n {
		n = want.Length
	}
	s.servePartial(w, r, tdpath, fh, off, n, total, func() bool {
		s.access.Record(bytesID)
		return audit()
	})
}

// servePartial sends n bytes from off of a partial TDLib file whose
// full size is total. Content-Range carries total, not the size of the
// file on disk, which may be shorter. TDLib names its files with their
// extension (videos/file_12.mp4), which picks the Content-Type, as the
// blob name does for a stored file. served runs once the bytes are
// known to be there, before anything is sent.
func (s *Server) servePartial(w http.ResponseWriter, r *http.Request, path string, fh fileHead,
	off, n, total uint64, served func() bool) {
	f, err := os.Open(path)
	if err != nil {
		// Deleted since the daemon answered.
		unavailable(w, "1")
		return
	}
	defer f.Close()
	fi, err := f.Stat()
	if err != nil || uint64(fi.Size()) < off || uint64(fi.Size())-off < n {
		// Shorter on disk than the daemon said.
		unavailable(w, "1")
		return
	}
	if !served() {
		return
	}

	h := w.Header()
	fh.set(h, path)
	h.Set("Content-Length", strconv.FormatUint(n, 10))
	code := http.StatusOK
	if off != 0 || n != total {
		code = http.StatusPartialContent
		h.Set("Content-Range", "bytes "+strconv.FormatUint(off, 10)+"-"+
			strconv.FormatUint(off+n-1, 10)+"/"+strconv.FormatUint(total, 10))
	}
	w.WriteHeader(code)
	if r.Method == http.MethodHead {
		return
	}
	// A seek and a plain copy of the file, so the server can sendfile(2).
	if _, err := f.Seek(int64(off), io.SeekStart); err != nil {
		s.log.Warn("media: partial file seek", "path", path, "err", err)
		return
	}
	if _, err := io.CopyN(w, f, int64(n)); err != nil && r.Context().Err() == nil {
		s.log.Warn("media: partial file cut short", "path", path, "err", err)
	}
}
