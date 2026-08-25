ALTER TABLE telegram_private_messages
	DROP INDEX idx_private_messages_chat_edit,
	DROP INDEX idx_private_messages_chat_deleted;
ALTER TABLE telegram_group_messages
	DROP INDEX idx_group_messages_chat_edit,
	DROP INDEX idx_group_messages_chat_deleted;
