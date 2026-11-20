# Decision log: 2026-11-20, what an MCP token's scope is

Decided with the project owner on 2026-11-20, for the mcp-token-scope
item. It builds on
[2026-11-20-mcp-tokens-to-superadmin.md](2026-11-20-mcp-tokens-to-superadmin.md)
(who manages a token) and leaves the exposure list
(`telegram_public_groups`, migrations/000022) as the one gate of
`/mcp`.

## What a scope is

- A token's scope is a set of entries, each one of:
  - a federation (`federation`, its id; 0 is Default),
  - a group (`group`, its negative chat id),
  - a private chat (`private`, the peer user's id, which is the chat
    id of a one-to-one chat).
- A token with no entries reads what every token read before: every
  exposed group, and no private chat. That is every token that exists
  when this lands, so nothing changes for them.
- A token with entries reads only what they reach:
  - the exposed groups named by a `group` entry, and the exposed
    groups of each `federation` entry;
  - the private chats of its `private` entries.
  Federation and group entries only narrow: a group that is not
  exposed to MCP is never reached through them, whatever names it.
  Private chats are the one way past the exposure list, and only on a
  superadmin's token (below).
- Whether a token is narrowed is decided by its stored entries, not by
  the entries that still reach something. A token whose every entry
  has gone inactive reads nothing; it never falls back to "every
  exposed group". Nothing that removes a group, a federation or an
  owner's role can widen a token.

## How a scope follows changes

- A `federation` entry follows the federation's current groups, read
  on every request: a group added to the federation is reached from
  the next request on, a group taken out of it no longer is. Default
  follows its derived set, the groups in no other federation
  (web-accounts.md section 3).
- A deleted federation leaves its entry in place, inactive, reaching
  nothing. The entry has no foreign key on purpose: a cascading delete
  could empty the scope, and an empty scope means "every exposed
  group".
- A group withdrawn from MCP drops out of reach at once, through a
  `group` or a `federation` entry alike, and its entry stays, shown as
  inactive. Exposing the group again makes the entry active again.
- A `private` entry works only while the token's current owner is an
  active superadmin, or the token has no owner record (it counts as
  the superadmins'). When the token is transferred to an admin or a
  member, or its owner stops being a superadmin, the entry stays,
  inactive, and reaches nothing; transferring the token back to a
  superadmin makes it work again.

## Who sets a scope

- Whoever manages a token (the decision above) sets its scope, when
  minting it or later; the change is audited (`mcp_token_scope`, the
  token and its entries as written).
- Only a superadmin writes a `private` entry, and only on a token a
  superadmin owns or that has no owner record. Any other attempt is
  refused with 403 and a reason in the JSON, as the other admin
  refusals answer. An admin editing a token that still carries an
  inactive private entry from an earlier owner has to drop it to save
  the scope.
- A `federation` entry must name an existing federation, a `group`
  entry a group the viewer can see, a `private` entry a known user;
  anything else is 400. A group need not be exposed to be written: the
  entry is then inactive until it is, so a scope can be prepared
  before a group is exposed.
- A scope holds at most 200 entries; duplicates are kept once.

## What the tools do with a private chat

Chosen when implementing it, as the item left it open:

- A private chat is read by naming it: the message tools that take a
  `group_id` (search, recent messages, get messages, message history,
  popular words, senders, count user messages) accept the chat's id
  there when the token reaches it. The cross-chat forms (search and
  recent messages without a `group_id`) read only groups, as before:
  merging a second table into one ordered, cursored scan is a
  different query plan, and a private chat is something a caller
  asks for by name.
- `telegram_list_groups` adds `private_chats` (id and name) to its
  answer when the token reaches any, so a client can find them; a
  token without private chats gets the answer it got before, byte for
  byte.
- The group tools (`telegram_get_group`, `telegram_get_group_history`,
  `telegram_list_group_admins`) describe groups only; a private chat's
  id there is "no readable group", as any other unreachable id.
- The user tools are not narrowed by a scope: they describe accounts,
  not conversations, and were never gated (web/docs/mcp.md).
