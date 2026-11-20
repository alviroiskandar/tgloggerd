# chat-live-any-window

Send live chat events to every chat window: windows scrolled away from
the newest messages and public viewers of exposed groups, so the
results of a recheck show without a reload there too.

## Why

Rechecks fill missing messages and flag deletions in the stretch a
viewer is reading, but only a signed-in viewer at the newest messages
receives the live events that show them. Everyone else sees the
result only after a reload.

## Scope

- First a review, written as a decision file: who may open the event
  stream (anonymous viewers of exposed groups included or not), which
  topics each may subscribe to, what an event reveals beyond the page
  the viewer could load, connection and rate limits for anonymous
  streams, and the load on the Go service.
- Then Go (webapi) and React, as the review decides.

## Depends on

Nothing.

## Done when

- The review is decided, and windows and viewers it allows receive
  the events of the stretch they show.
