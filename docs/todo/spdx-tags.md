# spdx-tags

Give every first-party file an SPDX licence tag: the 97 files that
carry none today (docs/licensing-open-issues.md section 4).

## Why

LICENSE covers the whole work, so these files are licensed, but they
are not labelled: every SQL migration, every HTML template, both
CMakeLists.txt, the Dockerfile, docker-compose.yml, run.sh,
web/cmake/FindMySQL.cmake, the top-level README, web/README.web and
web/docs/*.md. The templates and migrations are substantial
first-party expression, and the per-file tag is what travels with a
file lifted out of the tree.

## Scope

- Count the untagged first-party files again (tracked, outside
  submodules and vendored code); the number has changed since the
  licensing review.
- Add `SPDX-License-Identifier: GPL-2.0-or-later` in each file's own
  comment form: `--` for SQL, `<!-- -->` for HTML, `#` for build and
  shell files; Markdown as the project's other tagged Markdown does.
- Mechanical commits by file kind; the gwdiscord tree in its own
  commits if any of its files are among them.
- Close section 4 of docs/licensing-open-issues.md.

Deferred by the project owner (2026-11-21): adding a licence grant
where none was recorded is a different act from the relicensing that
was acked, so it waits for the owner to start it.

## Depends on

Nothing.

## Done when

- Every first-party file carries an SPDX tag, and the licensing open
  issue is closed.
