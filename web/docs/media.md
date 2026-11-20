# Media: `/files/<token>`

How the web serves files, from the store or fetched from Telegram
through the daemon. The design behind it is
[../../docs/design/storage-cache.md](../../docs/design/storage-cache.md)
sections 10 and 11; the daemon side is
[../../docs/internal-api.md](../../docs/internal-api.md).

## Which service serves it

Two implementations exist while the web moves to Go
([../../docs/design/web-go.md](../../docs/design/web-go.md)): the Drogon
web's (`src/controllers/MediaController.cpp`, `src/media/Fetch.cpp`)
and the Go service's (`webapi/internal/server/media.go`,
`webapi/internal/media/`). nginx decides which one answers: `/files/`
goes to Drogon until a `location /files/` sends it to the Go service
([../../webapi/README](../../webapi/README), "Routing /files/ through
nginx"). Both read the same store, TDLib directory, settings and
tokens, so a link works on either. What follows holds for both, except
where "Differences" says otherwise.

## The URL

`/files/<token>` is public: the token is the file id
(`telegram_files.id`) encrypted under `WEB_APP_KEY`
(`src/auth/FileToken.cpp`), so it cannot be guessed or enumerated, and
no session is needed. A token that does not decrypt answers 404, like
an unknown id.

## Which bytes

- A row with `alias_of` set holds no bytes of its own; the bytes of the
  row it names are served (one hop). The download name and the
  inline-or-download choice still come from the token's row.
- A stored row (`on_disk = 1`) whose blob is present is read from
  `WEB_STORAGE_DIR` at a path derived from the row's SHA-256 and
  extension only, never from the request.
- A single `Range: bytes=...` is honoured with a 206; an unsatisfiable
  one gets 416.
- Anything else is not in the store: evicted, never fetched (media from
  chats the account is not a member of), over the daemon's per-file
  cap, or a stored row whose blob is missing on disk. Such a file is
  fetched from Telegram, below.

## Files not in the store

The web asks the daemon to fetch the file, `POST /v1/files/{id}/fetch`
([internal-api.md](../../docs/internal-api.md)), for the token's row,
and waits for the answer (the daemon's `TG_FETCH_TIMEOUT_SECS` plus 10
seconds at most). The `Range` header becomes the fetch's `offset` and
`length`; without one, or with an open-ended one, it asks from the
start or from the range's first byte for one chunk (`TG_FETCH_CHUNK`).

- `stored`: the file was small enough (under `TG_MAX_STORE_FILE_SIZE`)
  and is now in the store, possibly as another row's bytes; the `id`
  in the answer names the row, and its blob is served exactly as a
  stored file, Range included. It is served from the store from then
  on.
- `partial`: a big file. The web serves the `available` bytes from
  `offset` (fewer if the Range asked for fewer) straight from TDLib's
  cache at `path`, as a 206 whose `Content-Range` total is the file's
  full `size` from the answer, not the size of the partial file on
  disk. A media player asks for the next range as it plays or seeks.
  When the answer covers the whole file from byte 0, it is a 200.
  `path` must name a regular file inside the TDLib directory,
  `<TG_DATA_DIR>/tdlib/` resolved against the working directory
  (`/workspace` in the compose deployment, as for the daemon), after
  resolving `..` and symlinks; anything else is refused with 502 and
  logged. The web reads `TG_DATA_DIR` from the environment it shares
  with the daemon.

Who may fetch is decided in one place (`src/media/Fetch.cpp`,
`mayFetch()`), shared by this route and the chat pages:

- With `WEB_TGLOGGERD_API` empty nothing is fetched: 404 "File not
  stored".
- A request without a session may fetch small files only (under
  `TG_MAX_STORE_FILE_SIZE`, read from the shared environment; any file
  when it is 0). A big file, or a `partial` answer for a file whose
  size was unknown, is refused with 403 "Sign in to load this file".
  That bounds what a leaked token can cost: one small file stored, not
  a stream of a big one.

The daemon's answers map to:

| Daemon | Browser |
|---|---|
| 503 (busy, fetch limit, TDLib not authorized) | 503 with the daemon's `Retry-After` (5 if it sent none) |
| no answer (daemon stopped, unreachable, timed out) | 503 with `Retry-After: 10` |
| 410 (gone from Telegram, not accessible) | 410 "File no longer available" |
| 504 (Telegram slow) | 504; the download goes on, so asking again soon is cheap |
| 404 | 404 |
| 416 (offset past the end of a big file) | 416, `Content-Range: bytes */<file_size>` |
| anything else (400, 401, 500, a malformed answer) | 502, logged |

Served bytes, from the store or from TDLib, count as an access (below)
and, for a signed-in request, write the same `media` audit row as a
stored file.

## Chat pages

The chat JSON (`/v1/chats/{scope}/{id}/messages`, and the data behind
the server-rendered chat pages) gives every media object (message
media, album items, link-preview images, edit snapshots) a `stored`
flag and a `fetchable` flag: `fetchable` is true when the bytes are not
stored and the viewer may fetch them by the rule above (a public view,
without a session, gets it only for small files). The pages render:

- a stored file, or an unstored photo or sticker that is fetchable, as
  usual: images load lazily and are fetched when they come into view;
- an unstored, fetchable video, animation (not an animated sticker),
  audio or document as "Load from Telegram": a video or animation is a
  `<video preload="none">` box, so nothing is fetched before it is
  clicked, that opens in the viewer and plays from there; audio is a
  player with `preload="none"`; a document is a download link with its
  name and size;
- an unstored file that cannot be fetched as "not stored, only
  metadata kept".

The Go service's chat API (`GET /api/chats/{scope}/{id}/messages`, and
the React chat pages' data; webapi/openapi.yaml, ChatMedia) and the
message page (MessageFull.media) apply the same rule to every media
object they nest: message media, album items, edit snapshots,
link-preview images, rich blocks, a message's detail (a game's or
invoice's photo, paid media, a giveaway's sticker) and a service
message's (a chat photo or its animation, a background, a gift's
sticker and symbol). Each carries `url` only when the file is stored,
or fetchable and small (null otherwise; see "Fetch from Telegram"
below for big files), `fetch` when it is fetchable, and `thumb` with
the same URL for pictures.

