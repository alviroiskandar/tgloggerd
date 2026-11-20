# profile-photo-videos

Store and show the video of an animated profile photo: users' and
groups' photo carousels play it, as Telegram clients do, instead of
showing only the still.

## Why

Telegram profile photos can be short videos. The daemon stores only
the still of each recorded photo, so the photo carousels of users and
groups show a still where the account showed a moving picture, and the
video is lost once the photo changes.

## Scope

- Daemon: a migration adds `video_file_id` (NULL-able, FK to
  telegram_files) to telegram_user_hist_profile_photo and
  telegram_group_hist_photo. When a recorded photo is animated, the
  daemon stores its MPEG-4 video (the big animation, else the small
  one) through the usual store path, under the store's cap, and sets
  the column. Old rows are left without a video.
  Done: migration 000051; the video is recorded metadata-only like the
  still and fetched on first view (docs/internal-api.md, the photos
  endpoints).
- Go (webapi): the items of GET /api/users/{id}/photos and GET
  /api/groups/{id}/photos gain `video_url` (the /files/<token> URL of
  the video, or null when there is none or it cannot be served) and
  `video_file_id` (int or null). OpenAPI and web/docs follow.
- React: the carousels play the video muted and looped when the item
  has one, with the still as its poster.

## Depends on

Nothing.

## Done when

- A user or group that sets an animated photo has it stored with its
  video, and the carousel plays it; items without a video are
  unchanged.
- Go tests cover video_url present, absent and unservable; React tests
  cover the carousel with and without a video.
