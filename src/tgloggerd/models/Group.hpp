// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#ifndef TGLOGGERD__MODELS__GROUP_HPP
#define TGLOGGERD__MODELS__GROUP_HPP

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace tgloggerd {
namespace models {

/*
 * Kind of group chat. In TDLib a broadcast channel is a supergroup with
 * is_channel set; it is represented here as its own type.
 */
enum class GroupType {
	BasicGroup,
	Supergroup,
	Channel,
};

/*
 * A snapshot of a Telegram group chat, assembled from the TDLib chat,
 * supergroup/basicGroup and full-info objects. Maps to the groups and
 * telegram_group_usernames tables. Uses only plain types.
 */
struct Group {
	int64_t		id = 0;
	GroupType	type = GroupType::BasicGroup;
	std::string	title;
	std::string	description;

	/* telegram_files.id of the current group photo; resolved after download. */
	std::optional<uint64_t>	photo_file_id;

	/* td_api::usernames (supergroups/channels only). */
	std::vector<std::string>	active_usernames;
	std::vector<std::string>	disabled_usernames;
	std::vector<std::string>	collectible_usernames;
};

} /* namespace models */
} /* namespace tgloggerd */

#endif /* #ifndef TGLOGGERD__MODELS__GROUP_HPP */
