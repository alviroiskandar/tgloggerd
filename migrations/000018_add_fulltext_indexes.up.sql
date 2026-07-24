-- Full-text indexes for free-text search: user bios, group descriptions, and
-- private/group message text. InnoDB FULLTEXT (default parser, whitespace
-- tokenized, innodb_ft_min_token_size words). The first FULLTEXT index on a
-- table rebuilds it to add the hidden FTS_DOC_ID column, so on the large
-- message tables this is a slow, table-locking operation -- run with the daemon
-- stopped.
ALTER TABLE telegram_user_extra_info  ADD FULLTEXT INDEX ftx_user_extra_info_bio (bio);
ALTER TABLE telegram_groups           ADD FULLTEXT INDEX ftx_groups_description (description);
ALTER TABLE telegram_private_messages ADD FULLTEXT INDEX ftx_private_messages_text (text);
ALTER TABLE telegram_group_messages   ADD FULLTEXT INDEX ftx_group_messages_text (text);
