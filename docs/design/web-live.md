# Live updates

Status: built and in service since 2026-11-18. The daemon's stream
and the Go service's GET /api/events (webapi/README, "Live updates")
are routed through nginx (webapi/README, "Routing /api/events through
nginx").

## 1. Goal

Pages update as things happen: new, edited and deleted messages,
renamed users and groups, new photos and usernames, statistics and
counters (message counts, dashboard totals), files that become
available.

## 2. Shape

    tgloggerd --GET /v1/events (internal API, stream)--> Go web service
    Go web service --GET /api/events (Server-Sent Events)--> browsers

- One publisher, the daemon, so no broker: the Go service subscribes to
  the daemon's stream. A broker (for example Redis Pub/Sub) is worth
  adding only with several web instances and several publishers; the
  Go side keeps one subscriber interface so it can be swapped in.
- Server-Sent Events to the browser: updates flow one way, they pass
  through the existing nginx (buffering off for that path), and the
  browser reconnects by itself with `Last-Event-ID`.

## 3. Events are invalidations

An event says what changed, never the new content:

    {"seq": 1234, "kind": "message.new", "chat_id": -100..., "message_id": 42}
    {"seq": 1235, "kind": "user.updated", "user_id": 7}
    {"seq": 1236, "kind": "group.updated", "chat_id": -100...}
    {"seq": 1237, "kind": "stats.changed", "chat_ids": [...], "user_ids": [...]}

`file.stored` carries `{file_id, chat_id, message_id}` (the message
when the file is message media). `user.updated` and `group.updated`
may carry `fields: [...]` naming what changed, so a client can skip a
refetch.

Kinds: `message.new`, `message.edited`, `message.deleted`,
`user.updated` (name, username, photo, bio, contact name),
`group.updated` (title, photo, description, usernames, admins),
`file.stored`, `stats.changed`.

- The page refetches what an event names through the normal API, which
  enforces the viewer's access and renders as usual. Content can never
  leak through an event, and nothing is rendered twice.
- The Go service still drops events about chats and users a viewer
  cannot see (federations, hidden groups, member grants, public
  pages), so the stream does not reveal activity in hidden groups.
- Counters change with almost every message, so the daemon coalesces
  them: one `stats.changed` per second at most, listing the chats and
  users whose counters moved.
- The daemon emits after the row is committed, from the worker that
  writes it, so a refetch always finds the change.

## 4. To the browser

The client side is specified in [web-live-client.md](web-live-client.md)
(the React application's design). The server side it needs:

- Server-Sent Events framing: `id:` is the seq, `event:` the kind,
  `data:` the JSON; a `retry:` value; a comment line as heartbeat about
  every 15 seconds.
- Topics: `GET /api/events?topics=chat:<id>,user:<id>,group:<id>,messages,stats&last=<seq>`,
  filtered by topic and by access.
- A `session` event (`scope_changed`, `revoked`) before the server
  closes a stream whose access changed.
- A batch endpoint, `GET /api/entities?users=..&groups=..`, returning
  display fields and counters of what the viewer may see and silently
  dropping the rest.
- HTTP/2 between nginx and browsers, so one stream per tab does not
  exhaust HTTP/1.1's per-host connection limit.
- Signed-out visitors of an exposed group's public chat get a stream
  too, limited to a few `chat:<id>` topics of exposed groups, without
  seqs or file ids and within caps of their own
  ([decision](decisions/2026-11-21-chat-live-public.md); webapi/README,
  "Live updates").

## 5. Resuming

- The daemon keeps recent events in memory; the Go service reconnects
  with its last `seq` and gets what it missed. Further back, it tells
  its clients to resynchronise.
- A browser that reconnects too late refetches its page from its
  cursors, as the polling live mode does today.
