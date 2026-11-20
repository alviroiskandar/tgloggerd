# Proposal: live chat events for every chat window

Status: proposal, for the project owner to decide. It is the review
the item `chat-live-any-window` (docs/todo/chat-live-any-window.md)
asks for first; once decided, the answers go into a decision file
under design/decisions/ and the Go and React work follows.

## 1. The question

A recheck (webapi/internal/server/recheck.go) fills missing messages
and flags deletions in the stretch a viewer is reading, and its
results arrive as the usual live events. Two kinds of chat window do
not receive them today:

- (a) a signed-in window that is not at the newest messages (scrolled
  back far enough that the window has newer pages, or opened at a
  message or a date);
- (b) a signed-out visitor reading the public chat of an exposed group
  (a group in `telegram_public_groups`).

A third case turned up during the review and belongs here as well:

- (c) a signed-in viewer reading the public chat of an exposed group
  its scope does not see (a member without the grant, an admin the
  group is hidden from). The chat answers (logdb.OpenChat lets an
  exposed group through for any scope), the client subscribes to
  `chat:<id>`, and the server drops every event for it
  (eventFor -> scope.SeesChat is false). The page says "New messages
  appear as they are logged" and the stream is up, so it does not
  poll either: it never updates.

## 2. How the stream works today

- The daemon publishes one stream, GET /v1/events
  (docs/internal-api.md): at most 16 subscribers, a ring of the 16384
  most recent events, a keepalive every 15 s.
- The Go service holds one subscription to it (live.DaemonSource,
  webapi/internal/live/source.go) feeding one live.Hub
  (webapi/internal/live/hub.go): a ring of WEBAPI_LIVE_HISTORY events
  for resuming, a bounded buffer of WEBAPI_LIVE_BUFFER events per
  subscriber (one that falls behind gets a resync instead of more
  memory), at most WEBAPI_LIVE_MAX_SUBSCRIBERS subscriptions. Every
  event is matched against every subscriber under the hub's one lock
  (Hub.publish).
- GET /api/events (webapi/internal/server/events.go) is behind
  RequireSession. It parses the topics (parseTopics, at most 2000),
  takes the request scope once, and matches each event first by topic
  (topics.wants) and then by scope (eventFor, filterEvent). For a
  member it reads, once at open, which of its user topics it may see
  (liveUsers). Every WEBAPI_LIVE_RECHECK it verifies the session again
  and ends the stream with a `session` event (`revoked`,
  `scope_changed`) when it changed (liveRecheck).
- The search engine's two indexers (searchengine.HubSource) are
  subscribers of the same hub and count against the same cap.
- The React chat view (web/app/src/routes/chat.tsx) subscribes to
  `chat:<id>` only while `following = live && !newer && !publicView`:
  the Live toggle is on, the window holds the newest messages, and the
  viewer is signed in. Its handler already knows how to place a
  message filled into the window (fillGaps) and how to refresh an
  edited, deleted or newly stored message it holds (replaceMessage);
  everything else on message.new fetches the tail (fetchNewer).
  Without the stream it polls the tail every second, but only while
  following.

## 3. What an event reveals

Events are invalidations (web-live.md section 3): a kind and ids,
never content. The content is refetched through
GET /api/chats/{scope}/{id}/messages, which applies the viewer's
access. So the question per field is whether the ids, the timing or
the existence of the event tell a viewer more than the window it may
load.

The window a viewer may load of a chat it may open shows every logged
message of that chat, deleted ones too (struck through, with
`deleted_at` and `deleted_on_recheck`), with their edit history and
their senders, for signed-in and signed-out viewers alike
(server/chats.go, view.message and view.edits). There is no hidden
user inside a chat: a sender shows as the window shows it.

Per kind, for a viewer who may open chat C (case a, b or c):

