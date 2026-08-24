<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org> -->

# Telegram → Discord: forwarded author names

When `tgloggerd` mirrors a Telegram message to a Discord channel (via a
`telegram_discord_webhooks` route; see
[platform-forwarding.md](../web/docs/platform-forwarding.md)), the Discord
message is posted through a webhook with the **author's name overridden** to
identify who sent it on Telegram and which message it was.

The override — the webhook `username` — has this shape:

```
First Last (cx:<user-id>:<message-id>)
```

- **`First Last`** — the sender's Telegram first and last name, space-joined.
  For a channel post or an anonymous admin (no user) it is the chat's title
  instead.
- **`cx`** — a fixed literal marking the identifier that follows.
- **`<user-id>`** — the sender's Telegram user id, compact-encoded (below). For
  a channel/anonymous sender the user id is `0`, which encodes as `AA`.
- **`<message-id>`** — the message's server id (the id `tgloggerd` stores; see
  [the message-id note](#note-which-message-id)), compact-encoded.

Example: a message from *Ammar Faizi* (user id `243692601`) as message
`4634266` is posted by an author named

```
Ammar Faizi (cx:DoZ0OQ:Rraa)
```

The point of the suffix is to make each forwarded message **traceable back to
the exact Telegram user and message** from the Discord side alone, while
staying short and readable. Both ids are fully reversible (below).

## The `cx` encoding

Each id is a 64-bit unsigned integer, encoded as:

1. its **minimal big-endian byte string** — the integer's bytes most-significant
   first, with leading zero bytes removed, so an id uses only as many bytes as
   it needs;
2. **standard base64** of those bytes (alphabet `A–Z a–z 0–9 + /`);
3. with the trailing `=` **padding removed**.

`0` is a special case: it has no non-zero bytes, so it is encoded as a single
zero byte, which is base64 `AA`, rather than the empty string.

### Why big-endian, and why this is already minimal

The number of bytes an id occupies is `floor(log256(id)) + 1` — the fewest that
can hold it. That count is **the same whether the bytes are read big-endian or
little-endian** (little-endian would drop the *trailing* zero bytes instead of
the leading ones, for an identical length), so the choice of endianness does not
change the size at all. Big-endian is used because it is the canonical network
byte order: the encoded form orders the same way the integers do, and decoding
is a plain shift-and-or with no byte reversal.

Sizes in practice: a Telegram user id (~34–37 bits today) is 5 bytes → **7
base64 characters**; a server message id (thousands to millions) is 3–4 bytes →
**4–6 characters**. The whole suffix is typically under 20 characters.

### Truncation

Discord caps a webhook username at **80 characters**. The `cx` identifier is
never shortened — it must decode — so if the whole label would exceed 80, the
**name** is truncated to fit (from the end, so the last name goes first),
UTF-8-safe (never splitting a multi-byte character). The identifier always
survives intact.

## Decoding an id back to an integer

Add back the base64 padding (to a multiple of 4), base64-decode, and read the
bytes big-endian. In Python:

```python
import base64
def cx_decode(tok: str) -> int:
    return int.from_bytes(base64.b64decode(tok + "=" * (-len(tok) % 4)), "big")

cx_decode("DoZ0OQ")   # 243692601   (the user id)
cx_decode("Rraa")     # 4634266     (the message id)
cx_decode("AA")       # 0           (channel/anonymous sender)
```

The reference implementation is `src/tgloggerd/CompactId.hpp`
(`compactid::encode` / `compactid::decode`), which round-trips every 64-bit
value.

## Note: which message id

`<message-id>` is the **server message id** — `tdlib_message_id >> 20` — which is
what `tgloggerd` stores in `telegram_group_messages.message_id` and exposes
everywhere else. It is not the raw TDLib message id. To look the message up in
the archive, use it directly as `message_id`.

## Scope

The stamp is applied only to the **author of the forwarded message**. A reply
preview embed shows the *replied* message's author and is left as a plain name —
it refers to a different message, and its own identifier is not carried here. An
edit or delete of a forwarded message changes only the Discord message's content
and leaves the original stamped author name in place.

# Reply previews

When a forwarded Telegram message is a reply, the replied message is shown above
it as a small Discord embed (a coloured bar, the replied author, and a snippet of
the text). The snippet is built to be a useful glance without ever overrunning
Discord's limits:

- **Up to 5 lines** of the replied message are shown. If it has more, the first
  five are kept and a literal **`[...]`** is appended so it is clear the message
  continues.
- The snippet is also bounded to stay under Discord's **4096-character** embed
  description cap (the code uses a 4000-character budget for margin). If the text
  — even within five lines, or as one very long line — would exceed that, it is
  truncated to fit and **`[...]`** is appended. The budget always reserves room
  for the `[...]`, so the marker itself is never cut off.
- Truncation is UTF-8-safe (never splits a multi-byte character) and
  escape-safe (never ends on a dangling markdown-escape backslash).

So a short reply shows in full with no marker; a long or many-line reply shows
its opening and ends with `[...]`. The snippet is plain escaped text — Telegram
formatting (bold, links, …) is not re-rendered in the preview, because the
replied message is read back from the archive, which stores no parsed
formatting for it.

## A more compact form, if it is ever wanted

The current form favours readability and eyeball-decodability: two separately
base64'd ids with a `:` between them. If maximum compactness mattered more than
that, the ids could be packed into **one** base64 blob — a small header byte
giving the length of the first id, then both ids' bytes concatenated and encoded
together. That drops the `:` separator and the rounding-up that each *separate*
base64 group pays at its end, saving roughly one to three characters. The cost is
that the two ids can no longer be read apart by eye, and decoding needs the
length header. base64url (`-` `_` instead of `+` `/`) is also available if the
standard `+`/`/` characters are ever undesirable in a display name; it is the
same length. Neither is used today because the readable form is already well
under Discord's limit.
