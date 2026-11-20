# Live updates in the React application

Status: built in `web/app/` and in service since 2026-11-18, against
the Go service's event stream and entity lookup. The browser side
of [web-live.md](web-live.md): how the React application in `web/app/`
subscribes to `GET /api/events`, what each event refreshes, how
refreshes are throttled, what happens without the stream, and how a
signed-out visitor of a public chat follows it (section 7).

## 1. Principles

- An event never carries content; it marks cached data stale, and the
  data comes back through the normal API, under the viewer's access
  rules (web-live.md section 3).
- What the reader is looking at must not move under them. New rows do
  not reshuffle a list or jump a scrolled chat; they announce
  themselves and wait for a click. Values in place (names, photos,
  counters, an edited message's text) update silently.
- Work only for what is visible: a hidden tab, a page that does not
  show a thing, or a collapsed panel does not refetch.
- Without the stream, the page still updates, by polling, and says so.

## 2. Data layer

Server data stays where it is loaded: a page's data in the navigation
state (`src/nav.tsx`), a chat's window of messages in the chat view.
Every request goes through the generated client in
`web/app/src/api/`. Live updates refetch through the same calls:

| Data | Refetched by | Through |
|---|---|---|
| a page's props | `refresh()` in `src/nav.tsx`, throttled per page | `GET /api/page/{path}` |
| a chat's newer messages, or one message | the chat view, from its cursor | `GET /api/chats/{scope}/{id}/messages` |
| a user's or a group's display fields and counters | the entity cache (`src/live/entities.ts`) | `GET /api/entities` |

The entity cache holds the users and groups that were fetched again
because an event named them, at most 2000 of each, least recently used
first out. Lists, cells, chat bubbles and the chat header show the
cached value where it is newer than the page's data, and the page's
otherwise, so a renamed user is fixed in every list, bubble and header
by fetching one entity, not every page, and a list keeps its order.
Only ids on screen are fetched; ids the server leaves out (hidden or
unknown) are not asked for again until the next page load.

## 3. Subscription manager

`web/app/src/live/` holds one manager per browser tab.

- One `EventSource` per tab to `/api/events?topics=<list>`. Components
  declare what they show with a hook, `useLiveTopics(['chat:-1001234',
  'user:42'])`; the manager keeps the union of the mounted components'
  topics.
- Topics: `chat:<id>` (messages, edits, deletions and files of one
  chat), `user:<id>`, `group:<id>`, `messages` (every chat the viewer
  can see; message search lists), `stats` (dashboard and counters).
- When the topic set changes (navigation), the manager waits 300 ms for
  it to settle, then reconnects with the new set and `last=<seq>`, so
  nothing between the two connections is lost. Topics that stay are
  not dropped meanwhile: the old connection closes only when the new
  one is open.
- Events are dispatched by kind to the invalidation table (section 4);
  each handler also checks the event against the topics still mounted,
  because the stream may deliver a few events for a page just left.
- The manager exposes its state, `live`, `reconnecting`, `polling` or
  `paused` (the chat's Live toggle off), shown in the chat header and
  as a small indicator in the top bar.

## 4. What each event refreshes

| Event | Refreshes | How |
|---|---|---|
| `message.new` | open chat on `chat_id` | inside the window: put in its place by message id; at the live edge: fetch newer from the cursor and append; scrolled up: show "N new messages" below, nothing moves; a window that does not hold the newest (opened at a message or a date) leaves a message after it to its newer pages |
| | message search lists (`messages` topic) | mark page 1 stale and show "New messages, refresh"; never reorder rows on their own |
| `message.edited` | the message, wherever loaded | refetch that message in the open chat window, or the message page; the bubble updates in place with its edit marker |
| `message.deleted` | the message, wherever loaded | as edited; the bubble is struck through, not removed |
| `user.updated` | the user in the entity cache | one batched entity fetch; every name, photo and username shown for that user updates; on that user's own page, also its page data (bio, history) |
| `group.updated` | the group in the entity cache | as users; on the group's page, also its page data (description, admins, usernames) |
| `file.stored` | the bubble and file card of that file | refetch the message or the file row; a "Not stored" placeholder becomes the media |
| `stats.changed` | the dashboard; entities of the listed `chat_ids` and `user_ids` that are on screen | counters update in place; a list sorted by a counter keeps its order until refreshed |
| `resync` | every mounted query | refetch all, chats at the live edge from their cursors; a chat window that does not hold the newest is refetched in place, from its first message to its last |
| `session` | the whole app | `scope_changed`: refetch everything, a page now hidden becomes 404; `revoked`: go to the sign-in page |

## 5. Throttling

- Invalidations are collected for one animation frame and applied
  together, so a burst becomes one pass.
- Each key has a minimum interval between refetches: chat windows
  250 ms, entities 1 s, page data 2 s, dashboard statistics 5 s. An
  invalidation inside the interval schedules one trailing refetch; it
  never queues several.
- Entity refetches are batched: ids collected for 500 ms, then one
  request for all of them.
- At most four refetches run at once; the rest wait.
- In a hidden tab (`document.hidden`) nothing is refetched: keys are
  only marked stale, and every stale mounted query refetches once when
  the tab becomes visible.
- `stats.changed` arrives at most once a second already; the client
  adds no further delay beyond the per-key intervals above.

## 6. Without the stream

- The manager reconnects on its own (the browser's `EventSource` retry,
  using the server's `retry:` value). After three failures within 30 s,
  or when `/api/events` answers that live updates are off, it switches
  to polling and keeps retrying the stream with back-off (5 s doubling
  to 60 s), each wait jittered to between half its step and the whole
  step, so tabs cut off together do not all retry together.
- Polling: an open chat at its live edge asks for newer messages every
  second, as today's live mode does; other pages refetch their mounted
  queries every 30 s and on window focus. Hidden tabs do not poll.
- When the stream comes back, the manager refetches every mounted query
  once (as for `resync`), then stops polling.
- The chat's Live toggle (`paused`) stops both the stream subscription
  for that chat and polling; turning it on again refetches from the
  cursor.

## 7. Signed-out viewers

A visitor reading the public chat of an exposed group follows that
chat live too, as decided in
[the decision log](decisions/2026-11-21-chat-live-public.md). Its
stream differs from a signed-in one; the server side is in
`webapi/openapi.yaml` (GET /api/events).

- Which pages: only the public chat (route `group_chat`, signed out)
  opens a stream, with its own `chat:<id>` topic and nothing else.
  Every other signed-out page has none, and no signed-out page asks for
  `messages`, `user:`, `group:` or `stats`; the entity cache stays off.
- Reader: the stream is read with fetch (`src/live/fetchsource.ts`),
  not `EventSource`, which hides a refusal's status and headers: a
  visitor refused for the caps gets 503 with a Retry-After, and the
  reader keeps it for the next attempt. It never reconnects on its own
  and sends no Last-Event-ID.
- No seqs: the manager runs in its signed-out mode. Frames have no
  `id:` and events no seq, so it sends no `last=`, keeps no seq and
  drops no event as a repeat. There is no resuming: the server starts
  every open with a `resync`, so each reconnect is one refetch, and the
  manager makes up no resync of its own.
- Refetch rate: that first `resync` carries `refetch_interval`, the
  least number of seconds between two refetches of a chat window (5
  when absent). All of a visitor's refetches go under one key at that
  interval, so a burst of events is one refetch: the tail for a new
  message at the newest, the window in one pass for a message filled
  in, an edit, a deletion or a stored file; a message after a window
  with newer pages is left to those pages, and a `resync` refetches the
  tail at the newest and the window otherwise. Visitors' windows come
  from a cache the server shares between them, up to 2 s old.
- Without the stream: never the one-second tail polling. While the
  stream is refused or down, the chat refetches its window every 30 s
  while the tab is visible, not at all while hidden, and once when it
  becomes visible again. The footer then says "Live updates
  unavailable; checking for new messages every 30 seconds." in place
  of "New messages appear as they are logged.", and goes back once the
  stream is open.
- Reconnects: the back-off of section 6 with its jitter, and never
  before a refusal's Retry-After; the wait is then the Retry-After plus
  a jittered part of the first step (1.25 to 2.5 s).
- Session events: a visitor has no session to lose, so `revoked` is
  ignored. `scope_changed` (the group was withdrawn from exposure)
  reloads the page data, whose answer says whether the chat is still
  public.

## 8. Needs from the server

These would make the client simpler or correct; the first four matter
most.

1. **`file.stored` fields.** web-live.md lists the kind but not its
   fields. The client needs `file_id`, `chat_id` and `message_id` to
   find the bubble and the file card.
2. **SSE framing.** `id:` set to `seq` (so the browser's reconnect
   sends `Last-Event-ID`), `event:` set to the kind (handlers per
   kind), `data:` the JSON, a `retry:` value, and a comment line about
   every 15 s as a heartbeat so a dead connection behind nginx is
   noticed.
3. **Topics.** `GET /api/events?topics=chat:<id>,user:<id>,group:<id>,
   messages,stats&last=<seq>`, the server filtering by topic as well as
   by access. Without topics, every tab would receive every event the
   viewer may see and filter locally.
4. **A batch entity endpoint.** `GET /api/entities?users=1,2&groups=3`
   returning the display fields (name, username, photo, contact name,
   counters) for the ids the viewer may see, ignoring the others. It
   serves the entity refetches above and is an OpenAPI operation like
   the rest.
5. **A `session` event** when the viewer's access changes while
   connected: `scope_changed` (restrictions, grants or role edited) or
   `revoked` (disabled, signed out elsewhere, session ended), after
   which the server closes the stream.
6. **HTTP/2 to the browser** at nginx. Browsers allow six HTTP/1.1
   connections per host, and each tab holds one for the stream; with a
   few tabs open, page requests would queue. With HTTP/2 every stream
   shares one connection. If that is not possible, the client can elect
   one tab per browser (Web Locks) to hold the stream and relay events
   to the others over a BroadcastChannel, at some extra complexity.
7. Optional: `user.updated` and `group.updated` could carry
   `fields: ["name", "photo", ...]`, letting the client skip a refetch
   when nothing it shows changed (a bio change on a page that shows only
   names).

## 9. Tests

- The manager is tested against a fake event source: topic changes
  reconnect once with `last`, events for unmounted topics are ignored,
  bursts produce one refetch per key, hidden tabs defer, three failures
  switch to polling and recovery stops it.
- The invalidation table is tested per event kind against a query
  cache with known keys.
- An end-to-end test with the stand-in server checks that a renamed
  user changes in a list and a chat without a page reload, and that a
  list does not reorder until refreshed.
