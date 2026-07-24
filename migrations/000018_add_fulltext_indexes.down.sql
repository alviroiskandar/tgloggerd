ALTER TABLE telegram_group_messages   DROP INDEX ftx_group_messages_text;
ALTER TABLE telegram_private_messages DROP INDEX ftx_private_messages_text;
ALTER TABLE telegram_groups           DROP INDEX ftx_groups_description;
ALTER TABLE telegram_user_extra_info  DROP INDEX ftx_user_extra_info_bio;
