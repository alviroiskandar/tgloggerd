# Message types: storing every kind of Telegram message faithfully

Status: approved and built 2026-11-18: the daemon stores every kind
and the web shows them (web/docs/message-kinds.md). What is left is in
the "Message types" section of [../TODO.md](../TODO.md); the decisions
behind it are under [decisions/](decisions/) (files named
`2026-11-18-msg-*.md`).

## 1. Problem and goals

TDLib 1.8.70 (the tree's `submodules/td`) reports the content of a
message as one of 109 `MessageContent` classes. The daemon recognises
eight of them as media (`extract_message_content` and
`message_content_file` in `src/tgloggerd/TDLib.cpp`), describes about
thirty service messages in English (`describe_service_message`), and
stores everything else as `content_type = 'unknown'` with no text, no
file and no record of what it was. In the live archive about half a
percent of group messages are such rows; what they were cannot be
told from the database.

Even the recognised kinds lose detail: a pinned-message notice does not
say which message was pinned, a member removal does not say who was
removed, a sticker keeps its file but not its emoji, and a photo with a
spoiler keeps the photo but not the spoiler.

Goals:

1. Every message gets a row that says exactly which TDLib class it
   was, even for classes the daemon does not interpret.
2. Every class a person can see in a chat is stored with the fields a
   reader needs to reconstruct it: what it said, what it showed, who
   it referred to, and its files.
3. Structured detail is stored as data, not only as English text; the
   existing `text` description of service messages stays for
   compatibility and search.
4. Schema changes are additive (new columns, new tables, new ENUM
   values appended at the end), so the deployed web keeps working
   before it learns the new kinds.
5. Rows stored before the change are repaired where TDLib can still
   return the message.

Non-goals: secret chats (the daemon runs with `use_secret_chats_ =
false`), scheduled messages, and content only bots receive
(`updatePollAnswer`, passport data in full).

## 2. What is stored today

Per message, both `telegram_group_messages` and
`telegram_private_messages` keep: chat and message id (the server id,
see `to_server_msg_id`), sender, date, edit date, `content_type` (ENUM
`text, photo, video, document, audio, voice, sticker, animation,
service, unknown`), `text` (text or caption), `entities` (formatting
as JSON, UTF-16 offsets), `service_type` (an action id for service
messages), one `file_id`, `deleted_at`, `is_forwarded`,
`media_album_id`, and the reply target (`reply_to_chat_id`,
`reply_to_msg_id`, `reply_to_id`). Group rows also keep
`is_channel_post` and `author_signature`; private rows `is_outgoing`.

Side tables: `*_message_edits` (snapshot of content_type, text,
entities and file before each edit), `*_message_fwd_info` (forward
origin: user, hidden user, chat or channel, origin date and message),
`telegram_link_previews` (one card per text message: url, title,
description as plain text, type, flags, image file), and
`telegram_files` (one row per Telegram file, `file_type` ENUM without
a video note kind).

Ingest paths: `updateNewMessage` and `updateMessageSendSucceeded`
(`handle_new_message`), `updateMessageContent` (late link previews and
inline edit versions; polls, locations and expired media are skipped
as churn), `updateMessageEdited` (re-fetch with `getMessage`),
`updateDeleteMessages`, history backfill (`getChatHistory`), and the
reply resolver and relink re-fetch (`getMessage`). No other message
update is handled: not `updateMessageIsPinned`,
`updateMessageInteractionInfo`, `updateMessageReactions`, `updatePoll`,
`updateMessageFactCheck` or `updateForumTopic*`.

### 2.1 Expired media (fixed)

The upsert in `models/GroupMessage.cpp` (and its private twin) took the
content columns from any copy whose `edit_date` was not older than the
stored one. A self-destructing photo, video, video note or voice note
becomes `messageExpired*` once opened or timed out; when that message
was ingested again (reply resolution, backfill, relink) the stored
`photo` row was rewritten to `unknown` with no text. Since 2026-11-18
an expired copy of a stored message leaves the row alone (decision
[2026-11-18-msg-expired-media-keep](decisions/2026-11-18-msg-expired-media-keep.md));
rows overwritten before that cannot be told apart from other `unknown`
rows.

## 3. Content types and what is lost

"Stored" is one of: **fully** (nothing a reader needs is lost),
**partly**, **text only** (a service message whose detail survives
only in the English description), or **not** (stored as `unknown`).
"Planned kind" is the `content_type` after the change (section 4.1);
service messages keep `content_type = 'service'` and get the
`service_type` shown. An animated emoji is stored as text (decision
[2026-11-18-msg-animated-emoji-text](decisions/2026-11-18-msg-animated-emoji-text.md)).

### 3.1 Text and media

| TDLib class | Stored | What is lost today | Planned kind |
|---|---|---|---|
| messageText | partly | link preview options (disabled, forced url, size, position); entities of the preview description | text |
| messageRichMessage | fully | stored since msg-rich-store as its blocks' plain text, a curated summary beside TDLib's JSON (section 4.2) and its media (role `rich`, section 4.3); still lost: the layout of a message whose JSON is over 256 KiB; a message TDLib delivers without all of its blocks (`is_full` false) is stored as it came and again in full once `getFullRichMessage` answers | rich |
| messageAnimatedEmoji | not | the emoji itself: this is a text message of one emoji that TDLib reports specially | text |
| messagePhoto | partly | spoiler, caption above media, secret flag, size, live-photo video (`video`) | photo |
| messageVideo | partly | cover, start timestamp, other qualities, size, duration, MIME, spoiler, caption above | video |
| messageAnimation | partly | size, duration, MIME, spoiler, caption above, secret flag | animation |
| messageAudio | partly | title, performer, duration, MIME, album cover | audio |
| messageDocument | partly | MIME type, thumbnail | document |
| messageVoiceNote | partly | duration, waveform, MIME, listened flag | voice |
| messageVideoNote | not | the round video file, duration, length, secret flag | video_note |
| messageSticker | partly | emoji, set id, format, kind (regular, mask, custom emoji), premium flag | sticker |
| messagePaidMedia | not | price in stars, every photo or video (or its preview), caption | paid_media |
| messageExpiredPhoto, messageExpiredVideo, messageExpiredVideoNote, messageExpiredVoiceNote | not | which kind expired; worse, see section 2.1 | expired |

### 3.2 Places, people, games and interactive content

| TDLib class | Stored | What is lost today | Planned kind |
|---|---|---|---|
| messageLocation | not | latitude, longitude, accuracy | location |
| messageLiveLocation | not | the position, period, heading, proximity radius, expiry; later positions | location |
| messageVenue | not | title, address, provider and venue id, type, position | venue |
| messageContact | not | phone, names, vCard, user id | contact |
| messageDice | not | emoji and value | dice |
| messageStakeDice | not | value, stake and prize | dice |
| messageGame | not | game title, description, short name, photo or animation, text | game |
| messagePoll | not | question, options, type (regular, quiz), flags, results, close date, description, media | poll |
| messageChecklist | not | title, tasks, who did which task | checklist |
| messageWidget | fully | stored since msg-widgets as its caption (`text`) and a curated summary beside TDLib's JSON, which holds the HTML (section 4.2); the HTML is shown as escaped source, never rendered (decision [2026-11-20-msg-widgets-html](decisions/2026-11-20-msg-widgets-html.md)) | widget |
| messageStory | not | which story was shared (poster chat, story id) | story |
| messageInvoice | not | product title, description, photo, currency, amount, paid media | invoice |
| messageGiveaway | not | parameters, prize, winner count | giveaway |
| messageCall | not | video or voice, duration, how it ended | call |
| messageGroupCall | not | active or missed, video, duration, participants | call |
| messageUnsupported | not | (TDLib itself cannot read it) | unsupported |

### 3.3 Service messages: chat membership and settings

Since msg-service-detail, every class here keeps its detail as curated
keys (section 4.2.3), and the old-row repair (section 4.9) gave the rows
stored before them their class and keys. What is still lost:

| TDLib class | Stored | What is still lost | service_type |
|---|---|---|---|
| messageBasicGroupChatCreate | fully | nothing; `title` and the initial members' `user_ids` | chat_create |
| messageSupergroupChatCreate | fully | nothing; `title` | chat_create |
| messageChatChangeTitle | fully | nothing; the new `title` | chat_change_title |
| messageChatChangePhoto | fully | a sticker-based photo's sticker and fill; the new photo is the `chat_photo` file (section 4.3), an animated photo's video the one at position 1 | chat_change_photo |
| messageChatDeletePhoto | fully | nothing | chat_delete_photo |
| messageChatAddMembers | fully | nothing; the added members' `user_ids` | chat_add_members |
| messageChatJoinByLink | fully | nothing | chat_join_by_link |
| messageChatJoinByRequest | fully | nothing | chat_join_by_request |
| messageChatJoinFromCommunity | fully | nothing (the community itself is in `telegram_communities`, section 4.13) | chat_join_from_community |
| messageChatDeleteMember | fully | nothing; the removed member's `user_id` | chat_delete_member |
| messageChatAddedToCommunity | fully | nothing (the community itself is in `telegram_communities`, section 4.13) | chat_added_to_community |
| messageChatRemovedFromCommunity | fully | nothing (TDLib gives no community id) | chat_removed_from_community |
| messageChatUpgradeTo | fully | nothing; `supergroup_id` | chat_upgrade_to |
| messageChatUpgradeFrom | fully | nothing; the old `title` and `basic_group_id` | chat_upgrade_from |
| messageChatOwnerLeft | fully | nothing; `new_owner_user_id` | chat_owner_left |
| messageChatOwnerChanged | fully | nothing; `new_owner_user_id` | chat_owner_changed |
| messageChatHasProtectedContentToggled | fully | nothing | chat_has_protected_content_toggled |
| messageChatHasProtectedContentDisableRequested | fully | nothing | chat_has_protected_content_disable_requested |
| messagePinMessage | fully | nothing; the pinned `message_id` | pin_message |
| messageScreenshotTaken | fully | nothing | screenshot_taken |
| messageChatSetBackground | fully | a chat theme background's colours (TDLib gives only the theme's name); the background's fill and pattern settings are curated (section 4.2.3) and its wallpaper or pattern is the `background` file (section 4.3) | chat_set_background |
| messageChatSetTheme | fully | nothing; the theme's name, or for a gift theme its gift | chat_set_theme |
| messageChatSetMessageAutoDeleteTime | fully | nothing; `seconds` and `from_user_id` | chat_set_message_auto_delete_time |
| messageChatBoost | fully | nothing; `boost_count` | chat_boost |
| messageForumTopicCreated | fully | nothing; name, icon and the implicit-name flag (the topic itself is in `telegram_forum_topics`, section 4.5) | forum_topic_created |
| messageForumTopicEdited | fully | nothing; the new name and icon | forum_topic_edited |
| messageForumTopicIsClosedToggled | fully | nothing | forum_topic_is_closed_toggled |
| messageForumTopicIsHiddenToggled | fully | nothing | forum_topic_is_hidden_toggled |
| messageCustomServiceAction | fully | nothing | custom_service_action |
| messageContactRegistered | fully | nothing | contact_registered |
| messageSuggestProfilePhoto | fully | as for messageChatChangePhoto: the photo and an animated photo's video are `chat_photo` files | suggest_profile_photo |
| messageSuggestBirthdate | fully | nothing | suggest_birthdate |
| messageProximityAlertTriggered | fully | nothing | proximity_alert_triggered |
| messageManagedBotCreated | fully | nothing | managed_bot_created |
| messageBotWriteAccessAllowed | fully | nothing | bot_write_access_allowed |
| messageUsersShared | fully | nothing | users_shared |
| messageChatShared | fully | nothing | chat_shared |
| messageWebAppDataSent | fully | nothing | web_app_data_sent |
| messageWebAppDataReceived | fully | nothing (bots only) | web_app_data_received |
| messagePassportDataSent | fully | nothing; the element `types` | passport_data_sent |
| messagePassportDataReceived | partly | the encrypted data, kept out on purpose; only the element `types` (bots only) | passport_data_received |
| messageGameScore | fully | nothing | game_score |

