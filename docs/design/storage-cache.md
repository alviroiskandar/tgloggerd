# Storage cache: a capped file store in front of Telegram

Status: designed 2026-11-18, not implemented. The items that implement
it are in the "Storage" section of [../TODO.md](../TODO.md); the
decisions behind it are under [decisions/](decisions/).

## 1. Problem and goals

The file store (`TG_STORAGE_DIR`, rows in `telegram_files`) has been an
archive: every media file the logger sees is downloaded and kept for
ever. On 2026-11-18 it held 979,136 files, 594 GiB, and grew by 2 to
120 GiB a day, mostly media from channels the account is not even a
member of. 3,812 more rows (1.69 TiB) are over the per-file cap
`TG_MAX_STORE_FILE_SIZE` and kept as metadata only, yet each of them
was downloaded in full to hash it. The disk is shared with other
projects and filled once already (2026-10-22), which crashed MySQL and
TDLib.

The store becomes a cache in front of Telegram:

1. a configurable cap on its total size, evicting the least recently
   accessed files first;
2. a configurable retention (default 90 days): files not accessed for
   that long are evicted;
3. a quick way to fetch a file from Telegram by its id, including
   partial (byte range) fetches of big files;
4. no downloads from chats the account is not a member of: only the
   file id is kept, and the file is fetched from Telegram when someone
   views it.

Eviction removes the bytes, never the row: nine foreign keys reference
`telegram_files` with `ON DELETE SET NULL`, so deleting a row would
silently unlink every message, edit, photo and preview that uses it.
An evicted row keeps `on_disk = 0` and enough to fetch it again.

## 2. Architecture

Only tgloggerd may hold the TDLib session, so it gets a small internal
API that the web calls. The API is a control plane: small JSON
requests and replies. File bytes never travel over it; they move
through the repository mount that every container shares at
`/workspace`.

    browser --GET /files/<token>--> web (MediaController)
      stored and present -> served from the store, as before
      otherwise -> POST tgloggerd /v1/files/{id}/fetch {offset, length}
                   tgloggerd: TDLib downloads that range; replies with
                   where the bytes are on the shared mount
      web serves them with newFileResponse(path, offset, length)
    web --every 60 s--> POST tgloggerd /v1/files/access {ids}
    tgloggerd StorageJanitor: retention and cap sweeps

Why not proxy the bytes: Drogon's HttpClient buffers whole bodies and
has no Unix socket support, and its chunked async stream responses
send no body at all to an HTTP/1.0 upstream request, which is what the
documented nginx configuration sends. Serving from the shared mount
keeps sendfile, Range handling and the existing proxy setup working.

## 3. Schema

Migration 000032 changes `telegram_files`, additively and keeping all
data:

- `tg_unique_id VARCHAR(255) NULL`, UNIQUE: TDLib's `remote.unique_id`,
  which is the same for the same file across sessions and accounts.
  It is the dedup key for files recorded without their bytes.
- `sha256` becomes NULL-able, still UNIQUE. NULL means the bytes have
  never been seen.
- `accessed_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP`, set to
  `created_at` for existing rows (see section 9), with the key
  `idx_files_evict (on_disk, accessed_at)` for the sweeps.
- `alias_of BIGINT UNSIGNED NULL`, a foreign key to
  `telegram_files(id)` ON DELETE SET NULL. When a file fetched on
  demand turns out to have the same bytes as another row, the row
  points at that one instead of taking its sha256; readers follow it
  one hop. This keeps every reference and every minted token valid
  without repointing nine foreign keys.
- The `on_disk` comment changes to say what 0 now means: the bytes are
  not stored (evicted, never fetched, or over the per-file cap) and
  can be fetched from Telegram.

Migration 000052 adds `generated_from_file_id BIGINT UNSIGNED NULL`, a
foreign key to `telegram_files(id)` ON DELETE SET NULL with a key of
its own: the video a generated still was taken from (section 9.1).

## 4. Cross-thread TDLib requests

Only the loop thread may call `send_query`; other threads have nothing
but a fire-and-forget send. The fetch service and the API run on their
own threads and need answers, so `TDLib` gains

    std::future<td_api::object_ptr<td_api::Object>>
    request(td_api::object_ptr<td_api::Function> f);

