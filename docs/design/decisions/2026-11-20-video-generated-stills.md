# Decision log: 2026-11-20, Generated stills for videos

Decided by the project owner on 2026-11-20, for the video-thumbnails
item:

- Videos (and animations, video notes and video stickers) whose bytes
  are stored but which have neither a cover nor a Telegram thumbnail
  get a still generated from the stored bytes.
- Generation runs with ffmpeg in a container of its own, off every
  request path; a still is a small file of the store like a Telegram
  thumbnail, counted against the store's cap and evicted like others.
- Videos whose bytes are not stored get no generated still; they keep
  the minithumb or the type icon.
- Reason: a grid of type icons tells a reader nothing about which
  video is which, and Telegram sends no thumbnail for many uploads.