### 3.4 Service messages: calls, polls, checklists, posts

Since msg-service-detail, every class here keeps its detail as curated
keys (section 4.2.3), and the old-row repair (section 4.9) gave the rows
stored before them their class and keys. What is still lost:

| TDLib class | Stored | What is still lost | service_type |
|---|---|---|---|
| messageVideoChatScheduled | fully | nothing | video_chat_scheduled |
| messageVideoChatStarted | fully | nothing | video_chat_started |
| messageVideoChatEnded | fully | nothing | video_chat_ended |
| messageInviteVideoChatParticipants | fully | nothing | invite_video_chat_participants |
| messagePollOptionAdded | fully | nothing | poll_option_added |
| messagePollOptionDeleted | fully | nothing | poll_option_deleted |
| messageChecklistTasksDone | fully | nothing | checklist_tasks_done |
| messageChecklistTasksAdded | fully | nothing | checklist_tasks_added |
| messageSuggestedPostApprovalFailed | fully | nothing | suggested_post_approval_failed |
| messageSuggestedPostApproved | fully | nothing | suggested_post_approved |
| messageSuggestedPostDeclined | fully | nothing | suggested_post_declined |
| messageSuggestedPostPaid | fully | nothing | suggested_post_paid |
| messageSuggestedPostRefunded | fully | nothing | suggested_post_refunded |

### 3.5 Service messages: payments, gifts, stars

Since msg-service-detail and msg-gifts, every class here keeps its
detail as curated keys (section 4.2.3); the old-row repair (section
4.9) gave the rows stored before them their class and keys. What is
still lost:

| TDLib class | Stored | What is still lost | service_type |
|---|---|---|---|
| messagePaymentSuccessful | fully | nothing (the invoice is its own message, linked by id) | payment_successful |
| messagePaymentSuccessfulBot | fully | the payload and order info, which only the bot sees | payment_successful_bot |
| messagePaymentRefunded | fully | nothing | payment_refunded |
| messageGiftedPremium | fully | nothing; its animation sticker is a `gift` file | gifted_premium |
| messagePremiumGiftCode | partly | the code itself, never kept since anyone reading the archive could redeem it (`has_code` says there was one) | premium_gift_code |
| messageGiftedStars | fully | nothing; its animation sticker is a `gift` file | gifted_stars |
| messageGiftedGrams | fully | nothing; its animation sticker is a `gift` file | gifted_grams |
| messageGiveawayCreated | fully | nothing (a giveaway without Stars has no `star_count`) | giveaway_created |
| messageGiveawayCompleted | fully | nothing | giveaway_completed |
| messageGiveawayWinners | fully | nothing | giveaway_winners |
| messageGiveawayPrizeStars | fully | nothing; its animation sticker is a `gift` file | giveaway_prize_stars |
| messageGift | fully | the gift's background colours; its sticker is a `gift` file | gift |
| messageUpgradedGift | fully | nothing; model, symbol, backdrop and original details are curated, the model and symbol stickers `gift` files | upgraded_gift |
| messageRefundedUpgradedGift | fully | nothing | refunded_upgraded_gift |
| messageUpgradedGiftPurchaseOffer | fully | nothing | upgraded_gift_purchase_offer |
| messageUpgradedGiftPurchaseOfferRejected | fully | nothing | upgraded_gift_purchase_offer_rejected |
| messagePaidMessagesRefunded | fully | nothing | paid_messages_refunded |
| messagePaidMessagePriceChanged | fully | nothing | paid_message_price_changed |
| messageDirectMessagePriceChanged | fully | nothing | direct_message_price_changed |
| messageUnpaidMessagesAllowedByLink | fully | nothing beyond the event itself | unpaid_messages_allowed_by_link |
| messageTonWalletTransfer | fully | nothing | ton_wallet_transfer |
| messageTonConnectRequest | fully | nothing | ton_connect_request |

A gift is kept as Telegram shows it: its sticker (an upgraded gift's
model and symbol stickers) and the animation stickers of gifted
Premium, Stars, grams and giveaway prizes, and a giveaway's prize
sticker, are message files with role `gift` (section 4.3); an upgraded
gift's model, symbol, backdrop and original details, and the gift a
chat theme comes from, are curated keys (section 4.2.3). Rows stored
before keep what they have. A Premium gift code's code is never kept.

Every service_type is the TDLib class name without its `message`
prefix in snake case, which is what the existing ones already are
except `chat_create` (two classes share it) and `payment_successful`
(today used for both payment classes; the bot one gets its own name
for new rows). See decision
[2026-11-18-msg-service-type-names](decisions/2026-11-18-msg-service-type-names.md).

### 3.6 Message-level fields

| `td_api::message` field | Stored | What is lost today |
|---|---|---|
| forward_info | partly | `source` (the chat and message a forward was saved from), public service announcement type |
| reply_to (messageReplyToMessage) | partly | quote (text, entities, position), checklist task or poll option replied to, origin and content of a reply into a chat not logged |
| reply_to (messageReplyToStory) | not | the story replied to |
| topic_id | not | forum topic, thread, direct-messages or saved-messages topic |
| is_pinned | not | pinned state (only the pin service message, without its target) |
| interaction_info | not | views, forwards, reply count, reactions |
| unread_reactions | not | (account-local state, not archived on purpose) |
| via_bot_user_id | not | inline bot used |
| reply_markup | not | inline and reply keyboards (buttons, URLs) |
| effect_id | not | message effect |
| self_destruct_type, auto_delete_in | not | self-destruct timer |
| paid_message_star_count | not | stars paid to send it |
| sender_boost_count, sender_tag | not | sender's boosts and tag |
| fact_check | not | fact check text |
| import_info | not | imported-from name and date |
| restriction_info | not | why the message is restricted |
| suggested_post_info | not | suggested post state and price |
| media_album_id, author_signature, is_channel_post, date, edit_date | fully | - |