usable from any thread except the loop thread. The caller registers a
promise in a mutex-guarded map under a fresh id from `aux_query_id_`
(already atomic, seeded at 2^62), then calls `client_manager_->send`
directly. `process_response` looks ids it does not find in `handlers_`
up in that map and fulfils the promise. The reply itself wakes
`receive()`, so nothing else needs waking. Callers wait with a timeout
and remove their entry when it expires; requests fail at once while
the client is not authorized or is closing, and every pending promise
is failed when the loop stops.

## 5. Chat membership

The logger does not know today which chats the account is in. It
caches, per chat id, a member flag:

- from `updateSupergroup` (`supergroup.status`) for supergroups and
  channels, and from a new `updateBasicGroup` handler
  (`basicGroup.status`, `is_active`) for basic groups;
- member: Creator, Administrator, Member, and Restricted with
  `is_member`; not a member: Left, Banned, Restricted without
  `is_member`, and an inactive basic group;
- private chats always count as member, and so does a chat whose
  status has not arrived yet, so that nothing is lost by default.

`is_member_chat(chat_id)` answers on the loop thread, where the
download decisions are made.

## 6. Recording files without downloading them

`maybe_download_message_file`, `maybe_store_link_preview` and
`maybe_download_group_photo` stop downloading when

- the chat is not a member chat and `TG_SKIP_NONMEMBER_MEDIA` is on
  (the default), or
- the file's size is known and at least `TG_MAX_STORE_FILE_SIZE`. This
  ends the download of over-cap files just to hash them.

Such a file is recorded instead: `DB::upsertFileMeta` writes
`tg_file_id`, `tg_unique_id`, type, size and original name with
`on_disk = 0` and `sha256` NULL, upserting on `tg_unique_id`, and the
message is linked to the row exactly as the existing index-hit path
links it. Every normal store records `tg_unique_id` too. User profile
photos are not chat files and are not affected.

## 7. The internal API

- HTTP/1.1 and JSON, in one translation unit
  (`src/tgloggerd/InternalApi.cpp`) on Boost.Beast and Asio, which are
  header-only and already in the build image, with nlohmann-json from
  the system. A small pool of `TG_API_THREADS` threads; request bodies
  are capped at 64 KiB.
- Listens on `TG_API_LISTEN`, empty (off) by default. In the
  deployment it is `0.0.0.0:8091`, which inside the container means
  loopback and the tgnet address 10.0.88.5; nothing is published on
  the host.
- Authentication: `Authorization: Bearer <key>`, where the key is the
  keyed BLAKE2b of the label `tgloggerd:internal-api:v1` under
  `WEB_APP_KEY`, the secret the web and the daemon already share (the
  same construction as the file token subkeys). Compared in constant
  time. No new secret to configure.
- A curated set of operations, never a raw TDLib passthrough: nothing
  the web sends can post a message. Joining a chat (added later for the
  admin join page) takes only an invite link or a public username, which
  the daemon parses itself.

Endpoints (the full contract is [../internal-api.md](../internal-api.md)):

| Endpoint | Does |
|---|---|
| `GET /v1/files/{id}` | Status: on_disk, size, type, ext, alias_of, whether it can be fetched |
| `POST /v1/files/{id}/fetch` | Makes `{offset, length}` local (section 8) and says where the bytes are |
| `POST /v1/files/access` | Records accesses for a batch of ids (section 10) |
| `GET /v1/storage` | Stored bytes, cap, retention, dry-run flag, last sweep |
| `POST /v1/storage/sweep` | Runs a sweep now, optionally as a dry run |

Errors: 400 malformed, 401 bad bearer, 404 unknown file, 410 gone from
Telegram or not accessible to the account, 416 a fetch offset past the
end of the file, 503 busy or not authorized (with `Retry-After`), 504
timed out.

## 8. Fetching on demand

Resolving a row to a TDLib file:

1. `getRemoteFile(tg_file_id)`, then try the download;
2. on a file reference or access error, fetch the row's source again
   and take the file from it: the newest message referencing it
   (`getMessage`, then the same extraction as ingest,
   `message_content_file` or `link_preview_image`), the group photo
   (`getChat`) or the user photo (`getUser`), a photo of the user's or
   group's photo list (by its unique id), the custom emoji whose
   sticker it is (`getCustomEmojiStickers`), the message effect
   holding it (`getMessageEffect`, by its unique id) or the community
   whose photo it is (`getCommunityFullInfo`, by its unique id).
   TDLib's file database is off, so a fresh source is the reliable way
   to a valid reference.

