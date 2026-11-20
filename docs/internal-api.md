# tgloggerd internal API

A small HTTP/1.1 + JSON API inside the tgloggerd daemon, for the web.
Only the daemon may hold the TDLib session, so whatever the web needs
from Telegram or from the file store's bookkeeping goes through here.
It is a control plane: small JSON requests and replies. File bytes
never travel over it; they move through the repository mount that
every container shares at `/workspace`. The design behind it is
[design/storage-cache.md](design/storage-cache.md) section 7.

It is a curated set of operations, never a TDLib passthrough: nothing
the web sends can post a message, and the one operation that changes
the account, joining a chat, takes only an invite link or a public
username that the daemon parses itself.

## Running it

| Variable | Default | Meaning |
|---|---|---|
| `TG_API_LISTEN` | empty | `host:port` (or `[v6]:port`) to listen on. Empty: the API is off and nothing listens. |
| `TG_API_THREADS` | 4 | Handler threads. Each may hold a database connection, so the daemon's default DB pool grows by this many when the API is on. |
| `WEB_APP_KEY` | none | The secret the web and the daemon share; the bearer key is derived from it (below). |
| `TG_CONTACT_LOOKUP_INTERVAL` | 300 | Least seconds between two lookups of `POST /v1/users/{id}/lookup-name`; at least 60. |

In the compose deployment `TG_API_LISTEN=0.0.0.0:8091`. Inside the
container that means loopback and the tgnet address 10.0.88.5; the
port is not published on the host, and must not be.

The daemon starts the API after the database and TDLib are set up, and
stops it before anything its handlers use goes away. When the API
cannot start (no or a bad `WEB_APP_KEY`, a bad `TG_API_LISTEN`, a bind
failure) the daemon logs an error and keeps logging Telegram without
it; clients see a refused connection and treat the daemon as away.

## Transport

- HTTP/1.1 with keep-alive; an HTTP/1.0 request is answered and closed
  unless it asks for keep-alive.
- Request bodies are JSON, at most 64 KiB (more: 413). The
  `Content-Type` is not checked. Replies are `application/json` with
  `Cache-Control: no-store`, except for the event stream
  (`GET /v1/events`, `text/event-stream`).
- A connection idle for 30 seconds, or a peer that stalls a read or a
  write for that long, is closed. At most 256 connections are open at
  once; more are closed on accept.
- One I/O thread does the network work; the operations run on the
  `TG_API_THREADS` handler threads and may take a while (a fetch waits
  for Telegram). At most 4 requests per handler thread are queued or
  running; beyond that a request is answered 503 with
  `Retry-After: 1` at once instead of waiting behind slow ones.
- The event stream is the exception: it is served from the I/O thread
  and holds no handler thread however long it stays open, so its
  subscribers cannot take the threads the other operations need. It
  does count against the 256 connections.
- At shutdown the daemon stops accepting, answers the requests already
  running (closing their connections), closes idle connections and
  then exits. A request that arrives on a kept-alive connection during
  shutdown gets 503.
- The query string is accepted and ignored by the endpoints below,
  except for `since` on `GET /v1/events`.

## Authentication

Every request carries

    Authorization: Bearer <key>

where `<key>` is 64 lowercase hex digits: the 32-byte keyed BLAKE2b
(libsodium `crypto_generichash`, output length 32) of the ASCII label

    tgloggerd:internal-api:v1

keyed with the base64-decoded `WEB_APP_KEY`. Any of the standard and
URL-safe base64 alphabets, padded or not, is accepted, and the decoded
key must be at least 16 bytes. This is the construction the
`/files/<token>` subkeys use (`src/tgloggerd/FileToken.cpp`,
`web/src/auth/FileToken.cpp`), with its own label, so there is no new
secret to configure and the key changes when `WEB_APP_KEY` does.

The daemon compares the key in constant time (`sodium_memcmp`) before
it looks at the path, so an unauthenticated client learns nothing about
which endpoints exist. The scheme name is case-insensitive.

To compute the key by hand, for curl:

    python3 -c 'import base64,hashlib,sys; print(hashlib.blake2b(b"tgloggerd:internal-api:v1", key=base64.b64decode(sys.argv[1]), digest_size=32).hexdigest())' "$WEB_APP_KEY"

## Errors

An error is answered with its status and a JSON body

    {"error": "<code>", "message": "<human-readable detail>"}

Clients act on the status and `error`; `message` is for logs and may
change.

| Status | `error` | When |
|---|---|---|
| 400 | `bad_request` | Malformed: a body that is not JSON or not of the endpoint's shape, a bad id, too many ids. |
| 401 | `unauthorized` | Missing or wrong bearer key. Comes with `WWW-Authenticate: Bearer`. |
| 403 | `forbidden` | The deployment does not allow it: a real sweep asked for while `TG_STORAGE_DRY_RUN=1`. |
| 404 | `not_found` | Unknown file or user id, or no such endpoint. |
| 405 | `method_not_allowed` | The path exists under another method; `Allow` lists them. |
| 409 | `not_a_contact`, `unsupported` | The operation does not apply to this user as they are (see the endpoint). |
| 410 | `gone`, `generated` | The file is gone from Telegram or not accessible to the account; `generated`: the file is a still the daemon took from a stored video, which Telegram never had. |
| 413 | `too_large` | Request body over 64 KiB. The connection is closed. |
| 416 | `bad_range` | A fetch's `offset` is at or past the end of the file. |
| 429 | `rate_limited`, `flood_wait` | The daemon's own limit for the operation, or Telegram's flood wait. Comes with `Retry-After` (seconds). Not retried by the daemon. |
| 500 | `internal` | A bug or a database failure; the daemon logs the detail. |
| 502 | `telegram_error` | Telegram refused the operation for another reason; `message` has its error. |
| 503 | `unavailable`, `restore_pending` | Busy (too many requests in flight, a fetch limit reached, another lookup running), the TDLib client not authorized, or the daemon shutting down; `restore_pending`: see the lookup. Comes with `Retry-After` (seconds). |
| 504 | `timeout` | The operation did not finish in time (a fetch waiting on Telegram). |

## Endpoints

