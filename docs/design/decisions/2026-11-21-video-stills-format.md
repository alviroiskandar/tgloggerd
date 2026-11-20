# Decision log: 2026-11-21, The format of generated video stills

Decided by the project owner on 2026-11-21, for the video-thumbnails
item ("Frames"), following
[2026-11-20-video-generated-stills](2026-11-20-video-generated-stills.md):

- Size: a still fits 320 px on its long side, keeping the aspect
  ratio; a smaller video is never upscaled.
- Format: JPEG at the equivalent of quality 80, and PNG for video
  (.webm) stickers, whose transparency JPEG would lose. ffmpeg's MJPEG
  encoder has no quality setting of libjpeg's kind but a quantizer
  scale, `-q:v` 2 to 31; it is set to 5: the average of its luminance
  table is 20.3 there, against 23.1 for libjpeg's quality 80 table and
  17.3 for quality 85 (measured on ffmpeg 5.1, Debian 12), so 5 is the
  nearest that is not coarser than quality 80. A still of a 640x360
  test video is about 6 KB.
- Cap: a still over 200 KB is refused, and the video keeps the
  minithumb or the type icon. A JPEG of this size is far below it; the
  cap stops a noisy transparent sticker's PNG, which hardly compresses,
  from taking more room than the video it stands for.
- Frame: at 1 second, or halfway into a clip shorter than 2 seconds,
  seeking before the input (fast, to the key frame before the time,
  then decoding to it). A clip whose stated duration is past its end
  gives its first frame instead.
- Record: a still is a telegram_files row with file_type `thumbnail`,
  `tg_file_id` `generated:<sha256>`, no `tg_unique_id`, and the new
  column `generated_from_file_id` (migration 000052), a foreign key to
  the telegram_files row of the video it was taken from, NULL for
  every file that came from Telegram (and again if the video's row is
  deleted). It is linked as role `thumbnail`, position 0, to every
  live message whose main file is that video: one still per video, not
  per message. A Telegram thumbnail stored for the message later
  replaces it; a still never replaces a Telegram thumbnail.
- Reason: the grid needs a picture per video, not a poster; at this
  size and quality one still costs a few KB of the store, and a video
  file carried by many messages is decoded once.
