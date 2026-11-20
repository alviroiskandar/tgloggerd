# video-thumbnails

Show a thumbnail for every video before it is played, wherever the web
shows one: a still frame instead of a generic film icon. The daemon,
the Go service and the React application do their part (below); what
is left is the videos Telegram sends no picture for, and the rows
logged before.

## Why

Videos, round video notes, GIF-like animations and video stickers
(.webm) show a type icon in the files grid and the file preview, and
only sometimes a cover in the chat, so a reader cannot tell them apart
or see what they are without playing each.

## Scope

Done: the daemon stores the still thumbnail TDLib offers with a video,
an animation, a video note or a video sticker (JPEG, PNG or WebP;
telegram_message_files role `thumbnail`, file_type `thumbnail`) for
new and edited messages; the Go service gives it as ChatMedia's (and
the message page's) `poster` and `thumb` after a stored cover, and as
FileRow's `thumb` with the minithumb beside it (FileRow `minithumb`);
web/docs/media.md, "Video stills"; and the React application draws
the stills and blurred previews in the chat, on the message page and
in the files grid and preview. Left:

- Old rows: messages logged before the daemon stored thumbnails have
  none. A one-off pass could ask TDLib for each such message again and
  store its thumbnail through the fetch path, under the store's cap.
  Done: the opt-in pass `TG_THUMBNAILS_ON_START` (`dry` counts)
  re-reads them through the normal store path (message-types.md
  section 4.9); what is left is to run it, and the videos Telegram
  sends no still for stay for the frames below.
- Frames: a still taken from the stored bytes (ffmpeg, in a container
  of its own, never in the web service's request path) for videos
  with neither a cover nor a Telegram thumbnail, as decided in
  [2026-11-20-video-generated-stills.md](../design/decisions/2026-11-20-video-generated-stills.md).
  Done: the stills service (docker-compose.yml, `docker/stills/`) and
  the daemon's opt-in pass `TG_GENERATE_STILLS` (`dry` counts), which
  stores each still as the video's messages' `thumbnail`
  (message-types.md section 4.3, "Generated stills"; format in
  [2026-11-21-video-stills-format.md](../design/decisions/2026-11-21-video-stills-format.md));
  what is left is to start the service and turn the pass on, and for
  the Go service not to offer a fetch of an evicted generated still
  (`tg_file_id` `generated:`, answered 410 `generated`).
- Search and list results that show media: done. The files list and
  a group's Files tab give every video its still, a video that only
  plays a role in a message (a rich block, an animated chat photo, a
  link preview's medium) too, from the picture the chat shows it with
  (web/docs/media.md, "Video stills"). The other lists show no media
  picture: the message lists and the search API's message answers
  give a text snippet, the dashboard and MCP answers none. The search
  API's files answer keeps its `thumb` cell as the row's own
  `/files/<token>` link, as the Drogon answer it ports does.
- Not in scope: animated (.tgs) stickers, which the chat already plays
  with Lottie; a thumbnail for them is a separate question.

## Design notes

- Sources, in order of preference: a stored cover, the stored Telegram
  thumbnail, the minithumb (a tiny blurred JPEG) as a placeholder, and,
  only if decided, a generated frame. Thumbnails are small files of
  their own in the store, counted against its cap and evicted like
  others.
- FileRow.thumb stays an image, never the video itself, and only of
  stored bytes, so a list never fetches.

## Depends on

Nothing.

## Done when

- Old rows have their thumbnails, or the project owner has decided
  they need none.
- Stored videos without a cover or a Telegram thumbnail have a
  generated still.