## 4. Design

### 4.1 The content kind (`content_type`) and the exact class (`tdlib_type`)

The `content_type` ENUM of both message tables and both edit tables
gains, appended at the end so existing values keep their numbers:
`video_note, location, venue, contact, dice, game, poll, checklist,
story, paid_media, invoice, giveaway, call, rich, widget, expired,
unsupported`. `unknown` stays and from now on means only "a class
newer than the daemon's code".

A new column `tdlib_type VARCHAR(64)` holds the exact TDLib class name
(for example `messageVideoNote`) for every row written after the
change, so no row is ever ambiguous again, and a later TDLib with new
classes is still recorded by name.

See decision
[2026-11-18-msg-content-kinds](decisions/2026-11-18-msg-content-kinds.md).

Both message tables have a FULLTEXT index on `text`, and with it
InnoDB can neither append ENUM members nor add a column in place: only
a copy of the table works, which took about ten hours on a synthetic
table of the archive's size. Migration 000034 therefore drops the
FULLTEXT index, makes the changes (the ENUM append is then a metadata
change and the new columns an online rebuild), and builds the index
again. It also adds every other column this design puts on the two
message tables (sections 4.5 to 4.7), so they are rebuilt only once.
See decision
[2026-11-18-msg-message-table-ddl](decisions/2026-11-18-msg-message-table-ddl.md).
The edit tables and `telegram_files` change instantly.

`telegram_files.file_type` gains `video_note`, and `thumbnail` for
the still thumbnails of videos (section 4.3).

### 4.2 Structured detail (`content_extra`)

A new nullable `content_extra JSON` column on both message tables and
both edit tables holds the fields of section 3 that a reader needs and
that have no column, as a curated object per class with stable keys,
for example:

- location: `{"lat":..,"lon":..,"accuracy":..}` plus, for a live
  location, `{"live_period":..,"heading":..,"proximity_radius":..}`;
- contact: `{"phone":..,"first_name":..,"last_name":..,"vcard":..,
  "user_id":..}`;
- sticker: `{"emoji":..,"set_id":..,"format":"webp|tgs|webm",
  "sticker_type":..,"custom_emoji_id":..}`;
- media: `{"width":..,"height":..,"duration":..,"mime":..,
  "spoiler":true,"caption_above":true}` (only the keys that apply);
- service messages: the action's own fields, e.g. `{"user_ids":[..]}`
  for chat_add_members, `{"message_id":..}` (server id) for
  pin_message, `{"currency":..,"amount":..}` for payments.

User ids, chat ids and message ids in it use the same conventions as
the columns (server message ids). File references are not in the JSON
but in the message-files table (section 4.3), so the foreign keys keep
working.

For a class with no curated mapping yet, `content_extra` holds
`{"tdlib": <the content as TDLib's own JSON>}`, produced by the JSON
serializer TDLib already builds (`tdjson_private`), with local state
removed (file paths and download state, the account's viewed and
listened marks). Until the curated items land that is every class but
`messageText` (fully kept by the text, entities and link preview),
`messageAnimatedEmoji` (stored as its text) and the expired classes
(nothing left); for good it is the rare and bot-only ones, rich
messages and widgets. Encrypted passport data is never stored, and a
content whose JSON exceeds 256 KiB keeps only its class and size
(`tdlib_omitted_bytes`). A later version can turn the JSON into a
curated object from the row alone.

A rich message is the one class that keeps TDLib's JSON beside curated
keys (decision
[2026-11-19-msg-rich-raw-json](decisions/2026-11-19-msg-rich-raw-json.md)):
`{"blocks": <number of top-level blocks>, "is_rtl": <bool>, "tdlib":
...}`. The `tdlib` JSON is what the web draws it from, since page
blocks are some thirty types that TDLib's shape describes best. Its
`text` is the plain text of the blocks, one line per heading,
paragraph, caption, list item (after its label), table row (cells
apart by ` | `), button and so on, in reading order, with custom emoji
as their alternative text and formulas as their source; it has no
entities. The text, the curated keys and the media files (section 4.3)
come from the live object, so a message over the JSON cap, which keeps
only the class under `tdlib`, still has them.

A widget keeps TDLib's JSON the same way, since its HTML is what the
message page shows: `{"width", "height", "has_caption", "widget_id",
"tdlib": ...}`. `width` 0 is the whole width of the bubble and `height`
0 a height that is not fixed; `widget_id` (a string) is there only for
a shared widget. Its caption is the message's `text`. The HTML is shown
as escaped source and never rendered (decision
[2026-11-20-msg-widgets-html](decisions/2026-11-20-msg-widgets-html.md)).

TDLib can deliver a rich message without all of its blocks
(`richMessage.is_full` false). It is stored as it came, then the daemon
asks `getFullRichMessage` for the rest, re-reads the message, puts the
full blocks in place of the partial ones and stores it again through
the normal path, so its text, curated keys and block files are those of
the whole message. A message not edited since keeps its edit date, so
the stored row takes the content and no edit version is written. At
most two such calls a second, each message once per run, and one that
fails is not asked for again within an hour. A Discord forward of such
a message carries what arrived first.

`content_extra` is part of the edit snapshot: an edit that changes a
curated key (a live location moving, say) is a version. The `tdlib`
part does not count when deciding whether an edit changed anything,
since it also changes with nobody editing (refreshed file references),
and a row with no curated keys yet matches any.
See decision
[2026-11-18-msg-content-extra](decisions/2026-11-18-msg-content-extra.md).

### 4.2.1 Media keys

The curated `content_extra` of the media classes replaces their TDLib
JSON. Keys appear only when they apply: a flag only when true, a number
only when non-zero, a string only when non-empty. Ids that are 64-bit in
TDLib are JSON strings, as in TDLib's own JSON, so that JavaScript
readers do not lose precision. Files are not referenced here (section
4.3); file names stay in `telegram_files.orig_file_name`.

Shared keys:

| Key | Meaning |
|---|---|
| `width`, `height` | pixels; for a photo, its largest size |
| `duration` | seconds |
| `mime` | MIME type as sent |
| `spoiler` | true: hidden behind a spoiler |
| `caption_above` | true: caption shown above the media |
| `secret` | true: self-destructing once opened |
| `has_stickers` | true: stickers were added to the media |
| `minithumbnail` | `{"width", "height", "data"}`: TDLib's tiny inline JPEG, base64; shows a blurred preview even when the file is not stored |

Per class:

| Class | Keys |
|---|---|
| messagePhoto | width, height, minithumbnail, spoiler, caption_above, secret, has_stickers; `live_photo`: `{width, height, duration, mime}` when the photo carries a video |
| messageVideo | width, height, duration, mime, minithumbnail, spoiler, caption_above, secret, has_stickers, `streaming` (supports streaming), `start_timestamp`, `cover` (true when a cover photo was set), `qualities`: `[{width, height, codec}]` of the alternative videos |
| messageAnimation | width, height, duration, mime, minithumbnail, spoiler, caption_above, secret, has_stickers |
| messageAudio | duration, mime, `title`, `performer`, minithumbnail (album cover), `album_cover` (true when one exists) |
| messageDocument | mime, minithumbnail |
| messageVoiceNote | duration, mime, `waveform` (base64, TDLib's 5-bit samples), `transcript` (Telegram's speech recognition, when done) |
| messageVideoNote | duration, `length` (diameter in pixels), minithumbnail, waveform, transcript, secret |
| messageSticker | `emoji`, `sticker_id`, `set_id`, `format` (`webp`, `tgs`, `webm`), `sticker_type` (`regular`, `mask`, `custom_emoji`), `custom_emoji_id`, width, height, `premium` (sent as a premium sticker) |
| messagePaidMedia | `star_count`, caption_above, `items`: `[{kind, width, height, duration, minithumbnail}]` with kind `photo`, `video`, `preview` (not paid for: only its size and blur are known) or `unsupported` |

### 4.2.2 Places, people, games and calls

The same rules as section 4.2.1. Coordinates are degrees; chat and user
ids are numbers (they fit in 53 bits); call, game and other 64-bit ids
are strings; message ids are server ids.