## Fetch from Telegram (Go service)

Fetching costs Telegram traffic and disk, so on the Go service a page
view fetches only small files by itself, and a big one only when
someone asks for it:

- A page view (the chat API, the files list, a group's Files tab) gives
  an unstored file `url` only when it is under `TG_MAX_STORE_FILE_SIZE`
  (any file when that is 0, a file of unknown size too) and the viewer
  may fetch it: opening it fetches and stores it, as above. This is the
  rule anonymous links have always had, now for signed-in views too.
- A bigger unstored file the viewer may fetch keeps `fetchable` true
  but has `url` null. Its `fetch` is the path of the action,
  `POST /api/files/<token>/fetch` (fetchFile in webapi/openapi.yaml,
  with the session's CSRF token), which every fetchable file has. The
  token is the file's own, so the action reaches only files a page view
  showed the account.
- The action asks the daemon to fetch the file from its first byte and
  answers `{"state", "url", "size"}`: `stored` for a file now (or
  already) in the store, `partial` for a big file being downloaded into
  TDLib's cache. Either way `url` is the `/files/<token>` URL to view or
  download it from then on; a big file streams through it as above,
  for signed-in requests only. A file already stored is answered
  without asking the daemon.
- Who may: superadmins and admins, and members given `fetch_files`; a
  member without it gets 403 `fetch_not_allowed` and the daemon is not
  asked. Without a session it is 401, and with `WEB_TGLOGGERD_API`
  empty 502 `daemon`. The daemon's 404, 410, 503 (with `Retry-After`)
  and 504 become `not_found`, `gone`, `unavailable` and `timeout` with
  the same statuses; anything else is 502 `daemon`.
- Every request that reaches the daemon writes a `file_fetch` audit
  row, `file <id>: ok` or the error code.
- `/files/<token>` itself keeps its rule: without a session small files
  only, with one (and `fetch_files`) any size, so the URL the action
  answers with works, and so does a link handed out earlier.
- A still the daemon generated from a stored video
  (docs/design/message-types.md, "Generated stills";
  `generated_from_file_id` set) is never fetchable: Telegram never had
  it, and the daemon takes it again from the video when it is evicted.
  Not stored, it has `fetchable` false and `url` and `fetch` null for
  every viewer, its link answers 404 "File not stored" and the action
  410 `gone`, neither asking the daemon (which would answer 410
  `generated`).

The decision is made in one place, `Server.mayFetch()` in
`webapi/internal/server/media.go` with `media.Policy`, which tells a
link (`/files/<token>`), a page view and the action apart.

## Video stills

A video, animation, video note or video (.webm) sticker shows a still
picture before it plays, from what Telegram sends; no frame is taken
from the video itself. The daemon stores two kinds of picture with the
message, each a file of its own in the store, under its cap and
eviction like any other (docs/design/message-types.md section 4.3):

- the video's cover, when its sender set one (role `cover`);
- the thumbnail TDLib offers with most videos, when it is a still
  image (JPEG, PNG or WebP; role `thumbnail`, file_type `thumbnail`).
  Messages logged before the daemon stored thumbnails have none.

Besides those, a message logged since migration 000034 records TDLib's
minithumbnail, a tiny blurred JPEG, in its `content_extra`.

The chat API's media (ChatMedia, and the message page's media) carry
the picture as `poster`, which is then also `thumb`: the cover when
stored, else the thumbnail when stored, else the cover when it may be
fetched. Without one, `thumb` is the minithumb's data: URL when there
is one, and `minithumb` is always given when known, for a blurred
placeholder.

The files list's rows (FileRow) of those kinds carry `thumb` as the
stored cover, else the stored thumbnail, of any message the viewer
sees that carried the file (the same video sent again carries the same
pictures), and `minithumb` likewise. `thumb` is never the video's own
URL, and never an unstored picture's, so showing the list fetches
nothing. Both are read for a whole page at once, in one query per
message table. A thumbnail's own row is listed as a `photo`, its own
`thumb`; the `file_type` filter still finds it as `thumbnail`.

## Files list

Every row of the Drogon files list (`/files`) links to the file's
`/files/<token>`, stored or not, so opening an unstored one fetches it
by the rule above. The Go service's files page data (FilesProps,
FileRow in webapi/openapi.yaml) gives each row `url`, `fetchable` and
`fetch` as the chat API does (a big unstored file is fetched through
the action, "Fetch from Telegram"); the list's thumbnails are of stored pictures only (a
picture's own bytes, a video's still: see "Video stills"), so showing
the list fetches nothing. A row names the first message the viewer
sees that carried the file (`chat`, `message`), and its `duration` is
the length in seconds of a video, audio file, voice note or video note
that message stored in its `content_extra`. It is read with the
message, so a page costs no extra query, and is absent when the message
stored none or the viewer sees no message that carried the file.

