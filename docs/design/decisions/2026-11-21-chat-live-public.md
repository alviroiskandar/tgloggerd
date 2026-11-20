# Decision log: 2026-11-21, Live chat events for signed-out viewers

Decided by the project owner on 2026-11-21, for the
chat-live-any-window item, on the questions of
[the proposal](../proposals/chat-live-any-window.md) (section 9),
taking its recommendations:

1. Signed-out viewers of exposed groups get a live stream, as section
   8.2 of the proposal designs it: GET /api/events with an optional
   session; for a signed-out stream only `chat:<id>` topics of
   exposed groups, at most 4 per stream, anything else refused alike
   (401 `unauthenticated`, whether the chat exists or not); exposure
   read from a snapshot refreshed on a ticker and at once when MCP
   exposure changes; a withdrawn group ends the stream with
   `scope_changed`, never `revoked`; `file_id` left out of their
   events.
2. Signed-out streams carry no sequence numbers (no `id:` lines, seq
   0, Last-Event-ID ignored), so each reconnect is a resync and one
   refetch of the window. Signed-in streams keep theirs.
3. No `group:<id>` topic for signed-out viewers.
4. A refused or disconnected visitor's chat refetches every 30 s
   while the tab is visible, and the header says live updates are
   unavailable; never the one-second tail polling.
5. Defaults, each a setting (WEBAPI_LIVE_PUBLIC_*): at most 200
   signed-out streams in all, 4 per client address (an IPv4 address
   or an IPv6 /64, from the trusted-proxy client address), 10 opens
   per address per minute, refused with 503 and Retry-After;
   signed-out clients refetch a chat window at most every 5 s.
   Signed-in streams keep their slots when the visitors' cap is full.
6. Signed-out chat windows are answered from a shared cache that may
   be 2 s old, with one read in flight per window.
7. A signed-in viewer outside its access gets an exposed group's
   events only through an explicit `chat:<id>` topic, never through
   `messages` (as merged in the live stream fixes).

Reason: public readers of an exposed group otherwise see recheck
results and new messages only on reload; the limits keep an anonymous
stream to what the public page already shows and its cost to one read
per chat per interval.
