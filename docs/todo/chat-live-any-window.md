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

- Done: the review
  ([proposal](../design/proposals/chat-live-any-window.md)), its
  decision
  ([2026-11-21-chat-live-public.md](../design/decisions/2026-11-21-chat-live-public.md)),
  signed-in windows not at the newest (React) and the faults the
  review found (Go).
- Left: signed-out viewers of exposed groups, Go (webapi) and React,
  as the decision says.

## Depends on

Nothing.

## Done when

- The review is decided, and windows and viewers it allows receive
  the events of the stretch they show.