## Documents (Go service)

A file sent "as a file" (a document) is previewed by its extension,
lower case, as the chat already shows photos, videos and voice notes:
a screenshot sent as a file opens like a photo, a video file plays, a
text file shows its text. The Go service gives such a document its
`render` (MediaRender in webapi/openapi.yaml) in the chat API's media,
the message page, the files list (FileRow) and a group's Files tab
(GroupFileRow):

| render | extensions |
|---|---|
| `image` | jpg, jpeg, png, gif, webp, avif, bmp, ico, apng, svg |
| `video` | mp4, m4v, webm, mov, ogv |
| `audio` | mp3, m4a, aac, ogg, oga, opus, wav, flac |
| `pdf` | pdf |
| `text` | txt, log, md, csv, tsv, json, xml, yaml, yml, toml, ini, conf, cfg, sh, py, c, h, cpp, hpp, go, rs, js, ts, java, kt, sql, diff, patch, html, htm |

Any other extension stays `file`, a download, as before. The table is
`media.DocumentRender` (webapi/internal/media/preview.go). The pages
draw an svg only through an img element, html and htm as their source
text (never rendered), at most the first 512 KiB of a text, and a pdf
as a link that opens in a new tab; a download is offered beside every
preview. Fetching is unchanged: a big unstored document is fetched
only through its "Fetch from Telegram" action.

`/files/<token>` serves a document by the same table, from the store
and when streamed from TDLib's cache alike:

| render | Content-Type | Content-Disposition | X-Content-Type-Options | Content-Security-Policy |
|---|---|---|---|---|
| `image` | the extension's (`image/svg+xml` for svg) | `inline; filename=<name>` | `nosniff` | the sandbox |
| `text` | `text/plain; charset=utf-8`, whatever the extension | `inline; filename=<name>` | `nosniff` | the sandbox |
| `video`, `audio` | the extension's | `inline; filename=<name>` | `nosniff` | none |
| `pdf` | `application/pdf` | `inline; filename=<name>` | `nosniff` | none |
| `file` | the name's, as before | `attachment; filename=<name>` | `nosniff` | none |

