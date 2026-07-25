-- Index (content_type, date) on the message tables so filtering by content
-- type (e.g. only photos) is fast: the COUNT and the date-ordered page both
-- become index range scans instead of scanning millions of rows. Leading
-- content_type serves the equality filter; the trailing date serves the
-- default ORDER BY date. A plain secondary-index add is online (INPLACE,
-- LOCK=NONE by default), so this needs no downtime.
ALTER TABLE telegram_group_messages   ADD INDEX idx_group_messages_ctype_date (content_type, date);
ALTER TABLE telegram_private_messages ADD INDEX idx_private_messages_ctype_date (content_type, date);