File ids are `telegram_files.id`, decimal, 1 to 2^64-1; anything else
in their place is a 400. Times are Unix seconds.

### GET /v1/status

The state of the daemon's TDLib client, for the web's dashboard. It
reads a value the client keeps up to date and never calls TDLib, so it
answers at once in every state, also while the client is not
authorized.

    200 {"tdlib": "ready", "since": 1763400000,
         "account": {"user_id": 7000000001}}

- `tdlib`: `ready` while the client is authorized and not closing,
  exactly when the operations that need Telegram are sent; `waiting`
  before that (still connecting, or the account not logged in);
  `closing` once it is logging out, closing or closed, which does not
  change back.
- `since`: when `tdlib` last changed, or, before the first change, when
  the daemon started.
- `account`: the logged-in account: `user_id` is its own Telegram user
  id (from `getMe` once the client is authorized). Null until then,
  that is before login and in the moment between authorization and
  TDLib's answer; it stays set once known.

Tests: `status_api_test` (in `CMakeLists.txt`) serves this route over a
real listener on loopback with a stand-in for the client's state.

### GET /v1/files/{id}

A file's cache state.

    200 {
      "id": 123,
      "on_disk": true,
      "size": 48213,
      "type": "photo",
      "ext": "jpg",
      "alias_of": null,
      "has_unique_id": true,
      "fetchable": true,
      "accessed_at": 1763400000
    }

- `on_disk`: the bytes are in the store. False means evicted, never
  fetched, or over the per-file cap; such a file can be fetched.
