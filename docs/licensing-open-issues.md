<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org> -->

# Licensing: open issues

Written 2026-08-24, alongside the move to `GPL-2.0-or-later`. `LICENSE` at the
repository root states the terms; this file is the list of things that move did
**not** settle, so that none of it has to be rediscovered.

Each item says what it is, where the evidence is, and what closing it would
take. Nothing here blocks the current source-only distribution. Several items
become live the day a binary or a non-toolchain image is published, and one
becomes live the day someone flips a build flag.

Items are marked **[verified]** where the file was read in this tree, and
**[unverified]** where the claim is carried over from analysis and still wants
checking before anyone relies on it.

---

## 1. No OpenSSL version floor — actionable now

Three call sites take any OpenSSL:

- `CMakeLists.txt:92` — `find_package(OpenSSL REQUIRED)`
- `src/gwdiscord/CMakeLists.txt:42` — `find_package(OpenSSL REQUIRED)`
- `web/cmake/FindMySQL.cmake:20` — `find_package(OpenSSL QUIET)`

OpenSSL relicensed to Apache-2.0 at 3.0.0. Versions **1.1.1 and earlier** carry
the old dual OpenSSL/SSLeay licence, whose BSD-4-clause-style advertising
requirement is incompatible with GPL version 2 **and** version 3. So this is the
one OpenSSL problem `-or-later` cannot help with at all: there is no arm of the
licence under which that combination works.

Today's image is `ubuntu:24.04` with OpenSSL 3.0.13, so it does not bite. Nothing
enforces it. **[verified]**

Closing it: `find_package(OpenSSL 3.0 REQUIRED)` at each site, which turns a
silent licensing landmine into a build error. Three commits, because
`src/gwdiscord/` is committed separately.

## 2. MariaDB PARSEC plugin is GPL-2.0-only with no exception — latent, one flag away

`submodules/mariadb-connector-c/plugins/auth/parsec.c:4-6` reads "under the terms
of the GNU General Public License ... **version 2 of the License**" — no "or
later", and MariaDB plc grants no FOSS exception. GPLv2-only code cannot be
combined with GPLv3, so enabling it would make `tgloggerd_web` undistributable
under an elected version 3, with no cure available. **[verified]**

It is kept out today by `web/CMakeLists.txt:56` alone
(`CLIENT_PLUGIN_PARSEC OFF`), and the comment above that line gives a *build*
reason, not a licensing one — "unused here and their install rules break the
subproject build". Anyone re-enabling it to get MariaDB auth working would have
no way to know what they were stepping on. `CLIENT_PLUGIN_CLIENT_ED25519` at
`:55` is in the same position, and PARSEC depends on it.

Closing it: amend the comment at `web/CMakeLists.txt:53-56` to record the licence
reason, which is the part that must not be lost.

## 3. `ma_dtoa.c` is LGPL-2.0-only and statically linked — latent, low severity