| Class | Keys |
|---|---|
| messageLocation | `lat`, `lon`, `accuracy` (metres) |
| messageLiveLocation | lat, lon, accuracy, `live: true`, `live_period`, `heading`, `proximity_radius`; the remaining time is left out, since it changes with nobody moving |
| messageVenue | lat, lon, accuracy, `title`, `address`, `provider`, `venue_id`, `venue_type` |
| messageContact | `phone`, `first_name`, `last_name`, `vcard`, `user_id` |
| messageDice | `emoji`, `value` |
| messageStakeDice | `stake: true`, `value`, `stake_gram_amount`, `prize_gram_amount` |
| messageGame | `game_id`, `short_name`, `title`, `description`, `has_photo`, `has_animation`; the game's text is the message text |
| messageStory | `poster_chat_id`, `story_id`, `via_mention` (a reference only; decision [2026-11-18-msg-stories-reference-only](decisions/2026-11-18-msg-stories-reference-only.md)) |
| messageInvoice | `title`, `description` (`{text, entities}`), `has_photo`, `currency`, `total_amount` (in the currency's smallest unit), `start_parameter`, `is_test`, `need_shipping_address`, `receipt_message_id`, `has_paid_media` |
| messageChecklist | `tasks`: `[{id, text, entities, completed_by: {user_id} or {chat_id}, completion_date}]`, `others_can_add_tasks`, `others_can_mark_tasks_as_done`; the title is the message text |
| messageGiveaway | `boosted_chat_id`, `additional_chat_ids`, `winners_selection_date`, `only_new_members`, `has_public_winners`, `country_codes`, `prize_description`, `winner_count`, `prize`: `{premium_months}` or `{stars}` |
| messageCall | `call_id`, `video`, `duration`, `discard_reason` (`missed`, `declined`, `disconnected`, `hung_up`, `upgrade_to_group_call`) |
| messageGroupCall | `call_id`, `group: true`, `active`, `missed`, `video`, `duration`, `participants`: `[{user_id} or {chat_id}]` |

A live location's new positions arrive as content updates and edits.
Each one updates the live row, but a location keeps at most one version
a minute in the edits table, so a shared position is kept as a track
with one point a minute rather than one a second.

### 4.2.3 Service messages

A service message keeps its English description in `text` and its
action's own fields in `content_extra`, by the same rules as section
4.2.1. Message ids are server ids; user and chat ids are numbers;
64-bit ids are strings; a sender is `{user_id}` or `{chat_id}`; a price
is `{stars}` or `{gram_cents}`; a union type is named by its variant in
snake case (`"origin": "resale"`). A toggle keeps its value even when
false.

| service_type | Keys |
|---|---|
| chat_create | `title`, `user_ids` (basic groups) |
| chat_change_title | `title` |
| chat_change_photo, suggest_profile_photo | `photo`: `{photo_id, width, height, animated, minithumbnail}`; an animated photo's video is the `chat_photo` file at position 1 |
| chat_add_members | `user_ids` |
| chat_delete_member | `user_id` |
| chat_join_from_community, chat_added_to_community | `community_id` |
| chat_upgrade_to | `supergroup_id` |
| chat_upgrade_from | `title`, `basic_group_id` |
| chat_owner_left, chat_owner_changed | `new_owner_user_id` |
| chat_has_protected_content_toggled | `old_value`, `new_value`, `request_message_id` |
| chat_has_protected_content_disable_requested | `expired` |
| pin_message | `message_id` |
| chat_set_background | `background`: `{id, name, dark, type, dark_theme_dimming}` and the fields of its type (below), `old_background_message_id`, `only_for_self` |
| chat_set_theme | `theme_name`, or `theme_kind` for a gift theme; a gift theme also `gift_id` and `gift_name` (its upgraded gift's id and unique name) |
| chat_set_message_auto_delete_time | `seconds`, `from_user_id` |
| chat_boost | `boost_count` |
| forum_topic_created | `name`, `name_implicit`, `icon_color`, `icon_custom_emoji_id` |
| forum_topic_edited | `name`, `icon_edited`, `icon_custom_emoji_id` |
| forum_topic_is_closed_toggled, forum_topic_is_hidden_toggled | `closed`, `hidden` |
| suggest_birthdate | `day`, `month`, `year` |
| game_score | `game_message_id`, `game_id`, `score` |
| managed_bot_created | `bot_user_id`, `manager_bot_user_id` |
| video_chat_scheduled, video_chat_started, video_chat_ended, invite_video_chat_participants | `group_call_id`, `start_date`, `duration`, `user_ids` |
| poll_option_added, poll_option_deleted | `poll_message_id`, `option_id`, `option` (`{text, entities}`) |
| checklist_tasks_done | `checklist_message_id`, `done_task_ids`, `not_done_task_ids` |
| checklist_tasks_added | `checklist_message_id`, `tasks` |
| payment_successful | `invoice_chat_id`, `invoice_message_id`, `currency`, `total_amount`, `subscription_until_date`, `recurring`, `first_recurring`, `invoice_name` |
| payment_successful_bot | currency, total_amount, subscription_until_date, recurring, first_recurring, `telegram_payment_charge_id`, `provider_payment_charge_id` |
| payment_refunded | `owner`, currency, total_amount, the two charge ids |
| gifted_premium, premium_gift_code | `gifter_user_id`, `receiver_user_id` or `creator`, `text`, `currency`, `amount`, `cryptocurrency`, `cryptocurrency_amount`, `month_count`, `day_count`, `from_giveaway`, `unclaimed`, `has_code` |
| gifted_stars, gifted_grams, giveaway_prize_stars | gifter and receiver, amounts, `star_count` or `gram_amount`, `transaction_id`, giveaway chat and message |
| giveaway_created, giveaway_completed, giveaway_winners | `star_count`; `giveaway_message_id`, `winner_count`, `unclaimed_prize_count`; the winners, prize and selection date |
| gift, upgraded_gift, refunded_upgraded_gift | `gift` (`{id, star_count, emoji, ...}` or, upgraded, the upgraded gift below), `sender`, `receiver`, `origin`, `received_gift_id`, `text`, its flags and dates |
| upgraded_gift_purchase_offer(_rejected) | `gift`, `state`, `price`, `expiration_date` or `offer_message_id`, `expired` |
| paid_messages_refunded | `message_count`, `star_count` |
| paid_message_price_changed, direct_message_price_changed | `paid_message_star_count`, `enabled` |
| suggested_post_* | `suggested_post_message_id`, `price`, `send_date`, `comment`, `star_count`, `nanostar_count`, `gram_amount`, `reason` |
| ton_wallet_transfer | `sender_user_id`, `transaction_id`, `peer_address`, `amount`, `comment` |
| ton_connect_request | `session_id`, `state`, `dapp_name`, `topic` |
| users_shared, chat_shared | `users` (`{user_id, first_name, last_name, username}`), `chat` (`{chat_id, title, username}`), `button_id` |
| bot_write_access_allowed | `reason`, `domain_name` |
| web_app_data_sent, web_app_data_received | `button_text`, `data` |
| passport_data_sent, passport_data_received | `types` |
| proximity_alert_triggered | `traveler`, `watcher`, `distance` |

A background's `type` is `wallpaper`, `pattern`, `fill` or
`chat_theme`, and adds the fields of that type to `background`:

| type | Keys |
|---|---|
| `wallpaper` | `is_blurred`, `is_moving`; the JPEG is the `background` file |
| `pattern` | `fill`, `intensity` (0-100), `is_inverted`, `is_moving`; the PNG or TGV (gzipped SVG, `application/x-tgwallpattern`) pattern is the `background` file, drawn over the fill |
| `fill` | `fill`; no file |
| `chat_theme` | `theme_name` (an emoji chat theme; its colours are not given); no file |

A `fill` is `{type, colors, rotation_angle}`: `type` is `solid`,
`gradient` or `freeform_gradient`; `colors` holds the RGB colours as
integers (`0xRRGGBB`), one for a solid fill, the top then the bottom
colour of a gradient, the three or four of a freeform gradient;
`rotation_angle` is a gradient's clockwise rotation in degrees (0-359,
a multiple of 45, absent when 0). The flags are absent when false and
`intensity` when 0, as for the other keys.

An upgraded gift (the `gift` of upgraded_gift and of the two purchase
offer types) is `{id, regular_gift_id, title, name, number,
publisher_chat_id, owner}` and what it looks like:

| Key | Meaning |
|---|---|
| `model` | `{name, rarity, crafted}`: the model; `crafted` when it is only obtained by crafting gifts |
| `symbol` | `{name, rarity}`: the symbol on the backdrop's pattern |
| `backdrop` | `{name, center_color, edge_color, symbol_color, text_color, rarity}`: colours are RGB integers (0xRRGGBB) as TDLib gives them, kept even when 0 (black) |
| `original_details` | `{sender, receiver, text, date}`: the gift it was upgraded from, as TDLib gives it (sender left out for a private gift) |

A rarity is `rarity_per_mille` (how many of 1000 upgraded gifts get the
attribute; 0 means under 0.1% and is kept) or, when Telegram names it
instead, `rarity`: `uncommon`, `rare`, `epic` or `legendary`. The
model's and the symbol's stickers are message files (section 4.3).

Kept out on purpose: a Premium gift code (anyone who can read the
archive could redeem it; `has_code` records that there was one), an
encrypted TON comment (`comment_encrypted` instead), a bot payment's
payload and order info, encrypted passport data (only the element types
are kept), and flags that only say what this account may do. The
classes not listed carry nothing beyond their description.

### 4.3 Several files per message (`telegram_message_files`)

A message can carry more than one file: paid media (several photos or
videos), a photo with a live-photo video, a video with a cover, a poll
with media, a game with a photo, an invoice with a photo, a chat photo
change. A new table follows the pattern of `telegram_link_previews`:

    telegram_message_files (migration 000036)
      id, group_message_id NULL, private_message_id NULL,
      role ENUM('cover','live_video','paid','preview','poll','game',
      'invoice','chat_photo','thumbnail','rich','gift','background',
      'preview_media'), position SMALLINT,
      file_id (FK telegram_files, ON DELETE SET NULL),
      UNIQUE (message, role, position)

The existing `file_id` column stays the main file and keeps its
meaning, so the web and the forwarders do not change; the new table
holds the other files only (decision
[2026-11-18-msg-extra-files-only](decisions/2026-11-18-msg-extra-files-only.md)).
A video note's round video is now the main file, like any other media.

| Role | Files |
|---|---|
| `cover` | a video's cover photo (largest size) |
| `live_video` | the video of a live photo |
| `paid` | each photo or video of paid media, `position` its index among the items (previews of unpaid items have no file); an invoice's paid media at position 0 |
| `game` | a game's photo (position 0) and animation (position 1) |
| `invoice` | an invoice's product photo |
| `chat_photo` | the new photo of a chat photo change or a suggested profile photo: the still photo (largest size) at position 0 and, when the photo is animated, its MPEG-4 video at position 1 (file_type `animation`; the big animation, up to 1280x1280, else the small one, 160x160, when TDLib gives only that) |
| `thumbnail` | the still thumbnail TDLib offers with a video, an animation, a video note or a video (.webm) sticker, when it is an image (JPEG, PNG or WebP; MPEG-4, GIF, TGS and WebM thumbnails are not stored), file_type `thumbnail`: the picture the web shows before the video plays; in a rich message, the still thumbnail of each video and animation of its blocks, at the `position` of its `rich` medium (since msg-rich-video-thumbnails); messages logged before the daemon stored thumbnails get theirs from the opt-in thumbnail pass (section 4.9); a video with no Telegram thumbnail and no cover gets a still generated from its stored bytes (below, "Generated stills") |
| `poll`, `preview` | reserved: poll media (msg-polls); other thumbnails (a document's, a photo's sizes) and a video's other qualities are not stored, since they repeat the media |
| `rich` | the media inside a rich message's page blocks, `position` their order of appearance, depth first through collages, slideshows, quotes, lists, collapsible blocks, covers and embedded posts: each photo (largest size), video, animation, audio file, document and voice note, an embedded page's poster, an embedded post's author photo, the photos of related articles; not a chat link's photo (the chat's own) nor the icons inside rich text |
| `gift` | file_type `sticker`: at position 0 the gift's sticker (gift, refunded_upgraded_gift), an upgraded gift's model sticker (upgraded_gift and the two purchase offer types), the animation sticker of gifted_premium, gifted_stars, gifted_grams and giveaway_prize_stars, or a giveaway's prize sticker; at position 1 an upgraded gift's symbol sticker. Each keeps its position when the other is missing. Not a premium_gift_code's sticker |
| `background` | a chat background's document at position 0: the wallpaper (JPEG) or the pattern (PNG or TGV), file_type `document`; a fill or a chat theme has none |
| `preview_media` | a link preview's own media, beyond its image in `telegram_link_previews.photo_file_id` (section 4.8), by the preview's `type`: position 0 is its one medium where it has one: the video, animation or audio of such a preview or the cached medium of an embedded player, a document preview's file or a background's document (file_type `document`), a sticker preview's sticker or a gift auction's gift sticker (`sticker`), a video note's video (`video_note`), a voice note's voice (`voice`), a story album's video icon (`video`); a video preview's cover is at position 1 (`photo`, largest size); an album's item i at position i (`photo`, largest size, or `video`; a missing item leaves its position empty); the first five stickers of a sticker set or icons of a gift collection at their index (`sticker`); an upgraded gift's model sticker at 0 and symbol sticker at 1 (`sticker`), each keeping its position when the other is missing |

The last four roles come with migration 000045; nothing writes them
until their items land (`rich` is written since msg-rich-store,
`preview_media` since msg-link-preview-media, `background` since
msg-chat-backgrounds, `gift` since msg-gifts). The
ENUM append keeps every existing value's number, so it is instant
(`ALGORITHM=INSTANT`, confirmed on a copy with rows), and its down
file first deletes the rows that use them.

Each file goes through the same path as a main file: linked when it is
already known, recorded without its bytes where the storage cache rules
say so (`docs/design/storage-cache.md`), else downloaded. The fetcher
can fetch one again from the message, which it finds through the
table (a `MessageExtra` source with the role and position). A `rich`
file is found by its unique id instead (a `RichMedia` source: the
message is read again with `getMessage` and the medium picked out of
its blocks, as the photo history sources do with the photo lists),
since an edit of the page can move a medium to another position; one
without a unique id keeps the positional source. An edit
that replaces a file overwrites its (role, position); one that drops a
file leaves the old row in place. See decision
[2026-11-18-msg-message-files](decisions/2026-11-18-msg-message-files.md).

#### Generated stills

A video, an animation, a video note or a video (.webm) sticker whose
bytes are stored but which has neither a cover nor a Telegram thumbnail
gets a still taken from those bytes (decisions
[2026-11-20-video-generated-stills](decisions/2026-11-20-video-generated-stills.md)
and [2026-11-21-video-stills-format](decisions/2026-11-21-video-stills-format.md)):
the frame at 1 s, or halfway into a shorter clip, fitting 320 px on its
long side, as a JPEG (PNG for a sticker, which keeps its transparency),
at most 200 KB.

- Who: the stills service (docker-compose.yml, `docker/stills/`), ffmpeg
  in a container of its own with no network, no secrets and the store
  read-only, over a Unix socket shared with the daemon only. It refuses
  paths outside the store and files with no video stream, and kills
  ffprobe and ffmpeg after 20 s. The protocol is in
  `docker/stills/server.py`.
- When: the daemon's StillsWorker, behind `TG_GENERATE_STILLS` (off by
  default; `dry` only logs how many messages and videos there are per
  table and kind). At start it lists the stored videos whose live
  messages need a still, newest first; a new message is looked at half
  a minute after its main file is stored or linked, so that a Telegram
  thumbnail stored just after the video comes first; a generated still
  the storage janitor evicts is taken again as soon as the sweep that
  evicted it is over (docs/design/storage-cache.md section 9.1). One
  video at a time, about one a second, on a thread of its own: storing
  messages never waits for it. A video the service refuses is not asked
  for again until the next start.
- Which: a live message of those kinds whose main file is in the store
  (`on_disk`, with a digest) and that has no `cover` and no
  `thumbnail`, except a thumbnail that is a generated still evicted
  from the store (taken again) or a link whose file row is gone. The
  pass reads the `(content_type, date)` index and the message files'
  unique key; a new message is read by its unique key.
- Stored: through the store's normal path, as a telegram_files row with
  file_type `thumbnail`, `tg_file_id` `generated:<sha256>`, no
  `tg_unique_id` and `generated_from_file_id` the video's row
  (migration 000052). It is linked as `thumbnail`, position 0, to every
  live message whose main file is the video, once per video: another
  message of the same video gets the existing still linked. It replaces
  only a generated still or a lost link, never a thumbnail from
  Telegram, and a Telegram thumbnail stored later replaces it (the
  normal upsert of the role).
- Fetching: a generated still is never asked of Telegram (the fetch
  answers 410 `generated`, docs/internal-api.md); when evicted, it is
  taken again from the video (docs/design/storage-cache.md section
  9.1).

### 4.4 Polls

A poll changes after it is sent (votes, added options, closing), and
TDLib announces it with `updatePoll` keyed by the poll id, not the
message. Two tables (migration 000037):

    telegram_polls
      id (TDLib poll id), question, question_entities JSON,
      type ENUM('regular','quiz'), is_anonymous,
      allows_multiple_answers, allows_revoting, members_only,
      country_codes JSON, correct_option_ids JSON (quiz: 0-based
      positions), explanation, explanation_entities JSON,
      media JSON, explanation_media JSON, open_period, close_date,
      is_closed, total_voter_count, recent_voters JSON
    telegram_poll_options
      poll_id, option_id (TDLib's, a string), position, text,
      entities JSON, media JSON, voter_count, vote_percentage,
      author_user_id or author_chat_id, addition_date, deleted_at

The message row's `content_extra` is `{"poll_id": "<id>", "has_media":
true}`, and its `text` is the poll's description. Each ingested poll
message and every `updatePoll` store the poll as it stands; the results
are not versioned (the edit history stays about what the message said).
An option that disappears from the poll is kept with `deleted_at` set.
`updatePoll` does not carry the poll's own media, so a stored one is
kept.

Media (`media` columns) use the curated shape of section 4.2.1 plus a
`kind` (`photo`, `video`, `animation`, `audio`, `document`, `sticker`,
`location`, `venue`, `link`). Their files are in
`telegram_message_files` with role `poll`: the poll's own media at
position 0, a quiz explanation's at 1, and option i's at 2 + i.

Left out, as they describe only this account: its own vote, whether it
may vote or see the voters, and the order its client shows the options
in. Votes of individual users are not available to a user account for
anonymous polls and come only to bots otherwise, so they are not
stored. See decision
[2026-11-18-msg-polls](decisions/2026-11-18-msg-polls.md).

### 4.5 Forum topics

Group rows have `topic_kind ENUM('forum','thread','direct','saved')
NULL` and `topic_id BIGINT NULL` for `message.topic_id`, indexed with
`chat_id` (added by migration 000034). A forum topic's id is TDLib's
`forum_topic_id`; a thread's is the server id of the thread's first
message (comments of a channel post in its discussion group); direct
and saved-messages topics keep TDLib's id. A copy of a message without
a topic leaves a stored one in place.

The topics are in `telegram_forum_topics` (migration 000039), keyed by
`(chat_id, topic_id)`: `name`, `is_name_implicit`, `icon_color`,
`icon_custom_emoji_id`, `creation_date`, `creator_user_id` or
`creator_chat_id`, `is_general`, `is_closed`, `is_hidden`, `is_pinned`.
There is no foreign key to `telegram_groups`, since topic updates can
arrive before the group row is written. Unread counts, notification
settings and drafts describe this account and are left out.

The table is filled from three sources:

- `updateForumTopicInfo`, which replaces the stored state;
- one `getForumTopics` sweep per forum per run, started when
  `updateSupergroup` says the group is a forum (at most 20 pages of
  100), which also sets `is_pinned`; `updateForumTopic` keeps it
  current;
- a "topic created" service message, which only fills in a topic not
  stored yet, since history backfill reads such messages long after
  the topic may have been renamed.

### 4.6 Message-level metadata (`meta`)

A nullable `meta JSON` column on both message tables (added by
migration 000034; not snapshotted on edits) holds the fields of section
3.6 that are rare and not queried. Every copy of a message sets it, as
it sets the identity columns: it is not content, so it follows the copy
being written rather than the edit date. `is_pinned` (also 000034) is
set the same way and kept current by `updateMessageIsPinned`;
`updateMessageFactCheck` sets or removes `meta.fact_check` alone.

| Key | Value |
|---|---|
| `via_bot_user_id` | the inline bot used |
| `business_bot_user_id` | the business bot that sent it |
| `guest_bot_caller` | `{user_id}` or `{chat_id}` |
| `reply` | `quote` (`{text, entities, position, manual}`), `checklist_task_id`, `poll_option_id`; for a reply into a chat not logged also `origin` (`{kind, user_id or chat_id, message_id, sender_name, author_signature}`), `origin_send_date` and `content` (`{tdlib_type, text}`) |
| `reply_to_story` | `{poster_chat_id, story_id}` |
| `forward_source` | where a forward was saved from: `{chat_id, message_id, sender, sender_name, date}` |
| `psa_type` | public service announcement type of a forward |
| `import` | `{sender_name, date}` of an imported message |
| `link_preview_options` | `{disabled, url, force_small_media, force_large_media, show_above_text}`, when not the defaults |
| `reply_markup` | `{kind}`: `inline_keyboard` or `keyboard` with `rows` of `{text, type, url, query, user_id, copy_text}`, `force_reply`, `remove_keyboard` |
| `effect_id` | the message effect |
| `self_destruct_time`, `self_destruct_immediately` | the self-destruct timer |
| `paid_message_star_count` | stars paid to send it |
| `sender_boost_count`, `sender_tag` | the sender's boosts and tag |
| `fact_check` | `{text, entities, country_code}` |
| `restriction` | `{reason, sensitive}` |
| `suggested_post` | `{price, send_date, state}` |

Left out: bot callback data (the bot's own payload), the time left
before a self-destruct or auto-delete (it changes by the second), and
what describes this account (`can_*`, unread marks, `is_outgoing` of a
forward source). A plain reply in the same chat needs no `reply` key:
the reply columns hold it.

### 4.7 Views and reactions

`interaction_info` (views, forwards, reply count, reaction totals)
changes constantly on busy channels. The columns `view_count`,
`forward_count`, `reply_count` and `reactions JSON` (added by migration
000034) hold the latest totals. Every stored copy of a message sets
them; a copy that reports none keeps the stored ones. `reactions` is an
array of `{emoji | custom_emoji_id | paid: true, count}`; `[]` means
the reactions were all withdrawn. Reactions that are Saved Messages
tags are this account's own labels and are left out.

Between copies, `updateMessageInteractionInfo` (all totals) and
`updateMessageReactions` (reactions only) are coalesced per message on
the TDLib loop and written in one batch every `TG_INTERACTION_FLUSH_SECS`
(default 60; 0 keeps only the totals a stored copy carries), so a
message changes in the database at most once per interval however busy
it is. Every ten minutes the daemon logs how many updates arrived for
how many messages and how many writes they became.

Measured over 19.5 hours of those reports on the live account
(2026-11-18 and 19), updates arrive at about 8 a minute (476 an hour;
402 in a typical hour, 1368 in the busiest), for about 220 distinct
messages an hour, and the 60-second interval turns them into about 5
writes a minute (297 an hour). That is negligible next to the
messages the daemon stores, so the default stays at 60 seconds: totals
on the web are at most a minute old. Each write also moves the row's
`updated_at`, which nothing reads for messages.

Who reacted with what is available only through
`getMessageAddedReactions` and only where the chat allows it; it is
left out, and so is the history of the counters (decision
[2026-11-18-msg-interaction-latest-totals](decisions/2026-11-18-msg-interaction-latest-totals.md)).

### 4.8 Link previews

`telegram_link_previews` gains `description_entities JSON`,
`show_media_above_description` and `skip_confirmation` (migration
000041; instant, as the table has no FULLTEXT index). The message's
`link_preview_options` (turned off, another URL, small or large media,
above the text) go into its `meta` as `link_preview_options`, not onto
the card: a message with previews turned off has no card (decision
[2026-11-18-msg-link-preview-options-in-meta](decisions/2026-11-18-msg-link-preview-options-in-meta.md)).

Migration 000045 adds `extra JSON NULL`, also instant: the preview
type's own fields that have no column (a video's duration, an embedded
player's URL and size, the target of a message or story preview), as a
curated object like `content_extra`, NULL when the type has none. The
preview's own video, animation or audio is a `preview_media` file of
the message (section 4.3). Both are written by msg-link-preview-media.

The keys of `extra`, by `type`; a key whose value would be empty, zero
or false is left out, and an object left empty is stored as NULL:

| Type | Keys |
|---|---|
| `article`, `photo`, `app`, `web_app` | `width`, `height` (the image's largest size), `has_stickers`, `minithumbnail` |
| `embedded_video_player`, `embedded_animation_player`, `embedded_audio_player` | `embed_url`, `embed_width`, `embed_height` (the player's expected size), `duration`; `media`, the cached medium's own keys as for `video`, `animation` or `audio` (without `file_name`, `start_timestamp` and `cover`) when TDLib has it |
| `video` | `width`, `height`, `duration`, `mime`, `has_stickers`, `streaming`, `minithumbnail`, `file_name`, `start_timestamp`, `cover` (true when the video has a cover, which is a `preview_media` file at position 1) |
| `animation` | `width`, `height`, `duration`, `mime`, `minithumbnail`, `file_name` |
| `audio` | `duration`, `mime`, `audio_title`, `performer`, `file_name` |
| `document` | `mime`, `file_name` |
| `sticker` | `emoji`, `sticker_id`, `set_id` (strings), `format`, `width`, `height` |
| `chat` | `chat_type` (`basic_group`, `supergroup` or `channel`), `creates_join_request` |
| `user` | `is_bot` |
| `video_chat` | `is_live_stream`, `joins_as_speaker` |
| `message` | `chat_id` (TDLib's chat id, for a `t.me/c/` link) or `username` (a public link), and `message_id` (the server id); none when the URL is not a message link |
| `story` | `poster_chat_id`, `story_id` |
| `external_video` | `media_url`, `mime`, `width`, `height`, `duration` |
| `external_audio` | `media_url`, `mime`, `duration` |
| `video_note` | `duration`, `length`, `minithumbnail`, `waveform`, `transcript` |
| `voice_note` | `duration`, `mime`, `waveform`, `transcript` |
| `album` | `caption`; `items`, one per album item in order (its index is its `preview_media` position): `kind` (`photo` or `video`) with a photo's keys as for `photo` or a video's as for `video` (with `file_name`), or null for an item TDLib leaves empty |
| `background` | `background_type` (`wallpaper`, `pattern`, `fill` or `chat_theme`) and that type's keys as in a chat background's `content_extra` (section 4.2.3: `is_blurred`, `is_moving`, `fill`, `intensity`, `is_inverted`, `theme_name`); `mime`, `file_name` of its document |
| `sticker_set` | `set_id` (string); `stickers`, the first five TDLib gives (it gives up to four), each `emoji`, `sticker_id`, `format`, `width`, `height` |
| `gift_collection` | `icons`, the first five, keyed as a `sticker_set`'s `stickers` |
| `gift_auction` | `gift` (a regular gift's keys as in a gift message's `content_extra`), `auction_end_date`, `auction_start_date`, `gifts_per_round` |
| `upgraded_gift` | `gift` (an upgraded gift's keys as in an upgraded gift message's `content_extra`: ids, title, name, number, owner, model, symbol, backdrop, original details) |
| `live_story` | `poster_chat_id`, `story_id` |
| `story_album` | the photo icon's `width`, `height`, `has_stickers`, `minithumbnail`; `video`, the video icon's keys as for `video` |
| `text_composition_style` | `custom_emoji_id` (string; the emoji itself is in `telegram_custom_emoji`, section 4.11) |
| `theme` | `base_theme` (`classic`, `day`, `night`, `tinted` or `arctic`), `accent_color` (kept when 0), `background` (`name`, `dark`, `type` and that type's keys), `outgoing_message_fill`, `animate_outgoing_message_fill`, `outgoing_message_accent_color` (when the theme has one); not the theme's files |

The other types (`channel_boost`, `supergroup_boost`, `invoice`,
`group_call`, `direct_messages_chat`, `premium_gift_code`,
`request_managed_bot`, `shareable_chat_folder`, `unsupported`) have no
`extra`. A premium gift code's code is in its URL and stays there
(the message's text has it anyway); nothing else of it is kept, as for
the gift code service message. `audio_title` is not
`title`, which is the card's own column. TDLib gives a `message`
preview no target, so it is read from the URL: `t.me`, `telegram.me`
and `telegram.dog` links (`/<username>/<id>`, `/c/<channel>/<id>`, with
or without a topic id between) and `tg://resolve?domain=&post=` and
`tg://privatepost?channel=&post=`. A `t.me/c/` link's chat id is
TDLib's, `-100` followed by the channel id.

Every TDLib 1.8.70 preview type has its own `type` name, the snake
case of its class name; `unsupported` is left for
`linkPreviewTypeUnsupported` and for a class a later TDLib adds. The
names stored before keep their meaning, and rows stored as
`unsupported` before the names below existed keep that type: only new
and edited messages get `video_note`, `voice_note`, `album`,
`background`, `direct_messages_chat`, `sticker_set`, `gift_collection`,
`gift_auction`, `upgraded_gift`, `live_story`, `story_album`,
`text_composition_style`, `theme`, `premium_gift_code`,
`request_managed_bot` and `shareable_chat_folder`.

The card's image (`photo_file_id`) is, for these: a direct messages
chat's photo, a story album's photo icon (largest size), a background's
document's thumbnail and a video note's thumbnail; the others have
none, their pictures being `preview_media` files.

The `preview_media` files are the preview's own media, by position
(section 4.3): at position 0 the medium of a `video`, `animation`,
`audio`, `document`, `sticker`, `video_note` or `voice_note` preview
(the document's or the sticker's own file, beside the document's
thumbnail as the card's image), the cached medium of an embedded
player, a `background`'s document, a `story_album`'s video icon or a
`gift_auction`'s sticker; at position 1 a `video` preview's cover
(largest size); an `album`'s items and the first five stickers of a
`sticker_set` or `gift_collection` at their index; an `upgraded_gift`'s
model sticker at 0 and symbol sticker at 1. A theme's files are not
kept. An edit that changes the preview overwrites `extra` and each
medium's row, like the card's other fields; one that drops a medium
keeps its old row, as it keeps the old image.

### 4.9 Repairing old rows

Rows stored before classification as `unknown`, and service rows
without their detail, are re-read with `getMessage` and stored again
through the normal path, which classifies them. The pass is opt-in: it
runs at startup when `TG_REPAIR_MESSAGES_ON_START` is set, and `dry`
only logs how many rows it would re-read, per table and kind. It shares
the relink re-fetch's budget (10 `getMessage` a second for both), of
which the repair takes at most 5, and it waits while chat rechecks
are pending (`recheck::repairMayRun`).

A row qualifies while it is live, has no `tdlib_type`, and is
`unknown` or `service`. A re-read row gets its `tdlib_type`, so a pass
cut short by a restart resumes where it was without any saved state.
A row whose message TDLib cannot return any more keeps what it has and
is tried again on every start while the flag is on. Media rows are not
re-read for the detail of section 4.2 (decision
[2026-11-18-msg-repair-scope](decisions/2026-11-18-msg-repair-scope.md)).
The local re-map of rows holding only TDLib's JSON, once planned here,
is dropped (decision
[2026-11-18-msg-repair-opt-in](decisions/2026-11-18-msg-repair-opt-in.md)).

Rich rows stored before their mapping (`rich`, with a `tdlib_type` but
only TDLib's JSON in `content_extra`) are re-read by the same pass,
not re-mapped from the JSON (decision
[2026-11-19-msg-rich-reread](decisions/2026-11-19-msg-rich-reread.md)).
Such a row qualifies while it is live and its `content_extra` has no
`blocks` key; the normal path writes that key along with the text and
the media files, so a re-read row is not listed again, and one TDLib
cannot return keeps its JSON. The dry run counts them as `rich`.

The pass has run once over the rows stored before classification and is
off again. Of the rows it started with,
about 99.9 % were repaired: they now carry their class and, for service
messages, their detail. About 0.1 % stayed as they were, because TDLib
could no longer return their message: in groups only (no private chat
kept any), roughly half of them `unknown` rows and half service rows,
most of those pins, and the rest member, photo and title changes and
video chat notices; and almost no rich rows. They keep what they had
(the web shows them with its placeholder for an unknown kind). The pass itself
met no flood wait: under Telegram's limits TDLib sat out the waits
inside its requests, which is why the pass was capped at five re-reads
a second and waits for chat rechecks.

Videos, animations, video notes and video (.webm) stickers logged
before the daemon stored their still thumbnails (role `thumbnail`,
section 4.3) are re-read by a second opt-in pass,
`TG_THUMBNAILS_ON_START` (`dry` only logs how many there are per table
and kind). A message qualifies while it is live and has neither a
`thumbnail` nor a `cover` file; a sticker counts when its stored file's
extension is `webm` or, for one stored without its file, its curated
`format` is `webm`. The selection reads the `(content_type, date)`
index and the message files' unique key. Each is re-read with
`getMessage` through the normal path, which stores the still TDLib
offers under the store's cap and changes nothing else (no edit
version). The pass shares the repair's part of the budget (both
together at most 5 a second), runs once the repair queue is empty and
waits for chat rechecks the same way. A message that got its still is
not listed again; one whose video has no Telegram thumbnail stays a
candidate and is re-read on every start while the flag is on, so the
flag goes back off after a pass. The stills generated from the stored
bytes (section 4.3, "Generated stills") cover those.

### 4.10 Forwarding to Discord

Each kind follows a forwarding flag of its own (migrations 000031 and
000048; decision
[2026-11-20-fwd-more-types](decisions/2026-11-20-fwd-more-types.md)):
`fwd_animation`, `fwd_video_note`, `fwd_voice`, `fwd_audio`,
`fwd_poll` (quizzes too), `fwd_checklist`, `fwd_dice`, `fwd_game`,
`fwd_location` (venues and live locations too), `fwd_contact`,
`fwd_story`, `fwd_paid_media`, `fwd_invoice` and `fwd_service`, beside
text, photo, video, document and sticker. Rich messages, calls,
giveaways and every kind added later follow `fwd_other`. A message is
filtered part by part: its file follows its kind's flag; a message's
text or caption (a text message, a message with a file, paid media)
follows `fwd_text`; text that belongs to the kind (a poll's
description, a game's text, a checklist's title, a service line, a
rich message) follows the kind's flag. The rules are in
`src/tgloggerd/ForwardFilter.hpp`.

A kind with nothing to read but its structure is forwarded as a short
plain-text line after its text or caption, which keeps the caption's
formatting offsets valid; the line follows the kind's flag, so a
webhook can get the caption without it or the line without the
caption:

| Kind | Line |
|---|---|
| location, live location | `Location: <map link>`, `Live location: <map link>` (OpenStreetMap) |
| venue | `Venue: <title>, <address>` and a map link |
| contact | `Contact: <name>, <phone>` |
| dice | `Dice <emoji>: <value>` |
| poll | `Poll: <question>` (or `Quiz:`) and one `- <option>` line per option |
| game | `Game: <title>` |
| story | `Shared a story` |
| invoice | `Invoice: <title> (<currency>)` |
| checklist | `Checklist:` and one `[x]` or `[ ]` line per task, under the title (the message text) |
| call | `Call, 1:23`, `Video call`, `Missed call`, `Declined video call`; `Group call, 1:02:05` |
| giveaway | `Giveaway: <n> winners` |
| paid media | `Paid media: <n> items, <stars> stars` |

A video note forwards its round video as its file, like any other
media; paid media has no main file and forwards its line only.

The other direction, Discord to Telegram (`discord_telegram_routes`,
discordd), filters the same way, part by part (migrations 000031,
000048 and 000050; the rules are in `src/discordd/RouteFilter.hpp`
and the lines in `src/discordd/Render.cpp`):

| Part | Flag | Sent as |
|---|---|---|
| content | `fwd_text` | the text, under the author line |
| image, video, other file | `fwd_photo`, `fwd_video`, `fwd_document` | `[<file name>] <link>` |
| audio file, GIF | `fwd_audio`, `fwd_animation` | `[<file name>] <link>` |
| sticker | `fwd_sticker` | `[sticker: <name>] <link>` |
| voice message | `fwd_voice` | a voice note, captioned with the author line; `[voice message, 0:12] <link>` when it is too large, cannot be downloaded or sent, or the route takes another part of the message too; an upload TDLib took but did not confirm in time gets no link line, since it may still arrive |
| rich embed (a bot's or webhook's) | `fwd_embed` | its author, title, description, `<name>: <value>` per field, and its URL |
| poll | `fwd_poll` | `Poll: <question>` and one `- <answer>` line each, then `Closes <time>` |
| system message | `fwd_system` | one line, such as `Joined the server`, `Pinned a message` or `Started a thread: <name>` |
| forwarded message | the flags of its parts | `Forwarded:`, then the forwarded message's text, files, stickers and rich embeds, each under its own flag as above; a forwarded voice message is its `[voice message, 0:12] <link>` line, never a voice note |

Embeds Discord adds when it unfurls a link are left out: the link is
in the text already. A system message is any type but the default,
a reply and a bot's answer to a slash or context menu command; its
content is the event's detail and follows `fwd_system` with it. A
message with nothing the route takes is not sent at all. A voice
message is never edited, and a deleted one is tombstoned through its
caption.

A forward carries no parts of its own, only a copy of the message it
forwarded (its snapshot). The author line is the forwarder's, and the
`Forwarded:` line comes only with a part the route takes, so a forward
with nothing the route takes is not sent either. The archive keeps the
forwarded text as the forward's text (`discord_messages.content`) and
the forwarded files with its own (`discord_attachments`); a forward is
not recorded as a reply. Edits and deletes of a forward take the same
paths as any other message.

### 4.11 Custom emoji (`telegram_custom_emoji`)

Messages refer to a custom emoji by its id only: in text entities
(`textEntityTypeCustomEmoji`), reactions, forum topic icons, emoji
statuses and a sticker's `custom_emoji_id`. To draw one, the web needs
the sticker behind it. Migration 000046 adds a table for them:

    telegram_custom_emoji (migration 000046)
      custom_emoji_id BIGINT PRIMARY KEY,
      emoji VARCHAR(16), set_id BIGINT NULL,
      format ENUM('webp','tgs','webm') NULL,
      needs_repainting TINYINT(1),
      file_id (FK telegram_files, ON DELETE SET NULL),
      created_at, updated_at

One row per custom emoji, shared by every message that uses it. It is
filled on demand from `getCustomEmojiStickers` (up to 200 ids a call)
when an id without a row is asked for; the sticker's file is
an ordinary `telegram_files` row, recorded without its bytes until
someone views it, under the storage cache rules. `needs_repainting`
says the sticker is drawn in the text colour, as emoji statuses are.
The daemon fills it through `POST /v1/custom-emoji`
(docs/internal-api.md), which the web calls with the ids it is about
to draw: ids with a row are answered from the table, the rest are
asked of Telegram in one call. A sticker's file whose reference has
expired is found again by asking for its custom emoji afresh. The web
side is msg-custom-emoji-web.

### 4.12 Message effects (`telegram_message_effects`)

A message's effect (`meta.effect_id`) is not a custom emoji: its
`messageEffect` has up to four files, none keyed by a custom emoji id,
so it gets a sibling table
([2026-11-19-msg-effects-sibling-table](decisions/2026-11-19-msg-effects-sibling-table.md)):

    telegram_message_effects (migration 000047)
      effect_id BIGINT PRIMARY KEY,
      emoji VARCHAR(16), is_premium TINYINT(1),
      effect_type ENUM('emoji_reaction','premium_sticker') NULL,
      static_icon_file_id, select_animation_file_id,
      effect_animation_file_id, sticker_file_id
        (each FK telegram_files, ON DELETE SET NULL),
      sticker_format ENUM('webp','tgs','webm') NULL,
      created_at, updated_at

- `static_icon_file_id`: the static icon (WEBP), if the effect has
  one; `emoji` stands in for it otherwise.
- `emoji_reaction`: `select_animation_file_id` and
  `effect_animation_file_id` are the reaction's select and effect
  animations (TGS).
- `premium_sticker`: `sticker_file_id` is the sticker (in
  `sticker_format`) and `effect_animation_file_id` its premium
  animation.

Rows are filled on demand from `getMessageEffect` through
`POST /v1/message-effects`, like the custom emoji; each file is a
metadata-only `telegram_files` row until someone views it, found again
by asking for the effect afresh when its reference has expired.

### 4.13 Communities

A community groups supergroups, channels and chats with bots. The
service lines `chat_join_from_community` and `chat_added_to_community`
keep its id as `community_id` (section 4.2.3); a community id is not a
chat id. TDLib has no `getCommunity`: a community's data arrives in
`updateCommunity`, which TDLib sends before it hands out the id, and
its chats and full photo in its full info (`getCommunityFullInfo`,
`updateCommunityFullInfo`). Migration 000049 adds two tables:

    telegram_communities (migration 000049)
      id BIGINT PRIMARY KEY (TDLib community id),
      name VARCHAR(255), have_access TINYINT(1),
      photo_file_id (FK telegram_files, ON DELETE SET NULL),
      administrator_count INT NULL, date BIGINT NULL,
      created_at, updated_at

    telegram_community_chats (migration 000049)
      community_id (FK telegram_communities, ON DELETE CASCADE),
      chat_id BIGINT, is_hidden TINYINT(1),
      can_view_history TINYINT(1), created_at, updated_at,
      PRIMARY KEY (community_id, chat_id), KEY (chat_id)

- Every `updateCommunity` upserts the row: name, `have_access` (false:
  only this row is known, and TDLib takes the id in no method), `date`
  (Unix seconds: when the account joined, else when the community was
  created) and the photo. The photo is `chatPhotoInfo`'s big file
  (640x640), else its small one, recorded without its bytes like the
  photo of a chat the account is not in.
- A full info sets the photo to the full photo's "c" size, which is
  the same Telegram file as `chatPhotoInfo`'s big one, sets
  `administrator_count` (NULL unless the account administers the
  community: TDLib reports 0 then) and replaces the community's chat
  list; chats it no longer lists are deleted, except that an empty
  list (nothing visible right now) keeps the stored ones. A full info
  that comes before its community makes a nameless row.
- `telegram_community_chats` has no foreign key to the chat tables: a
  community's chats need not be stored.
- When a logged message (either service line) or a supergroup's or
  bot's full info (`community_id`) names an accessible community, the
  daemon writes its row again from the last `updateCommunity` and, if
  the row has no chats yet, asks `getCommunityFullInfo`, at a fixed
  pace, once per run when it succeeds and not again within the hour
  after it failed.
- A community's photo whose file reference has expired is found again
  by asking for its full info afresh (storage-cache.md section 8).

There is no internal API route for communities: the rows are filled
as the lines are logged. The web resolves a line's community from
these tables (msg-communities).

## 5. Web impact (Go port)

Done in the Go service (web/docs/message-kinds.md); the Drogon web
shows the new kinds like `unknown`, as it already fell back to.

- The search API's `content_type` field takes the new values, and
  `tdlib_type` and `service_type` are filters within one chat
  (`web/docs/search-api.md`).
- The chat window and the message page carry each kind's detail from
  `content_extra`, the message-files table and the poll tables, a
  service message's action with the people it names (its text rebuilt
  for the viewer), the forum topic, the pinned state and the
  interaction totals; every user and chat named goes through the
  request scope.
- Topics: a forum group's chat page lists its topics, and the page and
  the chat window take a topic filter.
- The MCP exposure allowlist decides separately whether the new
  columns are exposed.

## 6. Settled questions

The questions left open by the first draft were settled on 2026-11-18:

1. Raw TDLib JSON is stored only for classes without a curated
   mapping, not for every message
   ([2026-11-18-msg-raw-json-unmapped-only](decisions/2026-11-18-msg-raw-json-unmapped-only.md)).
2. Views, forwards, replies and reactions are stored as latest totals,
   without history (section 4.7).
3. Shared and replied stories are kept as references only; stories are
   not fetched or archived
   ([2026-11-18-msg-stories-reference-only](decisions/2026-11-18-msg-stories-reference-only.md)).
4. The repair pass covers `unknown` and service rows only (section
   4.9).