- The sandbox is `sandbox; default-src 'none'; img-src 'self';
  style-src 'unsafe-inline'`: an svg, or an html file a browser might
  sniff despite its text type, opened by its URL runs no script and
  loads nothing but its own images. A pdf goes without it, which would
  stop the browser's own viewer.
- The bytes are whatever was sent through Telegram, served from the
  site's own origin, so the same holds for every file answer, from the
  store, fetched or streamed, whatever the file: every answer carries
  `X-Content-Type-Options: nosniff`, and every answer shown inline
  carries the sandbox unless its Content-Type is `video/*`, `audio/*`
  or `application/pdf`. A downloaded file (an attachment) runs
  nothing and carries no sandbox.
- The extension (`file_ext`, the one the render is read from) picks
  the type of a previewed document, not the download name: a png
  named `x.html` is an image. The name is the download name, as for a
  download.
- `?download=1` on any `/files/<token>` URL gives the attachment,
  typed by the download name, with nosniff and without the sandbox;
  photos,
  videos and other media shown in the page get one too (their name is
  the original one, else the blob's). Any other value of `download`
  changes nothing.
- Media other than documents (photos, videos, voice notes, stickers,
  ...) are served inline as before, with no Content-Disposition and
  typed by the extension of the file on disk, plus the two headers
  above: a photo has the sandbox, a video or a voice note only
  nosniff. One whose file on disk would be typed as a page (`html`,
  `htm`, `xhtml`, `xht`) is sent as `text/plain; charset=utf-8`
  instead, and an svg, xml or other active type is sandboxed.

## Caching

Files are sent with `Cache-Control: public, max-age=604800`: a week,
without `immutable`. A token's bytes never change, so they are safe to
cache, but a browser that keeps showing a file asks for it again at
least weekly, and that request counts as an access below. A year and
`immutable`, as before, hid repeat views from the eviction record
entirely.

## Access reporting

The daemon evicts the least recently accessed files, and only it
writes `telegram_files`, so the web tells it which files it serves:

- Every file served (200 or 206) adds the id of the row whose bytes
  were sent, after `alias_of` (for a `stored` fetch, the answer's
  `id`), to a process-wide set. Nothing on the request path waits for
  the daemon.
- Every 60 seconds a timer on the main loop sends the set to
  `POST /v1/files/access` in batches of at most 1000 ids and empties
  it. A file served many times in a minute is reported once.
- Best effort: when the daemon is away, slow (5 second timeout) or
  refuses, that minute's ids are dropped, not retried, and one line is
  logged until a report succeeds again. A file viewed again is simply
  reported again.
- With `WEB_TGLOGGERD_API` empty nothing is recorded or sent.

## Differences between the two

The Go service answers the same URLs with the same status codes and
headers. Where it does more, it is the standard file serving of Go's
`http.ServeContent`, which browsers and media players expect:

- A stored file also carries `Last-Modified` (the blob's), and
  `If-Modified-Since` and `If-Range` are honoured: a browser
  revalidating its copy gets a 304. Drogon sends no validators.
- Several ranges in one `Range` header get a `multipart/byteranges`
  206; Drogon serves the first range only. `bytes=0-` is a 206 of the
  whole file; Drogon makes it a 200.
- A session counts only when it is valid against the account's
  `session_epoch`, as for every other request; Drogon checks only the
  cookie's signature and expiry on this route. A session ended by a
  password change is anonymous on the Go service: it fetches small
  files only and writes no audit row.
- A signed-in account fetches only with the `fetch_files` ability
  (web-accounts.md section 5): superadmins and admins hold it, a
  member only when given it. A member without it gets no link to an
  unstored file (`fetchable` false, `url` and `fetch` null), and a
  link it holds answers 404 "File not stored" without asking the
  daemon.
- Signed-in page views link big unstored files only through the "Fetch
  from Telegram" action (above); the Drogon chat pages link them
  directly, behind a "Load from Telegram" box that fetches when
  clicked.
- A document whose extension the browser can show is previewed
  ("Documents" above); the Drogon chat pages draw every document as a
  download link.
- Error answers are a bare HTML page with the same heading and text,
  not the Drogon layout.

