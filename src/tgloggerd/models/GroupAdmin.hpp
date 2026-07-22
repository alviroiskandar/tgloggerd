// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#ifndef TGLOGGERD__MODELS__GROUP_ADMIN_HPP
#define TGLOGGERD__MODELS__GROUP_ADMIN_HPP

#include <string>
#include <vector>
#include <cstdint>

namespace tgloggerd {
namespace models {

/*
 * A single group administrator and their privileges, from a
 * td_api::chatMember whose status is chatMemberStatusCreator or
 * chatMemberStatusAdministrator. For a creator, is_owner is true and the
 * can_* rights are all synthesized to true (the owner holds every right).
 */
struct GroupAdmin {
	int64_t		user_id = 0;
	bool		is_owner = false;	/* status == creator */
	std::string	custom_title;		/* chatMember.tag_ */
	int64_t		inviter_user_id = 0;	/* 0 if unknown */
	int64_t		joined_date = 0;	/* chatMember.joined_chat_date */

	/* td_api::chatAdministratorRights (17 flags). */
	bool		can_manage_chat = false;
	bool		can_change_info = false;
	bool		can_post_messages = false;
	bool		can_edit_messages = false;
	bool		can_delete_messages = false;
	bool		can_invite_users = false;
	bool		can_restrict_members = false;
	bool		can_pin_messages = false;
	bool		can_manage_topics = false;
	bool		can_promote_members = false;
	bool		can_manage_video_chats = false;
	bool		can_post_stories = false;
	bool		can_edit_stories = false;
	bool		can_delete_stories = false;
	bool		can_manage_direct_messages = false;
	bool		can_manage_tags = false;
	bool		is_anonymous = false;
};

/*
 * The full administrator set of one group, as returned by a single
 * getSupergroupMembers(administrators) call or basicGroupFullInfo. Synced
 * atomically against the stored set so add/remove/privilege-change events
 * are recorded. group_id is the chat_id (a valid telegram_groups.id).
 */
struct GroupAdminList {
	int64_t			group_id = 0;
	std::vector<GroupAdmin>	admins;
};

} /* namespace models */
} /* namespace tgloggerd */

#endif /* #ifndef TGLOGGERD__MODELS__GROUP_ADMIN_HPP */