The row-to-TDLib-file mapping is cached in memory.

A generated still (section 9.1) is never resolved: Telegram never had
it, and its message's sources would hand over the message's own
Telegram thumbnail instead. When its blob is not in the store the
fetch is answered 410 `generated` at once (docs/internal-api.md).

Small files (under `TG_MAX_STORE_FILE_SIZE`) are downloaded whole at
priority 32 and handed to the existing `storeDownloadedFile` path,
which hashes and copies them and marks the row stored with
`accessed_at = NOW()`. If the bytes already belong to another row, the
row gets `alias_of` instead and that row's blob is made present. The
reply says `stored` with the id of the row holding the bytes, and the
web serves from the store as usual. With `TG_PRUNE_TDLIB_FILES` on,
TDLib's copy is deleted a minute after storing rather than at once:
ingest may have picked up the same completed download for a message
and still be about to hash it.

Big files are never copied into the store. The service runs
`downloadFile(id, 32, offset, min(length, TG_FETCH_CHUNK), true)`, asks
`getFileDownloadedPrefixSize(offset)`, and replies `partial` with
TDLib's `local.path` and how many bytes from `offset` are there. The
web checks that the path is under `/workspace/<TG_DATA_DIR>/tdlib/`
before serving it.

Limits and housekeeping:

- downloads of the same file are serialized (a second `downloadFile`
  with another range would end the first one's wait);
- at most `TG_FETCH_MAX_ACTIVE` fetches run at once, each bounded by
  `TG_FETCH_TIMEOUT_SECS`;
- a file found gone from Telegram is remembered as such for an hour;
- a partial file is deleted from TDLib's cache (`deleteFile`) once it
  has not been asked for in `TG_FETCH_IDLE_SECS`;
- a file the ingest pipeline is already downloading is waited for, not
  requested again with a conflicting range. Ingest asks for the whole
  file (offset 0, no limit), so a small file's fetch asks the same and
  only raises the priority; a big file's fetch does that too and waits
  for its range to arrive.

## 9. Eviction: the StorageJanitor

A thread of its own, on the pattern of `DiscordForwarder::refresh_loop`,
woken every `TG_STORAGE_SWEEP_SECS` and on request:

1. Retention: rows with `on_disk = 1` and `accessed_at` older than
   `TG_STORAGE_RETENTION_DAYS` (0 turns it off), oldest first, in
   batches of 1000.
2. Cap: if `SUM(file_size)` over `on_disk = 1` exceeds
   `TG_STORAGE_MAX_BYTES` (0 means no cap), the least recently
   accessed rows until the total is at most 95% of the cap.

One eviction is

    UPDATE telegram_files SET on_disk = 0, updated_at = updated_at
     WHERE id = ? AND on_disk = 1 AND accessed_at = ?

and only when that changed exactly one row is the blob unlinked. An
access or a re-store in between moves `accessed_at`, the guard misses
and the file stays. Ingest's upsert sets `accessed_at = NOW()` when it
meets a known file, so a file seen again is fresh. What remains, a
blob unlinked just after a re-store, heals itself: the web treats a
missing blob as a file to fetch.

`TG_STORAGE_DRY_RUN` makes a sweep report what it would evict (files
and bytes, for retention and for the cap) without changing anything.
It is on unless set to 0: the deployed containers rebuild and migrate
from the checkout whenever they restart, so a janitor that evicted by
default could delete archived bytes before anyone chose to.
Batches are paced to avoid bursts of IO (a second between real
batches). Results are logged and returned by `GET /v1/storage`. The
first sweep runs a minute after start; `TG_STORAGE_SWEEP_SECS=0`
leaves only the sweeps asked for over the API. A sweep stops between
two rows when the daemon shuts down.

Seeding: the migration sets `accessed_at = created_at`. A one-off
script, run as root at the cutover because it reads both schemas,
raises it to the latest real signal of use: the web audit's `media`
rows (authenticated views since July 2026), the newest message that
references the file (a file still being posted is alive) and Discord
media forwards. Retention then evicts only files that are old and
were never seen again.

### 9.1 Generated stills

The videos, animations, video notes and video (.webm) stickers that
have neither a cover nor a Telegram thumbnail get a still taken from
their stored bytes (docs/design/message-types.md section 4.3,
"Generated stills"). A still is a small file of the store like any
other: put in it through the same path as a download (hashed, named by
its digest, under the per-file cap), counted against the total cap,
touched and evicted like the rest. Its row says what it is:
`tg_file_id` `generated:<sha256>`, no `tg_unique_id`, file_type
`thumbnail`, `generated_from_file_id` the video's row.

Since Telegram has no copy, an evicted still is not fetched (section
8): the message shows its minithumb or type icon meanwhile, and the
stills pass takes it again from the video the next time it runs over
the archive (at start, with `TG_GENERATE_STILLS` on): a message whose
thumbnail is an evicted generated still is a candidate again. The
same frame of the same video, taken by the same ffmpeg, gives the same
bytes, so the still usually comes
back as the same row (a still with other bytes, from another ffmpeg,
is a new row that takes the evicted one's place on the messages). A
video whose own bytes are evicted gets no
still until they are stored again.

## 10. Access tracking

`accessed_at` is written by the daemon only; the web stays read-only
on `telegram_files`, and no new MySQL grant is needed. The web adds
the id of every file it serves or fetches to a process-wide set and
flushes it every 60 seconds to `POST /v1/files/access`, best effort: a
failed flush never fails a request. The daemon sets
`accessed_at = NOW()`, keeping `updated_at`, for rows whose value is
more than an hour old.

Files are served with `Cache-Control: public, max-age=604800` instead
of a year and `immutable`, so a browser that keeps viewing a file asks
again at least weekly and its views count.

File access times on disk are not used: the store is mounted
`noatime`, so they hold no history; remounting the shared filesystem
would affect other projects; on btrfs an atime update turns every read
into a metadata write; and any reader, hashing or backup alike, would
count as an access.

## 11. The web

`MediaController::serve`:

1. follows `alias_of`;
2. serves a stored file whose blob is present, as before;
3. otherwise asks the daemon to fetch, turning the Range header into
   offset and length: `stored` is served from the store, `partial` as
   a 206 of the bytes available, with the file's full size in
   Content-Range. An open-ended range gets at most one chunk, which
   media players handle by asking for the next;
4. answers 503 with `Retry-After` when the daemon is busy or away, and
   410 when Telegram no longer has the file.

A request with only a token, no session, may fetch small files only;
streaming a big file needs a session (403 otherwise). That bounds what
a leaked token can cost. With `WEB_TGLOGGERD_API` empty nothing is
fetched and an unstored file stays a 404.

Pages stop calling unstored media "too large, not stored". Photos and
stickers render as usual and are fetched when viewed; video, audio and
documents offer "Load from Telegram" (video with `preload="none"`).
The chat JSON gains a `fetchable` flag. `web/docs/media.md` documents
how `/files/<token>` behaves.

## 12. Configuration

| Variable | Default | Deployment |
|---|---|---|
| `TG_API_LISTEN` | empty (off) | `0.0.0.0:8091` |
| `TG_API_THREADS` | 4 | default |
| `TG_STORAGE_MAX_BYTES` | 0 (no cap) | sized to the disk |
| `TG_STORAGE_RETENTION_DAYS` | 90 (0 = off) | 90 |
| `TG_STORAGE_SWEEP_SECS` | 600 (0 = on request only) | default |
| `TG_STORAGE_DRY_RUN` | 1 (report only) | 1 for the first run, then 0 |
| `TG_SKIP_NONMEMBER_MEDIA` | 1 | default |
| `TG_FETCH_MAX_ACTIVE` | 4 | default |
| `TG_FETCH_CHUNK` | 16 MiB | default |
| `TG_FETCH_TIMEOUT_SECS` | 30 | default |
| `TG_FETCH_IDLE_SECS` | 900 | default |
| `TG_GENERATE_STILLS` | 0 (off; `dry` counts) | on once the stills service runs |
| `TG_STILLS_SOCKET` | `/run/tgloggerd-stills/stills.sock` | default |
| `WEB_TGLOGGERD_API` (web) | empty (fetching off) | `http://10.0.88.5:8091` |

Every variable is documented in `env/settings.env.example`
(env/README). The daemon's database pool grows by one connection per
new thread that uses it.
