# TODO

Open work for tgloggerd. Each item is one row in this file plus one
detail file under [todo/](todo/). The design an item implements is in
the files under [design/](design/), which the items point into
("storage-cache.md section 6"). An item changes the design file it
touches, and each decision it makes is a new file under
[design/decisions/](design/decisions/) and nothing else, rather than
text appended to a shared file, so that items worked on in parallel
never edit the same file for bookkeeping.

## Rules

### Identifiers

- Every item has a slug: lowercase letters, digits and hyphens
  (`[a-z0-9]+(-[a-z0-9]+)*`), unique across the list. Never a number,
  so removing an item never renumbers the others.
- The detail file is `docs/todo/<slug>.md`, named exactly after the
  slug, and its first line is `# <slug>`.
- Refer to items by slug in commit messages and discussion, for
  example "Implements `storage-janitor` from docs/TODO.md". The slug
  is also the name of the branch that works on the item.

### Rows

- One row per item, in exactly this form:

      - [`<slug>`](todo/<slug>.md): <short description>

- The description is one sentence. Everything else (why, scope, design
  notes, dependencies, when it is done) belongs in the detail file.
- Rows sit in the section of their area, in suggested working order,
  dependencies first. Items that go beyond what was asked for are
  marked "(optional)".

### Adding an item

1. Pick a slug that is not in use (`ls docs/todo/`).
2. Create `docs/todo/<slug>.md` from the template below.
3. Add the row to the right section.
4. Commit the row and the file together.

Work in progress often turns up more work: a bug nearby, a missing
test, a follow-up. Add it as an item rather than widening the item
being worked on.

### Removing an item

- When an item is done, delete its row and its detail file in the
  commit that completes the work, or in the last commit of the series.
- When only part of it is done, edit the detail file down to what is
  left and keep the row.
- When an item is dropped without being done, delete both and say why
  in the commit message.
- Before deleting, look for other items that name it under "Depends
  on" (`grep -rn '<slug>' docs/`) and update them.
- Git history keeps finished items; this file lists open work only.

### Checking the list

`scripts/check-todo.sh` checks that rows and detail files match one to
one, that slugs are unique, that each row's slug matches its link,
that each detail file starts with its own slug, that every item a file
lists under "Depends on" has a row above it, and that this file, the
detail files and the design are plain ASCII. It prints nothing when
all is well. Run it from the repository root before every commit that
touches the list.

### Detail file template

    # <slug>

    <One paragraph: what this item delivers.>

    ## Why

    <Why it matters; what is missing or breaks without it.>

    ## Scope

    <What is in, and what is explicitly left to other items.>

    ## Design notes

    <Decisions, constraints, pointers into the design
    ("storage-cache.md section N"), known pitfalls, anything already
    measured or ruled out.>

    ## Depends on

    <Slugs of items that must land first, or "Nothing.">

    ## Done when

    <Checkable conditions: commands that pass, behaviour observed.>

## Storage

The file store as a capped cache in front of Telegram:
[design/storage-cache.md](design/storage-cache.md).


## Logging

## Message types

Storing every kind of Telegram message faithfully:
[design/message-types.md](design/message-types.md).

## Web backend in Go

The web backend moves from Drogon to Go:
[design/web-go.md](design/web-go.md).

- [`mcp-token-scope`](todo/mcp-token-scope.md): Give each MCP token its own scope of federations, groups and (superadmin only) private chats.

## Web interface

Accounts, roles and federations:
[design/web-accounts.md](design/web-accounts.md).

- [`profile-photo-videos`](todo/profile-photo-videos.md): Store and show the video of an animated profile photo in the photo carousels.
- [`chat-live-any-window`](todo/chat-live-any-window.md): Send live chat events to scrolled-back windows and public viewers of exposed groups.
- [`video-thumbnails`](todo/video-thumbnails.md): Show a still thumbnail for every video before it is played, wherever the web shows one.
- [`drogon-retire`](todo/drogon-retire.md): Remove the Drogon web once nothing routes to it.

## Operations