`submodules/mariadb-connector-c/libmariadb/ma_dtoa.c:3-7` is the **old Library
GPL, version 2 only** ("GNU Library General Public License ... version 2 of the
License"). Its object code is genuinely in the shipped web binary —
`nm -C web/build/tgloggerd_web | grep -i dtoa` returns matches. **[verified]**

Not an incompatibility: LGPL-2.0 section 3 permits substituting the ordinary GNU
GPL of a later version, so it reaches GPLv3. The defect is that the LGPL-2.0 text
is not shipped anywhere in this tree, and the connector's own `COPYING.LIB` is
LGPL 2.1, a different document.

Closing it: name it in a third-party notices file when one is written (item 5).
Not this project's file to fix.

## 4. 97 first-party files carry no SPDX tag — hygiene, deliberately deferred

Every SQL migration (54), every HTML template (20), both `CMakeLists.txt`, the
`Dockerfile`, `docker-compose.yml`, `run.sh`, `web/cmake/FindMySQL.cmake`, the
top-level `README`, `web/README.web`, and `web/docs/*.md`. **[verified]** — the
count comes from testing every tracked non-submodule, non-vendored file for the
string.

`LICENSE` covers the whole work, so these are licensed; they are just not
labelled. Two reasons that still matters: the templates and migrations are
substantial first-party expression, and the project's stated theory is that the
per-file tag is what travels when a file is lifted out.

It was left out of the relicense deliberately. Flipping an existing tag and
adding a grant where none was recorded are different acts, and only the first was
what got acked.

Closing it: three mechanical commits — `-- SPDX-License-Identifier:` for SQL,
`<!-- ... -->` for HTML, `#` for the build and shell files.

## 5. Corresponding Source is not assembled anywhere — becomes live on first binary release

Two obligations converge the moment a built binary or a non-toolchain image is
conveyed, and neither has any machinery today:

- Under an elected GPLv3, section 6 requires Corresponding Source. The
  dependencies are **gitlink submodules**, so `git archive` of this repository
  contains none of their source.
- Oracle's Universal FOSS Exception — the thing that makes the MySQL connector
  combinable at all (see `LICENSE`) — is itself conditioned on the other side
  being "distributed with complete corresponding source". So the source offer is
  not merely a GPL obligation here; it is a precondition of the exception.

MariaDB Connector/C is also linked **statically** into `tgloggerd_web`
(`web/CMakeLists.txt`), which brings LGPL-2.1 section 6's relinking obligation
with it on binary conveyance. Inert under source-only distribution. **[verified]**

Closing it: before any release, a tarball recipe that vendors submodule source,
plus a third-party notices file. Explicitly *not* part of the relicense.

## 6. No OpenSSL linking exception — open decision

`-or-later` leaves the version 2 arm formally available but practically dead:
nobody can exercise it on a binary that links OpenSSL 3.x, because that hits the
same GPLv2 section 6 wall as before. A downstream that must stay on version 2 —
to combine with GPLv2-only code, which this tree already contains in the form of
Oracle's connector — has no path.

An explicit OpenSSL linking exception would restore it, and would also cover the
pre-3.0 case in item 1 that `-or-later` structurally cannot.

It needs unanimous consent from every copyright holder. There are exactly two
right now and both are reachable; that will never be truer than it is today.
Ammar's ack covered the relicense, not an added exception, so this is an open
decision rather than a pending task.

## 7. Vendored stopword data — open question, probably moot

`web/src/mcp/Stopwords.hpp` merges two external word lists:
stopwords-iso/stopwords-id (MIT, Copyright (c) 2016 Gene Diaz) and NLTK's English
stopword corpus (**NLTK is Apache-2.0** **[verified]** via nltk/nltk
`LICENSE.txt`). The file is now tagged `GPL-2.0-or-later AND MIT` with attribution
in its header.

The open part: the Apache-2.0 half is not named in the SPDX expression. Under
`-or-later` that combination is fine via the version 3 arm — it is the same
mechanism as OpenSSL — but the expression is arguably incomplete.

Whether any of it matters depends on whether a sorted list of common words
attracts copyright at all, which is genuinely arguable and not worth arguing when
attribution is three lines. Recorded rather than resolved.

## 8. MySQL connector bundles Apache-2.0 code we do not currently link — latent

`submodules/mysql-connector-cpp/cdk/extra/` contains protobuf, rapidjson, lz4,
zstd and zlib for the X DevAPI. We link `connector-jdbc` (classic protocol) only,
and `ldd` on the daemon shows no protobuf or abseil. **[verified]**

Switching to the X protocol connector would pull a second Apache-2.0 surface into
the link graph. Under `-or-later` that is survivable for the same reason OpenSSL
is, but it is worth knowing it is there before anyone treats the switch as purely
technical.

---

## Checked and clear — do not re-investigate

- **Consent.** The entire history has exactly two authors, Alviro Iskandar
  Setiawan and Ammar Faizi, with no `Co-authored-by` trailers anywhere. Both hold
  copyright and both agreed. **[verified]**
- **`tgloggerd:latest` conveys nothing.** `.dockerignore` is `*`, the `Dockerfile`
  has zero `COPY`/`ADD` lines, and `/workspace` in the image is empty — the source
  and build tree are bind-mounted at run time. It is a toolchain image and
  triggers no obligation under this licence. Earlier analysis claimed the images
  ship OpenSSL "alongside the binary"; there is no binary. **[verified]**
- **Drogon's `orm_lib/COPYING` is MIT**, not a second BSD licence, so the
  `LICENSE` entry naming Drogon as MIT is complete. **[verified]**
- **TDLib's `td/generate/tl-parser/` is third-party GPL** (Vitaly Valtman,
  Vkontakte Ltd) inside an otherwise BSL-1.0 submodule — but it is
  `GPL-2.0-or-**later**` and builds as a standalone code-generation tool, not
  linked into anything shipped. Harmless; noted only because "TDLib is BSL-1.0"
  is not the whole story. **[verified]**
- **GPLv2 section 3's system-library carve-out was never a rescue.** It limits
  what must be shipped *as source*; it is not a combination permission and could
  never have cured a section 6 further-restrictions problem. GPLv3 section 1 is
  the same shape. Citing it as a failed rescue implies a rescue that does not
  exist.
- **The relicense remedies nothing retroactively, and did not need to.** GPLv2
  section 6 binds licensees; a copyright owner conveying their own work needs no
  licence from themselves. Copies already published under `GPL-2.0-only`,
  including `upstream/master` and the existing release tags, keep the terms they
  were given.