- `message.new`, `message.edited`, `message.deleted` of C:
  `chat_id`, `message_id`, `seq`. The message and its new state are
  in the window the viewer may load; the event adds only the moment
  it was logged, edited or found deleted, which the window shows as
  `date`, `edited_at` and `deleted_at` anyway. Events of other chats
  never reach the viewer: filterEvent keys on `chat_id` alone, so an
  edit of a message outside the exposure cannot pass a filter that
  admits only C. Nothing more is revealed.
- `file.stored` of a message of C: also `file_id`, the row id in
  `telegram_files`. The window never shows it (files are linked by
  encrypted /files/<token> URLs, server/media.go fileLink). It is a
  sequential id over the whole archive, so a signed-out visitor
  watching it learns how many files the archive stores across every
  chat, hidden ones included. Small, but more than the page. Remedy:
  drop `file_id` from events sent to a signed-out stream; the chat
  view does not use it (it refetches by `message_id`).
- `seq` (every frame's `id`, and the `hello` and `resync` frames):
  one counter over all events of the archive, starting from the
  daemon's start time in microseconds. A viewer who reconnects now
  and then, or just watches the gaps between the seqs it receives,
  learns the event rate of the whole archive (private chats and
  hidden groups included) and when the daemon last restarted.
  Signed-in members can do this today; for signed-out visitors it is
  new and open to anyone. Remedy, if wanted: send signed-out streams
  no `id` and no seq (section 8.2), so they cannot resume; a
  reconnect is then a `resync`, which costs one tail refetch.
- `group.updated` of C: `fields` may name `admins`. The public view
  of an exposed group shows none of its admins (GroupPublicProps in
  openapi.yaml), so a signed-out visitor would learn when the admin
  list changed. Remedy: no `group:<id>` topic for signed-out streams
  (the chat header's title and photo then update on reload), or pass
  only events whose `fields` are present and are all `photo`.
- `user.updated`, `stats.changed`, `bot.updated`: a signed-out viewer
  sees no user in full, no counters beyond the header's, no bots;
  none of these topics is offered to it (section 4).
- `resync` and `session`: carry no ids (a `resync` carries a seq;
  see above).

Case (a) changes nothing on the server: a signed-in viewer that sees
C already receives C's events wherever its window lies; only the
client chooses not to subscribe. An event about a message outside the
window tells it only what scrolling would.

Case (c) is the same as (b) for that one chat: the viewer may read
the exposed chat as anyone may, so it may hear of it as anyone may.
It must not widen anything else: `messages`, `stats` and user topics
stay as its scope decides.

## 4. Topics per viewer

- Signed-in, as today, plus: a `chat:<id>` topic of an exposed group
  the scope does not see passes the chat events of that group (case
  c). `messages` stays limited to the chats the scope sees, so the
  message search lists of a member do not fill with exposed groups.
- Signed-out: only `chat:<id>` of a group that is exposed, and at
  most a few of them per stream (the client opens one stream per tab
  and a chat page needs one; 4 leaves room for a quick navigation
  without letting a stream follow the whole public archive). Every
  other topic is refused. A refusal answers 401 `unauthenticated`
  whether the group exists or not, as getChatMessages answers a
  visitor, so a visitor cannot probe which group ids exist or which
  are exposed but unknown to it.
- Exposure is checked against a snapshot of `telegram_public_groups`
  held in memory and refreshed by one goroutine every few seconds
  (logdb.AllExposedGroupIDs), read through an atomic pointer, so the
  hub's match stays a memory lookup. A group withdrawn from exposure
  stops passing events at the next refresh, and the stream's periodic
  check ends it with `session` `scope_changed`; never `revoked`, which
  sends the client to the sign-in page.

## 5. Connection caps

Today one cap, WEBAPI_LIVE_MAX_SUBSCRIBERS, covers browsers and the
search indexers together, and only signed-in viewers can take a slot.
Signed-out streams can be opened by anyone in any number, including
by a third-party page that makes its visitors' browsers open
EventSources to this origin (they cannot read the answer without CORS
headers, and none are sent, but the connection is still held). So:

- Separate budgets: a total cap for signed-out streams, below the
  hub's cap, so visitors can never take the slots of signed-in
  viewers; signed-in streams keep the remaining slots.
- Internal subscribers (the indexers) must not count against either.
  Today a burst of browser streams after a daemon blip can fill the
  hub, and an indexer reconnecting then is refused with ErrFull and
  falls behind. Give live.Hub a way to subscribe outside the cap
  (for example Hub.SubscribeInternal, or a Reserved count in Options)
  and use it from searchengine.HubSource. This is worth doing with or
  without signed-out streams.
- Per address: at most a few open signed-out streams per client
  address (clientIP, which honours WEBAPI_TRUSTED_PROXIES), keyed by
  the IPv4 address or the IPv6 /64, since one IPv6 host holds a whole
  prefix. Keep the number above one: several tabs, and a shared
  address (an office, carrier NAT) are normal. Also limit how often
  one address may open a stream (a small token bucket), because every
  open costs a Subscribe and a replay or resync, and a client in a
  reconnect loop would otherwise churn.
- Smaller per-stream buffer for signed-out streams: one chat's events
  fit in far less than WEBAPI_LIVE_BUFFER, and each subscriber
  allocates its buffer at open. A visitor that falls behind gets a
  resync, as now.
- When a signed-out stream is refused (a cap reached, or live off), it
  answers 503 with a Retry-After, and the client must not fall back
  to polling the tail every second (section 7).

The numbers are settings with defaults in config.go
(WEBAPI_LIVE_PUBLIC_MAX, WEBAPI_LIVE_PUBLIC_PER_ADDR,
WEBAPI_LIVE_PUBLIC_OPENS_PER_MINUTE, or similar names), documented in
.env.example and webapi/README; a deployment sizes them.

## 6. Rate and fan-out cost

- Fan-out in the hub: every daemon event is matched against every
  subscriber under one lock. A signed-out match is a topic lookup
  plus an exposure lookup, both in memory, so the cost per event is
  linear in the number of subscribers and small while the caps stay
  in the hundreds. If the signed-out cap is ever raised into the
  thousands, index the subscribers by chat id (a map from chat id to
  subscribers, plus a list for the ones with broad topics), so an
  event touches only its chat's subscribers and the lock is held
  briefly.
- Refetches are the real cost. Every delivered event makes a client
  refetch: at the tail one window after its last message, inside the
  window one small window around the message. The client throttles
  each key to one refetch per 250 ms (web-live-client.md section 5),
  so N visitors at the tail of a chat that logs r messages a second
  send about N * min(r, 4) window requests a second, each several
  queries (OpenChat, the window, its kinds, edits, link previews,
  senders). A busy exposed group with many visitors turns into a
  steady query load on the read pool, which the per-address caps do
  not bound (they bound streams, not their events).
- Every one of those visitors asks for the same answer: a signed-out
  window depends only on the chat, the anchor, the limit and the
  topic (viewFor(Anonymous, false)), and visitors at the same tail
  hold the same last message id. A shared, short-lived answer cache
  for signed-out windows (a second or two, with one fetch in flight
  per key, in the manner of logdb.CountCache) collapses N
  refetches into one read per chat per interval. The file URLs in the
  answer are tokens that do not depend on the request, so a cached
  answer stays valid for every visitor.
- For signed-out clients a longer per-key refetch interval (one to
  two seconds instead of 250 ms) further bounds the rate at no real
  cost to a reader.
- Rechecks: every window request asks the rechecker for its stretch
  (rechecks.window). A tail refetch names the stretch after its last
  message id, a new key for every new message, so each live tail
  refetch asks for a recheck of an "older" stretch. Signed-out asks
  draw from the public bucket (an older stretch only above its
  reserve), so the bucket holds, but a busy public chat drains it for
  real page opens. Signed-in following viewers do the same today
  without a bucket, asking the daemon once per new message batch
  (section 10). Remedy: when a window with an `after` anchor reaches
  the newest message (Window.HasNewer is false), name the newest
  stretch (around 0, at most once a minute per chat) instead of the
  anchor.
- The daemon is not affected: the Go service keeps its one
  subscription however many browsers listen.

## 7. Reconnect behaviour

- The browser's EventSource reconnects on its own after the server's
  `retry:` (3 s today) and sends Last-Event-ID; the hub replays from
  its ring or answers resync. A refusal (503) closes the EventSource
  for good; the manager (web/app/src/live/manager.ts) then retries
  with back-off from 5 s doubling to 60 s, and after three failures
  in 30 s it polls.
- When the daemon's stream is lost for longer than the hub's grace
  (5 s), the hub ends every stream at once, and every browser
  reconnects after the same 3 s and refetches everything on the
  resync. With signed-out streams that herd grows. Remedies: a
  randomised `retry:` per stream (for example 3 to 10 s, longer for
  signed-out streams) and jitter in the manager's back-off.
- The manager's polling fallback polls the tail every second. For a
  signed-out viewer that is worse than the stream it replaces: a
  visitor refused for the cap would poll at one request a second. A
  signed-out chat should not poll the tail at that rate: either not at
  all (the header says live updates are unavailable) or every 30 s
  as other pages do. Signed-out windows never polled before, so no
  existing behaviour is lost either way.
- A `session` event on a signed-out stream is only `scope_changed`
  (the group was withdrawn); the client refetches and the chat
  answers sign-in, as it would on a reload.

## 8. Recommended design

### 8.1 Case (a): signed-in windows not at the newest

Client only; no server change, no security change.

- web/app/src/routes/chat.tsx: subscribe to `chat:<id>` whenever the
  Live toggle is on (`live`), not only when `following`. In the
  handler, when the window has newer pages (`newer`):
  - `message.new` after the window's last message: do nothing (the
    "jump to latest" control already says newer messages exist);
  - `message.new` inside the window: fillGaps, as now;
  - `message.edited`, `message.deleted`, `file.stored` of a message in
    the window: replaceMessage, as now;
  - `resync`: refetch the window from its anchor, not fetchNewer.
- Polling stays tied to `following`: a scrolled-back window does not
  poll the tail.
- Tests: the chat view's handler with a window that has newer pages
  (a filled message is placed, an edit replaces, a new message after
  the window fetches nothing).

### 8.2 Case (b): signed-out viewers of exposed groups

Server (webapi):

- server/server.go routes(): GET /api/events no longer behind
  RequireSession; the handler takes an optional session
  (optionalSession, as getChatMessages does) and splits into the
  signed-in path (as today) and a signed-out path.
- server/events.go:
  - a signed-out topic check: only `chat:<id>` with a negative id in
    the exposure snapshot, at most a small number per stream, else
    401 `unauthenticated` (section 4);
  - the signed-out match: topic, then exposure snapshot; drop
    `file_id`; no `group:` topic;
  - per-address and total signed-out caps and the open-rate bucket,
    checked before Subscribe, refused with 503 and Retry-After
    (section 5);
  - the periodic check for a signed-out stream re-reads exposure from
    the snapshot instead of the session, and ends the stream with
    `scope_changed`;
  - a longer, randomised `retry:` for signed-out streams;
  - if decided (section 9, question 2): no `id:` lines and seq 0 on
    signed-out streams; lastSeen ignored for them, so every open is a
    `resync`.
- live/hub.go: a per-subscription buffer size, and a subscription
  path outside the cap for internal subscribers; searchengine/source.go
  uses it.
- A small exposure snapshot (new file, e.g. server/exposure.go):
  loaded at start, refreshed on a ticker, swapped atomically; the MCP
  exposure handlers (POST and DELETE /api/mcp/groups) also refresh it
  at once after a change.
- server/chats.go getChatMessages: the shared answer cache for
  signed-out windows (section 6), and the recheck key fix for tail
  windows (rechecks.window).
- config/config.go: the new WEBAPI_LIVE_PUBLIC_* settings, in
  .env.example and webapi/README.
- openapi.yaml and web/docs: GET /api/events without a session, the
  signed-out topics and refusals, the missing `file_id` and seq.

Client (web/app):

- web/app/src/routes/chat.tsx: subscribe for a public view too
  (`publicView` no longer stops it), with the handler of 8.1.
- web/app/src/live/manager.ts: no one-second tail polling for a
  signed-out chat (section 7), back-off jitter; a stream without ids
  needs no Last-Event-ID handling (the browser simply sends none).
- web/app/src/live/PageLive.tsx: a signed-out page never acts on
  `revoked`.

Tests:

- Go: a visitor's stream gets the exposed chat's events and nothing
  else; a non-exposed or unknown chat id answers 401 alike; every
  other topic is refused; `file_id` is dropped; a withdrawn group
  stops events and ends the stream with `scope_changed`; per-address
  and total caps refuse with 503; signed-in streams keep their slots
  when the visitors' cap is full; an indexer subscribes with the hub
  full; the cached signed-out window answers identical bytes and
  reads once per interval.
- Client: the public chat subscribes; a refused visitor does not poll
  every second.
- e2e with the stand-in: a message filled into an exposed group's
  public chat appears without a reload.

### 8.3 Case (c): exposed groups outside a signed-in scope

- server/events.go eventFor: for the chat kinds, pass an event when
  `sc.SeesChat(id)` or when `id` is in the stream's `chat:` topics
  and in the exposure snapshot. `messages`, `stats`, user and group
  topics are not widened.
- Test: a member without the grant gets the exposed chat's events
  through `chat:<id>` and not through `messages`.

## 9. Decisions for the project owner

1. Signed-out streams at all? The alternative is (a) and (c) only,
   and public chats keep updating on reload. Recommended: yes, with
   the caps, the answer cache and the topic limits above.
2. Seq on signed-out streams: hide it (no resume; every reconnect is
   a resync and one tail refetch), or keep it and accept that anyone
   can read the archive's overall event rate and the daemon's start
   time. Recommended: hide it. Should signed-in members get the same
   treatment? Recommended: no for now; they are known accounts.
3. `group:<id>` for signed-out viewers: none (recommended), or
   photo-only events.
4. Polling for a refused or disconnected visitor: none, or every
   30 s. Recommended: every 30 s while the tab is visible, with the
   header saying live updates are unavailable.
5. The cap defaults: total signed-out streams, streams per address,
   opens per address per minute, signed-out buffer size, and the
   refetch interval of signed-out clients. The proposal names the
   mechanism; the defaults are a judgement of expected traffic.
6. The answer cache's lifetime: how stale (one or two seconds) a
   visitor's view may be in exchange for one read per chat per
   interval.
7. Whether case (c) widens only through an explicit `chat:<id>`
   topic (recommended) or also through `messages` for exposed groups.

## 10. Found outside the item

These exist today, independently of this item:

- Case (c) above: signed-in viewers of an exposed group outside their
  scope get a chat that claims to be live and never updates.
- Live tail refetches of signed-in viewers ask the daemon for a
  recheck after every new message batch (a new `around` key each
  time), spending the Telegram budget the rechecks share with the
  media relink and the repair. The key fix of section 6 covers it.
- The search indexers share the browsers' subscriber cap and can be
  locked out by a burst of browser streams (section 5).
- GET /api/events sets no write deadline: a client that stops reading
  holds its goroutine until the TCP connection or nginx's send
  timeout gives up. Behind nginx this is bounded; set a per-write
  deadline through the ResponseController all the same, so a stream
  reached without nginx is bounded too.
- The manager's reconnect and back-off have no jitter (section 7).
