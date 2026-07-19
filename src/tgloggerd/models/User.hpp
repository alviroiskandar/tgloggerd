// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#ifndef TGLOGGERD__MODELS__USER_HPP
#define TGLOGGERD__MODELS__USER_HPP

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace tgloggerd {
namespace models {

/*
 * Mirrors td_api::UserType.
 */
enum class UserType {
	Regular,
	Deleted,
	Bot,
	Unknown,
};

/*
 * A snapshot of a Telegram user (td_api::user), as stored in the users
 * and user_usernames tables. Only publicly meaningful, account
 * independent attributes are kept. Uses only plain types so it can be
 * shared between the TDLib and database layers.
 */
struct User {
	int64_t		id = 0;
	std::string	first_name;
	std::string	last_name;
	std::string	phone_number;
	UserType	type = UserType::Unknown;

	/* files.id of the current profile photo; resolved after download. */
	std::optional<uint64_t>	profile_photo_file_id;

	int32_t		accent_color_id = 0;
	int64_t		background_custom_emoji_id = 0;
	int32_t		profile_accent_color_id = -1;
	int64_t		profile_background_custom_emoji_id = 0;

	std::optional<int64_t>	emoji_status_custom_emoji_id;
	std::optional<int64_t>	emoji_status_expiration_date;

	bool		is_verified = false;
	bool		is_scam = false;
	bool		is_fake = false;

	bool		is_premium = false;
	bool		is_support = false;

	std::string	restriction_reason;
	bool		has_sensitive_content = false;

	bool		restricts_new_chats = false;
	int64_t		paid_message_star_count = 0;

	std::string	language_code;

	/* td_api::usernames */
	std::vector<std::string>	active_usernames;
	std::vector<std::string>	disabled_usernames;
	std::vector<std::string>	collectible_usernames;
};

/*
 * Extra user attributes from td_api::userFullInfo, which is fetched
 * separately from the user object. Applied onto the existing users row;
 * bio changes are tracked in user_hist_bio.
 */
struct UserFullInfo {
	int64_t		user_id = 0;
	std::string	bio;

	/* td_api::birthdate; nullopt components when unset/hidden. */
	std::optional<int32_t>	birthday_day;
	std::optional<int32_t>	birthday_month;
	std::optional<int32_t>	birthday_year;

	/* Linked personal chat id; 0 if none. */
	int64_t		personal_chat_id = 0;
};

} /* namespace models */
} /* namespace tgloggerd */

#endif /* #ifndef TGLOGGERD__MODELS__USER_HPP */