- `size`: `file_size` in bytes (0 if unknown).
- `type`: `telegram_files.file_type` (`photo`, `video`, `document`,
  `audio`, `voice`, `sticker`, `animation`, `video_note`, `thumbnail`
  (a video's still thumbnail), `unknown`).
- `ext`: the lowercase extension without the dot, or null.
- `alias_of`: the id of the row that holds the same bytes, or null.
  Readers follow it one hop (design section 3).
- `has_unique_id`: TDLib's `remote.unique_id` is recorded for the row.
- `fetchable`: whether asking for a fetch can succeed: false for a
  file found gone from Telegram within the last hour (a fetch would be
  answered 410 at once) and for a still generated from a stored video
  (`tg_file_id` `generated:<sha256>`, answered 410 `generated`), true
  otherwise. Every other row keeps its `tg_file_id`, so any other file
  may be fetched.
- `accessed_at`: the file's last recorded use.

Errors: 400 bad id, 404 unknown file.

### POST /v1/files/access

Records that the web used these files (design section 10).

    {"ids": [123, 456, 789]}

    200 {"ids": 3, "updated": 1}

- `ids`: at most 1000 positive integers; duplicates count once and
  unknown ids are ignored.
- For each row whose `accessed_at` is more than an hour old, sets
  `accessed_at = NOW()` and keeps `updated_at`, which records changes
  to the row and an access is not one. Rows accessed within the hour
  are left alone, so a file viewed over and over costs one write an
  hour at most.
- In the reply, `ids` is the number of distinct ids received and
  `updated` the number of rows whose `accessed_at` moved.

The web sends this best effort, batched every 60 seconds; a failed
batch is dropped, never retried into a request path.

Errors: 400 if the body is not `{"ids": [...]}` with positive integer
ids, or carries more than 1000.

### POST /v1/files/{id}/fetch

Gets a file's bytes from Telegram while someone waits for them (design
section 8). The body is optional:

    {"offset": 0, "length": 1048576}

- `offset`: the first byte wanted, default 0.
- `length`: how many bytes from `offset`, default 0, meaning one chunk
  (`TG_FETCH_CHUNK`). The daemon never fetches more than one chunk per
  request; an open-ended or longer request gets a chunk, and the
  client asks again for the rest.

A small file, under `TG_MAX_STORE_FILE_SIZE` (every file when that is
0), is downloaded whole and stored exactly as ingest stores it;
`offset` and `length` are ignored:

    200 {"state": "stored", "id": 123}

- `id`: the row that now holds the bytes. It is the requested id, or,
  when the bytes turned out to be another row's (same sha256), that
  row: the requested row then has `alias_of` set to it. Serve the file
  from the store by that row's sha256 and extension, as for any stored
  file.
- A file already in the store (its blob present, directly or through
  `alias_of`) is answered the same way without asking Telegram.

A big file is never copied into the store. The range is downloaded
into TDLib's cache and the answer says where it is:

    200 {
      "state": "partial",
      "path": "/workspace/data/users/main/tdlib/videos/file_12.mp4",
      "offset": 0,
      "available": 16777216,
      "size": 2147483648
    }

- `path`: TDLib's file, absolute, always under
  `/workspace/<TG_DATA_DIR>/tdlib/` (the daemon checks; a client should
  check again before opening it). Every container mounts the
  repository at `/workspace`, so the web opens the same path.
- `available`: how many bytes from `offset` are present at `path`. At
  least 1, at most `size - offset`; it may be more than was asked for
  when an earlier request already fetched further, and it may be less
  when Telegram delivered only part of the range within the time
  limit. Serve at most `available` bytes from `offset`, and only those:
  the rest of the file may be holes.
- `size`: the file's full size in bytes, for `Content-Range`.

The partial file stays in TDLib's cache while it is in use and is
deleted once no fetch has asked for it in `TG_FETCH_IDLE_SECS`.

How it works, and its limits:

- The file is found by its remote id; when that no longer downloads
  (an expired file reference), something referencing it is fetched
  from Telegram again: the newest live message carrying it, the group
  or user whose current photo it is, the photo list holding it, or the
  custom emoji or message effect whose file it is.
- Downloads of one file run one at a time; a request waits for the one
  in flight (within its own time limit). A file ingest is downloading
  is waited for, not requested again with another range.
- At most `TG_FETCH_MAX_ACTIVE` fetches run at once; more are answered
  503 with `Retry-After: 1` at once.
- Each fetch, from the request to the answer, is bounded by
  `TG_FETCH_TIMEOUT_SECS`; past it the answer is 504 and the download
  goes on in the background, so asking again soon is cheap.
- A file found gone from Telegram (the messages carrying it deleted,
  the chat no longer accessible) is answered 410 for an hour without
  asking Telegram again, and `GET /v1/files/{id}` says
  `"fetchable": false` meanwhile.
- A still the daemon generated from a stored video
  (docs/design/message-types.md section 4.3) is not on Telegram: when
  its blob is in the store it is answered `stored` like any other
  file, else 410 `generated` without asking Telegram (its message's
  sources would carry the message's own Telegram thumbnail, if any,
  never these bytes). The stills pass generates an evicted one again.

| Variable | Default | Meaning |
|---|---|---|
| `TG_FETCH_MAX_ACTIVE` | 4 | Fetches running at once. Keep it at most `TG_API_THREADS`. |
| `TG_FETCH_CHUNK` | 16777216 | Most bytes one request fetches (16 MiB). |
| `TG_FETCH_TIMEOUT_SECS` | 30 | Time limit of one fetch. |
| `TG_FETCH_IDLE_SECS` | 900 | A partial file not asked for this long is deleted from TDLib's cache. |

Errors: 400 bad id or body, 404 unknown file, 410 gone from Telegram
or not accessible (`gone`) or a generated still not in the store
(`generated`), 416 `offset` past the end of a big file, 503 too
many fetches, TDLib not authorized or Telegram asking to wait (with
`Retry-After`), 504 timed out, 500 the downloaded file could not be
stored.

### GET /v1/storage

The store's state and the storage janitor's last sweep (design
section 9).

    200 {
      "files": 812345,
      "bytes": 561903112704,
      "max_bytes": 536870912000,
      "low_watermark": 510027366400,
      "retention_days": 90,
      "sweep_secs": 600,
      "dry_run": true,
      "sweeping": false,
      "pending": false,
      "last_sweep": {
        "started_at": 1763400000,
        "finished_at": 1763400042,
        "dry_run": true,
        "completed": true,
        "error": null,
        "retention": {"files": 120345, "bytes": 40265318400},
        "cap": {"files": 30211, "bytes": 11610427904},
        "bytes_before": 561903112704,
        "bytes_after": 510027366400,
        "kept": 0,
        "missing_blobs": 0,
        "unlink_errors": 0
      }
    }

- `files`, `bytes`: the rows with `on_disk = 1` and the sum of their
  `file_size`, read when asked. This is the logical size the cap is
  measured in; btrfs compression makes the space used lower, and blobs
  no row describes are not counted.
- `max_bytes`: `TG_STORAGE_MAX_BYTES`, 0 for no cap. `low_watermark`:
  what a sweep over the cap evicts down to, 95% of it (0 for no cap).
- `retention_days`: `TG_STORAGE_RETENTION_DAYS`, 0 when off.
  `sweep_secs`: `TG_STORAGE_SWEEP_SECS`, 0 when sweeps only run on
  request.
- `dry_run`: `TG_STORAGE_DRY_RUN`; true means the janitor only reports
  what it would evict. It cannot be changed over the API.
- `sweeping`: a sweep is running now. `pending`: one was asked for and
  has not started yet.
- `last_sweep`: the last sweep that ended, null before the first one.
  - `dry_run`: whether it only reported.
  - `completed`: it went through both phases. False with `error` set
    when it failed (a database error) or was stopped by a shutdown;
    the counts are then what it did before that.
  - `retention`, `cap`: files and bytes evicted by each phase, or, in a
    dry run, that each would evict. Retention goes first; the cap
    phase counts only what is left after it.
  - `bytes_before`, `bytes_after`: the stored bytes when the sweep
    started and when it ended (in a dry run, what would be left).
  - `kept`: files the guard kept because they were accessed or stored
    again while the sweep ran (always 0 in a dry run).
  - `missing_blobs`: evicted files whose blob was already gone, which
    is not an error. `unlink_errors`: evicted files whose blob could
    not be removed; the daemon logs each, and the blob is left as an
    orphan.

### POST /v1/storage/sweep

Asks the janitor for a sweep now.

    {"dry_run": true}

    202 {"queued": true, "dry_run": true}

- The body is optional; without one, or without `dry_run`, the sweep
  runs in the deployment's mode (`TG_STORAGE_DRY_RUN`).
- `"dry_run": false` is refused with 403 while `TG_STORAGE_DRY_RUN=1`,
  so nothing sent over the API can make a report-only deployment evict.
  `"dry_run": true` is always allowed.
- It answers at once; the sweep runs on the janitor's thread, after the
  one running now if there is one. Read its result from
  `GET /v1/storage` once `sweeping` and `pending` are both false.
- Requests made while a sweep is pending merge into it; if any of them
  asked for a real sweep, it is a real one. The reply's `dry_run` is
  the mode the pending sweep will run in.

Errors: 400 if the body is not an object or `dry_run` is not a
boolean, 403 as above.

### POST /v1/chats/resolve

Previews the chat behind an invite link or a public username, for the
web's join page. Read-only on the account.

    {"ref": "https://t.me/+AbCdEfGhIjKlMnOp"}

    200 {
      "outcome": "ok",
      "message": "",
      "ref": "https://t.me/+AbCdEfGhIjKlMnOp",
      "kind": "invite",
      "chat": {
        "chat_id": -1001234567890,
        "title": "GNU/Weeb",
        "type": "supergroup",
        "member_count": 1234,
        "is_member": false,
        "banned": false,
        "needs_approval": false,
        "paid": false,
        "username": null
      }
    }

`ref` is what the admin pasted. The daemon accepts only, after
trimming whitespace:

- invite links `https://t.me/+<hash>` and `https://t.me/joinchat/<hash>`
  (`<hash>`: 4 to 64 of `[A-Za-z0-9_-]`, not all digits: `t.me/+<digits>`
  is a phone number);
- usernames `@<name>`, `<name>` and `https://t.me/<name>` (`<name>`: 4 to
  32 of `[A-Za-z0-9_]`, starting with a letter, not ending with `_`).

The scheme is optional (`http` or `https`), the host may be `t.me`,
`telegram.me` or `telegram.dog` with an optional `www.`, and a trailing
`/`, a query string or a fragment is ignored. Anything else (other
hosts, message, sticker or folder links, `tg://` links, extra path
segments) is a 400, and nothing reaches Telegram.

In the reply, `ref` is the normalised form (`https://t.me/+<hash>` or
`@<name>`) and `kind` is `invite` or `username`. `chat`:

- `chat_id`: TDLib's chat id, or null when the account cannot see the
  chat before joining (a private group behind an invite link).
- `type`: `basic_group`, `supergroup`, `channel`, or `private` for a
  username that belongs to a user or a bot.
- `member_count`: as Telegram reports it; 0 if unknown.
- `is_member`, `banned`: the account's status in the chat, when it can
  see the chat; false otherwise.
- `needs_approval`: joining sends a join request that an administrator
  must approve (the invite link's setting, or the public chat's).
- `paid`: the invite link is a paid subscription; joining it from here
  fails.
- `username`: the chat's public username, if any.

`outcome` is `ok`, or one of the failures below, with `chat` null when
nothing was resolved. A `private` chat answers `not_a_group`.

### POST /v1/chats/join

Makes the account join the chat. Same body as `/v1/chats/resolve`.

    {"ref": "@gnuweeb"}

    200 {"outcome": "joined", "message": "joined", "ref": "@gnuweeb",
         "kind": "username", "chat": {...}}

The daemon resolves the chat again first (the preview may be stale),
answers `already_member`, `banned` or `not_a_group` without asking
Telegram to join, and otherwise calls `joinChatByInviteLink` (invite)
or `joinChat` (username). Joins are serialised: one at a time across
the whole API. `chat` is the preview, with `chat_id` and `is_member`
updated after a successful join; null when the chat could not be
resolved.

Each Telegram-level result is a 200 with an `outcome`; the web shows
`message` (Telegram's own text for `error`):

| `outcome` | Meaning |
|---|---|
| `ok` | (resolve only) the chat was found. |
| `joined` | The account is now a member. |
| `already_member` | It was a member already; nothing was sent. |
| `request_sent` | A join request was sent; an administrator must approve it. |
| `approval_required` | The chat's guard bot must approve the join through a Web App; finish it in a Telegram client. |
| `declined` | The chat's guard bot declined the join. |
| `not_a_group` | The username is a user or a bot. |
| `not_found` | No such username. |
| `link_invalid` | The invite link is invalid, expired or revoked. |
| `flood_wait` | Telegram rate-limited the account; `retry_after` is the wait in seconds (0 if Telegram gave none). |
| `banned` | The account is banned from the chat, or it is private and not accessible to it. |
| `too_many_chats` | The account is in too many chats to join another. |
| `error` | Anything else; `message` is Telegram's error. |

Errors: 400 if the body is not `{"ref": "<string>"}` or the string is
not an invite link or a username; 503 when TDLib is not authorized or
the daemon is shutting down; 504 when Telegram did not answer a step
within 15 seconds. A preview is up to three TDLib requests and a join
one more, so a client should wait about a minute.

Joining has consequences on the account (Telegram's rate limits, a
visible join in the chat): the web asks for a confirmation and audits
every attempt. Once joined, chat-membership tracking picks the chat
up, and its media start being downloaded.

### POST /v1/users/resolve

Finds the user or bot behind a public username and has the daemon store
it, so that the web can link to its user page even when the account
never saw it in a chat (a forwarding bot, say). Read-only on the
account: `searchPublicChat` and `getUser`, then the daemon's own user
refresh (`getUser`, `getUserFullInfo`, the profile photo download). No
joining, no messages.

    {"username": "@FreezingNightBot"}

    200 {
      "outcome": "ok",
      "message": "",
      "user": {"user_id": 123456789, "username": "FreezingNightBot", "is_bot": true}
    }

`username` follows the username rules of `/v1/chats/resolve`: `@<name>`,
`<name>` or `https://t.me/<name>`. An invite link, a malformed name or a
body that is not `{"username": "<string>"}` is a 400, and nothing
reaches Telegram. In the reply, `user.username` is the user's first
active username as Telegram has it (its own capitalisation), and `user`
is null unless `outcome` is `ok`:

| `outcome` | Meaning |
|---|---|
| `ok` | Found; `user` says who. |
| `not_found` | No such username. |
| `not_a_user` | The username is a group or a channel. |
| `flood_wait` | Telegram rate-limited the account; `retry_after` is the wait in seconds (0 if Telegram gave none). |
| `error` | Anything else; `message` is Telegram's error. |

On `ok` the user is stored as a user seen in a message is: its row in
`telegram_users` (names, usernames, type, flags), its full info, and its
profile photo, downloaded into the store with its history. That happens
on the daemon's own loop right after the answer, through the same
refresh the msg_count crossings use, so the row appears within moments
and the photo once its download is done (a user refreshed in the last
few minutes is not fetched again). The web shows the photo once its
file is stored.

An answer of `ok`, `not_found` or `not_a_user` is reused for the same
username (case-insensitively) for 10 minutes without asking Telegram
again, so the call is cheap to repeat. Errors: 503 when TDLib is not
authorized or the daemon is shutting down; 504 when Telegram did not
answer a step within 15 seconds.

### POST /v1/chats/{chat_id}/refresh

Asks Telegram for a group's full info now and stores its member count
(`telegram_groups.member_count`, `member_count_at`, `can_get_members`;
docs/design/group-members.md). No body.

    200 {
      "member_count": 1234,
      "can_get_members": true,
      "is_channel": false,
      "account_is_admin": false,
      "fetched_at": 1763540000
    }

`chat_id` is TDLib's chat id, as stored in `telegram_groups.id`.
`member_count` is null and `fetched_at` (Unix time of the stored count)
is null until a count is known. `can_get_members` says whether the
account may list the members (false until known). `is_channel` and
`account_is_admin` come from TDLib's cached group, without asking
Telegram.

One full-info request reaches Telegram at most once a minute per chat.
Within the minute, or when Telegram fails, the stored row answers; a
failure adds `"error"` with Telegram's message, and `fetched_at` tells
how old the count is. Errors: 400 for a chat id that is not an integer;
404 for a chat not in `telegram_groups`, or one that is not a group or
channel; 503 when TDLib is not authorized; 504 when Telegram did not
answer within 15 seconds.

### GET /v1/chats/{chat_id}/members

One page of a group's members, in the order Telegram gives them for its
"recent" filter.
`offset` (default 0) and `limit` (1 to 200, default 50) are query
parameters; anything else is a 400.

    200 {
      "available": true,
      "reason": "",
      "total": 1234,
      "members": [
        {"user_id": 123456789, "status": "administrator", "joined_at": 1700000000}
      ]
    }

- Supergroups and channels: `getSupergroupMembers` with the recent
  filter. A channel lists its members only where the account is an
  administrator.
- Basic groups: the member list of their full info, paged here.
- `status` is `creator`, `administrator`, `member` or `restricted` (a
  restricted member still in the group); `joined_at` is null when
  Telegram does not say. Members that are not users (an anonymous
  administrator's chat) are left out.
- `total` is Telegram's count of the group's members, or the stored
  count when the list is not available, or null.
- The users in the answer are stored in `telegram_users` by the daemon's
  usual user handling (names, usernames, profile photos): TDLib reports
  every user before an answer mentions it.

When the list is not available, `available` is false, `members` is
empty and `reason` says why:

| `reason` | Meaning |
|---|---|
| `not_allowed` | A channel where the account is not an administrator, or Telegram refused the list (the group hides its members). |
| `not_member` | The account is not in the group. |
| `flood_wait` | Telegram rate-limited the account; `retry_after` is the wait in seconds. |
| `error` | Anything else; `message` is Telegram's error. |

With `q` (percent-encoded, trimmed, a leading `@` dropped, at most 64
characters; longer or not UTF-8 is a 400; empty is the plain list), the
answer is the members matching it, same shape and reasons:

- supergroups and channels: `getSupergroupMembers` with the search
  filter, which matches first and last names and the username;
- basic groups: the full info's members whose first name, last name,
  "first last" or a username contains `q` (case-insensitive for ASCII
  letters);
- a `q` of only digits (a leading `-` allowed) also looks that user up
  with `getChatMember` and, when the user is a member, puts them first
  on the first page (`offset` 0), in addition to the page's `limit`;
- each member carries `"matched_by": "id"` or `"search"`;
- `total` is the number of matches (the id match included) when known,
  else null. A search never updates the stored member count.

`hidden` (a superadmin hid a channel's list) is the web's own reason;
the daemon never answers it. An answer for the same chat, offset and
limit (and `q`) is reused for 2 minutes, except `flood_wait` and `error`. A
successful page also stores the group's count. Errors: 400, 404, 503
and 504 as for `/v1/chats/{chat_id}/refresh`.

### POST /v1/chats/{chat_id}/recheck

Queues a recheck of a chat's messages against Telegram around a message
and answers at once, without waiting for it. The web asks for one when a
chat is opened (the newest stretch) or an older page is loaded while
scrolling up (from that page's newer edge, its `before`, down).

    POST /v1/chats/-1001234567890/recheck
    {"around": 4567}

    202 {"queued": true}
    200 {"queued": false, "reason": "recent"}

`chat_id` is TDLib's chat id (a group's `telegram_groups.id`, or the
peer's user id for a private chat). `around` is a server message id, as
stored in `message_id`: the NEWER edge of the stretch to recheck (an
older page's `before`, or a message page's own id), which is read
mostly downwards from it. 0, the default (also for no body or `{}`), is
the newest stretch. Anything but an integer from 0 up is a 400.

| Answer | Meaning |
|---|---|
| 202 `{"queued": true}` | Queued now. |
| 200 `"reason": "recent"` | That stretch was rechecked or queued too recently: the newest stretch of the chat within the last 60 seconds, an older one (the same chat and `around`) within the last 24 hours. |
| 200 `"reason": "queued"` | That exact recheck is already waiting. |
| 200 `"reason": "busy"` | The queue is full (1000 rechecks); try later. |

Errors: 400 for a chat id that is not an integer or a bad `around`; 404
when neither TDLib (`getChat`, from its cache) nor the database
(`telegram_groups`, `telegram_users`) knows the chat; 503 when TDLib is
not authorized.

What a recheck does, on the TDLib loop:

- One `getChatHistory(chat_id, from, offset, limit, only_local false)`:
  for the newest stretch from 0 (the newest message), offset 0, limit
  50; around a message from it (`around << 20`, TDLib's id), offset
  -10, limit 60: up to 10 newer messages, the message itself and 49
  older.
- Every message returned is stored through the normal path, as if it
  had just arrived: a missing one is inserted (`message.new` event),
  one with a later `edit_date` gets its content replaced and the old
  version kept (`message.edited`). None is forwarded to Discord: only
  live new messages are.
- Deletions: the stored live messages with an id strictly between the
  lowest and highest id returned, but not returned, are marked deleted:
  `deleted_at`, as for a live deletion, and `meta.deleted_on_recheck =
  true`, which tells the two apart. Each sends `message.deleted`; its
  Discord copy is left as it is. A later recheck that finds such a
  message again undoes the deletion (`deleted_at` back to NULL, the
  flag removed, `message.edited` sent, a warning logged); a deletion
  Telegram reported is never undone.
  Nothing outside that range is concluded. Nothing at all is concluded
  from an answer of fewer than two messages, nor from a short one:
  fewer older messages than asked (50 for the newest stretch; around a
  message, 50 at or below it) while an older message is stored, that
  is, while the oldest returned is not the chat's first message. Fewer
  than 10 newer around a message is normal near the newest end and is
  not judged. TDLib can answer short while it loads a stretch; the
  messages such an answer returns are still stored.
- Each recheck logs, at info level, the chat, `around`, how many
  messages came back and how many it marked deleted (a warning above
  10).

What `getChatHistory` can and cannot see: TDLib runs with its message
database on, so it answers from its own database where it holds the
stretch and asks Telegram only for the gaps. A recheck therefore finds
messages TDLib has that the daemon never stored, and edits TDLib knows
of; a message deleted on Telegram is caught only when TDLib's answer
leaves it out, so deletions are concluded only from what TDLib returns,
never from what the database alone holds.

Pacing: rechecks share the Telegram budget of the media relink and the
old-row repair (10 requests a second together), after the relink and
before the repair, at most 2 a second and 2 awaiting their answer. A
flood wait holds every recheck back for its time. Rechecks asked for
run before the daemon's own.

The daemon's own: about two minutes after TDLib is authorized, the
newest stretch of every chat whose last message (from `updateNewChat`
and `updateChatLastMessage`) is at most 48 hours old is queued, newest
first, through the same queue and limits; they count as recent for the
60-second rule. Of those, only the chats with a gap are queued: nothing
stored for the chat, its newest stored message older than TDLib's last
message, or, for a supergroup or channel, fewer than 40 of the 50
message ids up to its newest stored one stored (their ids count up by
one per message, so a dense chat has nearly all of them; basic groups
and private chats number messages per account, so this part does not
apply to them). It covers the chats TDLib has
announced by then, which is every chat in the chat lists when the
history backfill, which loads them, is on. The log says how many were
active and how many had a gap.

Catch-up rechecks run in the background: one starts only while no
other recheck is in flight, so a recheck asked for never waits for
two of them. Under Telegram's flood limits a history read can take a
minute or more inside TDLib.

For a chat TDLib has not opened, its first answer is often only the
newest message or two. A recheck reads on, asking for the 50 messages
from the oldest one returned, up to 4 pages in all: after any page that
brought messages not stored before, full or short, since the hole likely
goes on below it, and after a short first page (fewer older messages
than asked while an older one is stored, so not the chat's start) even
when it brought nothing new, since TDLib's short first answer is the
newest message, usually stored already, while the ones before it are
what is missing. From the second page on, a page with nothing new ends
it, so a quiet chat costs one or two reads. Later pages skip
the queue and its limits, since their recheck was already let in. Each
page is stored like the first, and deletions are concluded from each
page alone, never across pages. Each page's log line names it ("page 2")
with how many of its messages were new.

### GET /v1/users/{user_id}/photos

A user's profile photos as Telegram has them (`getUserProfilePhotos`),
newest first. `offset` (default 0) and `limit` (1 to 100, default 100)
are query parameters.

    200 {
      "outcome": "ok",
      "message": "",
      "total": 3,
      "photos": [
        {"photo_id": "5123456789012345678", "date": 1700000000,
         "file_id": 123456, "video_file_id": null, "is_current": true,
         "animated": false}
      ]
    }

For each photo the daemon records, never twice:

- its largest size in `telegram_files` (`file_id`), metadata only: the
  bytes are fetched like any unstored file, when someone views it
  (`POST /v1/files/{id}/fetch`), and the current photo is downloaded by
  the daemon's own profile photo handling as before;
- a `telegram_user_hist_profile_photo` row dated with the photo's own
  date, unless the history has a row for that file already; a row
  dated later than the photo's own date (dated by when the daemon saw
  the photo) takes the photo's date;
- for an animated photo, its MPEG-4 video (the big animation, else the
  small one) in `telegram_files` (`video_file_id`, file_type `video`),
  metadata only like the still, and the history row's `video_file_id`
  (migration 000051). The video is fetched like the still, through the
  same history source by its own unique id. A user's or a group's full
  info showing an animated current photo links its video the same way
  when the history already has a row for that still.

`photo_id` is TDLib's photo id as a string; `is_current` marks the
user's current photo. A history photo whose bytes are not stored can be
fetched: the fetcher finds it in the user's photo list by its unique id.

`outcome` is `ok`, `flood_wait` (with `retry_after`) or `error` (with
Telegram's `message`); `photos` is empty and `total` null unless `ok`.
An `ok` answer is reused for the same user, offset and limit for 10
minutes. Errors: 400 for a bad id or query; 404 for a user not in
`telegram_users`; 503 when TDLib is not authorized; 504 when Telegram
did not answer within 15 seconds.

### GET /v1/chats/{chat_id}/photos

The same for a group or channel: its photo changes as Telegram keeps
them (`searchChatMessages` with the chat photo filter, newest first,
dated with the photo's date or the change message's), headed by its
current photo when no change message shows it (then `photo_id` and
`date` are null). Each photo is recorded in `telegram_files` and
`telegram_group_hist_photo` as above. 404 for a chat not in
`telegram_groups`; otherwise as `/v1/users/{user_id}/photos`.

### POST /v1/custom-emoji

The stickers behind custom emoji ids, for drawing them (design
`docs/design/message-types.md` section 4.11). The web sends the ids it
is about to draw, taken from text entities, reactions, forum topic
icons or emoji statuses:

    {"ids": ["5368324170671202286", "5370870893004203704"]}

    200 {
      "outcome": "ok",
      "message": "",
      "asked": 1,
      "emoji": [
        {"custom_emoji_id": "5368324170671202286", "emoji": "❤",
         "set_id": "773947703670341676", "format": "tgs",
         "needs_repainting": false, "file_id": 123456}
      ],
      "missing": ["5370870893004203704"]
    }

- `ids`: at most 200 (`getCustomEmojiStickers`'s limit), each a
  nonzero 64-bit integer, as a decimal string or a JSON number;
  duplicates count once. Ids are answered as strings: they do not fit
  a JavaScript number.
- Ids with a `telegram_custom_emoji` row are answered from it. The
  rest are asked of Telegram in one `getCustomEmojiStickers` call, and
  each sticker it returns gets a row and a metadata-only
  `telegram_files` row (`file_type` `sticker`, the format as
  extension): the bytes are fetched like any unstored file, when
  someone views it (`POST /v1/files/{id}/fetch`), under the storage
  cache rules. A row once filled is kept; asking again costs no call
  to Telegram.
- `asked`: how many ids this request sent to Telegram (0 when all were
  known).
- `emoji`: a row per id known, in the order asked. `set_id` is null
  when Telegram gave none, `format` (`webp`, `tgs`, `webm`) when it is
  unknown, `file_id` when the sticker had no file to record.
  `needs_repainting`: drawn in the text colour, like an emoji status.
  `emoji` is the emoji it stands for, at most 16 characters.
- `missing`: ids Telegram does not know. Such an id is answered as
  missing for an hour without asking Telegram again.
- `outcome`: `ok`, or how the call to Telegram failed: `flood_wait`
  (with `retry_after`, seconds), `unavailable` (TDLib not authorized),
  `timeout` (no answer within 15 seconds) or `error` (with Telegram's
  `message`). The rows already stored are answered all the same; an id
  in neither `emoji` nor `missing` was not resolved, and may be asked
  for again later.

A sticker whose file reference has expired is found again by asking
for its custom emoji afresh.

Errors: 400 if the body is not `{"ids": [...]}` with nonzero 64-bit
ids, or carries more than 200 distinct ones.

Tests: `emoji_api_test` (in `CMakeLists.txt`) serves this route and
`POST /v1/message-effects` over a real listener on loopback, with
stand-ins for the tables and for TDLib answering with hand-built
objects.

### POST /v1/message-effects

The same for message effects (`meta.effect_id`; design section 4.12),
stored in `telegram_message_effects`:

    {"ids": ["5104841245755180586"]}

    200 {
      "outcome": "ok",
      "message": "",
      "asked": 1,
      "effects": [
        {"effect_id": "5104841245755180586", "emoji": "🔥",
         "is_premium": false, "type": "emoji_reaction",
         "static_icon_file_id": 123457,
         "select_animation_file_id": 123458,
         "effect_animation_file_id": 123459,
         "sticker_file_id": null, "sticker_format": null}
      ],
      "missing": []
    }

- `ids`: at most 50, as for `/v1/custom-emoji`. An effect without a
  row is asked of Telegram with `getMessageEffect`, one call per
  effect, at most 20 seconds for all of them; a failure ends the calls
  for the rest of the request.
- `type`: `emoji_reaction` (its `select_animation_file_id` and
  `effect_animation_file_id`, both TGS), `premium_sticker`
  (`sticker_file_id` in `sticker_format`, and its premium animation,
  TGS, as `effect_animation_file_id`), or null for a type the daemon
  does not know. `static_icon_file_id` is the static icon (WEBP), null
  when the effect has none; `emoji` stands in for it. Every file is a
  metadata-only `telegram_files` row of type `sticker`, fetched as
  above, and found again by asking for the effect afresh.
- `asked`, `missing` (an effect Telegram answers 404 for) and
  `outcome` as for `/v1/custom-emoji`.

Errors: 400 as for `/v1/custom-emoji`, with at most 50 distinct ids.

### POST /v1/users/{id}/lookup-name

"Look up real name": reads the own name of a user who is a contact of the
logging account. Telegram shows an account a contact's name as the account
saved it, and the user's own name only while they are not a contact, so the
daemon removes the contact, reads the name TDLib then reports, and adds the
contact back exactly as it was. One user per request, never in bulk; the
web offers it as an admin action on a contact's page.

No body. `{id}` is a positive decimal user id.

    200 {
      "user_id": 742278412,
      "first_name": "Moe",
      "last_name": "",
      "contact_first_name": "Michael William Jonathan",
      "contact_last_name": "",
      "checked_at": 1763450000
    }

`first_name`/`last_name` are the own name, now stored in
`telegram_users.first_name`/`last_name` with `own_name_checked_at`;
`contact_*` are the contact name as it was, and is again.

The cycle (`src/tgloggerd/ContactLookup.cpp`):

1. Any contact an earlier cycle left removed is added back first; if one
   cannot be, the request is refused with `restore_pending` and nothing
   else happens.
2. A row in `telegram_contact_lookups` records the contact as the account
   has it: the contact name, the phone number if the account knows it, the
   private note (`userFullInfo.note`, with its formatting) and whether the
   account shares its phone number with the user
   (`!userFullInfo.need_phone_number_privacy_exception`).
3. `removeContacts([id])`, then up to 10 seconds for TDLib to report the
   user as no contact, with their own name.
4. `addContact(id, importedContact{phone, first, last, note},
   share_phone_number)` with the journalled values. A flood wait of up to
   30 seconds is waited out and the re-add tried again, up to 3 times.
5. The own name and the check time are stored, and only then the journal
   row is deleted.

A daemon that stops between 3 and 4 finds the row at its next start and
adds the contact back before anything else, retrying until it succeeds. A
re-add that keeps failing while the daemon runs leaves the row, answers
`restore_pending`, and is retried by the next lookup request or start.

Side effects the web states before an admin confirms: the user can briefly
see a change (the mutual-contact status, the account's phone number), and
per-contact settings TDLib does not expose, such as a custom photo set for
the contact, are not restored.

Errors:

- 400 bad id.
- 404 `not_found`: TDLib does not know the user.
- 409 `not_a_contact`: nothing to look up. 409 `unsupported`: the note has
  formatting `addContact` would not take back; nothing changed.
- 429 `rate_limited`: less than `TG_CONTACT_LOOKUP_INTERVAL` seconds since
  the last lookup that changed a contact. 429 `flood_wait`: Telegram refused
  the removal; nothing changed. Both with `Retry-After`; neither is retried.
- 502 `telegram_error`: another Telegram error before anything changed.
- 503 `unavailable`: another lookup is running, or TDLib is not ready.
  503 `restore_pending`: the contact (or one from an earlier lookup) is
  removed and not added back yet; the daemon keeps it in the journal and
  adds it back later. `Retry-After` at least 60.
- 504 `timeout`: the contact is added back, but TDLib did not report the
  own name in time; nothing was stored.
- 500 `internal`: the contact is added back, but storing the name failed.

### GET /v1/events

What changes in the archive, as a stream of small events, for the web's
live updates ([design/web-live.md](design/web-live.md) sections 3 and 4).
An event says what changed, never the new content: the web refetches
what it names through its usual paths, which check the viewer's access.

    GET /v1/events?since=1794945257988294

The answer is Server-Sent Events (`text/event-stream`, chunked) for as
long as the connection stays open. Each event is one frame, an `id`
line and a `data` line holding the event as JSON, then an empty line:

    id: 1794945257988295
    data: {"seq":1794945257988295,"kind":"message.new","chat_id":-1001234567890,"message_id":42}

- `seq`, also the frame's `id`, grows by one with each event. It starts
  from the daemon's start time in microseconds, so it keeps growing
  across restarts and stays an exact JSON number (below 2^53). Compare
  seqs; do not compute with them.
- `since`: the last seq the client has seen; without it the stream
  starts from now. Anything but decimal digits is a 400.
- The first frame says where the stream starts:
  - `{"seq": N, "kind": "hello"}`: the events after N follow, N being
    `since` or, without it, the newest seq.
  - `{"seq": N, "kind": "resync"}`: `since` cannot be resumed, because
    it is older than the recent events the daemon keeps, from before
    its last start, or newer than any event. What was missed cannot be
    sent: the client refetches what it shows, and the events after N
    follow.
- A `resync` can also come later in the stream, when the client reads
  so slowly that the events after the last one it got are no longer
  kept. The stream then goes on from the newest event.
- To resume after a disconnect, reconnect with `since` set to the seq
  of the last frame received, `hello` and `resync` included. Missed
  events are sent from a ring of the 16384 most recent ones, so a short
  disconnect loses nothing and a long one ends in a `resync`.
- After 15 seconds without an event the daemon sends the comment line
  `: keepalive` (SSE parsers skip it), so a dead connection is noticed.
- The daemon closes the stream when the client sends anything after
  its request (or closes its side), when a write stalls for 30
  seconds, and at shutdown, after a last empty chunk.
- At most 16 subscribers at once; more are answered 503 `unavailable`
  with `Retry-After`. The web is meant to hold one subscription and
  fan it out to its own clients.

The events (ids as stored: `chat_id` is TDLib's chat id, negative for
groups and the other user's id for private chats; `message_id` is the
`message_id` column of the message tables; `file_id` is
`telegram_files.id`):

| `kind` | Fields | Published when |
|---|---|---|
| `message.new` | `chat_id`, `message_id` | A message row was inserted: a live message, or an older one the backfill or a reply lookup brought in. |
| `message.edited` | `chat_id`, `message_id` | A copy with a later edit date replaced the content, a content version was recorded, or the message's link preview or reply link was resolved. |
| `message.deleted` | `chat_id`, `message_id` | The message was found deleted (once; the row stays). |
| `user.updated` | `user_id`, `fields`? | A user's row, full info, photo or own name was written. |
| `group.updated` | `chat_id`, `fields`? | A group's row, administrators or photo was written. |
| `file.stored` | `file_id`, `chat_id`?, `message_id`? | A message's media or link-preview image was linked to its file row (with `chat_id` and `message_id`), or a user's or group's photo was stored (`file_id` alone). |
| `stats.changed` | `chat_ids`, `user_ids`, `more`? | Counters moved; at most one per second. |
| `bot.updated` | `bot_id` | A bot of either kind (`telegram_bots`, `discord_bots`) was added, changed (name, enabled, status, identity) or removed. |

- Each event is published after its row is committed, by the worker
  that wrote it, so a refetch finds the change. A message upsert that
  changes nothing visible, such as a copy seen again, publishes
  nothing; the other writes publish whether or not they changed the
  row, which costs a client a refetch at most.
- `fields`, when present, names the parts the write may have changed;
  without it anything may have. Users: `name` (the own name, from
  "Look up real name"), `photo`, and `bio`, `birthday`, `personal_chat`
  together (the full info). Groups: `photo`, `admins`.
- `file.stored` does not mean the bytes are on disk: media from a chat
  the account is not in, or over the size cap, is linked to a row
  recorded without them; `GET /v1/files/{id}` tells. A fetch through
  `POST /v1/files/{id}/fetch` publishes nothing; the web has the answer.
- `stats.changed` gathers what moved since the last one: `chat_ids`,
  the chats (groups and private chats) that got new messages, and
  `user_ids`, the senders whose message counts moved. Each list holds
  at most 1000 ids; past that it is cut and `"more": true` says to treat
  every counter as changed.
- `bot.updated` is found by reading the bots every 5 seconds, since
  discordd (the status) and the web (the rest) write them without the
  daemon: one event per bot that differs from the last read, at most
  5 seconds late. The ids of the two tables never meet (migration
  000040), so `bot_id` names one bot. Nothing of the token is read.
- New kinds may be added; a client skips kinds it does not know.

Errors: 400 if `since` is not decimal digits, 503 with too many
subscribers or during shutdown.

## Adding an endpoint

The server core (`src/tgloggerd/InternalApi.cpp`) has no operations of
its own. The part of the daemon that owns an operation registers it in
`TgLoggerd::start()` before the API starts, as
`registerFileApi()` (`src/tgloggerd/FileApi.cpp`) does:

    api.route("GET", "/v1/files/{id}", [](const InternalApi::Request &req) {
            uint64_t id = parseFileId(req.param("id"));
            ...
            InternalApi::Response r;
            r.body = { ... };
            return r;
    });

- A pattern's segments are literal or `{name}`; literal segments win
  over captures. The capture is `req.param("name")`, the parsed body
  `req.body` (null when the request has none).
- Handlers run on the handler threads, never on the TDLib loop thread,
  so they may block on the database or on `TDLib::request`.
- To answer an error, throw `InternalApi::Error` (`badRequest`,
  `notFound`, `gone`, `unavailable`, `timeout`, or any status and code
  from the table above). A `nlohmann::json` type error while reading
  the body becomes a 400; any other exception a 500, logged.
- Document the endpoint in this file in the same change.

A long-lived answer, such as `GET /v1/events`, is a stream route instead:

    api.streamRoute("/v1/events", "text/event-stream",
            [](const InternalApi::Request &req, InternalApi::Wake wake) {
                    ...
                    return std::make_unique<MyStream>(...);
            });

- The opener and the stream's `pull()` run on the I/O thread and must
  not block; a stream never holds a handler thread. The opener may throw
  `InternalApi::Error` to answer an error instead.
- The stream calls `wake()` (from any thread, cheaply) when it has
  something new; the server then calls `pull()`, writes what it returns
  and calls it again once the write is done. A peer that reads slowly
  therefore only delays its own pulls, and the stream decides what to do
  about a backlog.
- After 15 seconds without data the server writes the stream's
  `keepalive()`. The answer is chunked on HTTP/1.1 and delimited by the
  close on HTTP/1.0; the connection closes when the stream ends, when
  the peer closes or sends anything, when a write stalls for 30 seconds,
  or at shutdown.
