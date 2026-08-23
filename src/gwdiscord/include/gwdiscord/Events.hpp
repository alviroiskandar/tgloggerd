// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef GWDISCORD__EVENTS_HPP
#define GWDISCORD__EVENTS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gwdiscord {

/*
 * Discord serialises every snowflake id as a JSON *string*, because the values
 * exceed what a double can represent exactly. We parse them to uint64_t once,
 * here, so callers never handle stringly-typed ids. 0 means "absent".
 */
using Snowflake = uint64_t;

/* Creation time of any snowflake, in milliseconds since the Unix epoch. */
constexpr uint64_t DISCORD_EPOCH_MS = 1420070400000ULL;
inline uint64_t snowflake_created_ms(Snowflake id)
{
	return (id >> 22) + DISCORD_EPOCH_MS;
}

struct User {
	Snowflake	id = 0;
	std::string	username;
	std::string	global_name;	/* display name; may be empty */
	std::string	discriminator;	/* "0" for migrated accounts */
	std::string	avatar;		/* avatar hash; may be empty */
	bool		bot = false;
};

struct Attachment {
	Snowflake	id = 0;
	std::string	filename;
	std::string	content_type;	/* may be empty */
	uint64_t	size = 0;
	/*
	 * CDN URLs are HMAC-signed and EXPIRE (the ex/is/hm query params).
	 * Fetch the bytes promptly; the only supported refresh is re-reading
	 * the message from the REST API.
	 */
	std::string	url;
	std::string	proxy_url;
	int		width = 0;	/* images only; 0 if not applicable */
	int		height = 0;
};

/* Where a reply points. Present only when the message is a reply. */
struct MessageReference {
	Snowflake	message_id = 0;
	Snowflake	channel_id = 0;
	Snowflake	guild_id = 0;
};

struct Message {
	Snowflake	id = 0;
	Snowflake	channel_id = 0;
	Snowflake	guild_id = 0;	/* 0 for a DM */
	User		author;
	/*
	 * Non-zero when the message was posted by a webhook rather than by a
	 * user or a bot. This is the discriminator a bridge uses to avoid
	 * re-forwarding its own traffic: note that a BOT's own message has
	 * author.bot == true but webhook_id == 0, so a bridge that also posts
	 * as a bot must additionally skip its own author id.
	 */
	Snowflake	webhook_id = 0;
	std::string	content;	/* empty without the MESSAGE_CONTENT intent */
	std::string	timestamp;	/* ISO-8601, as sent */
	std::string	edited_timestamp; /* empty when never edited */
	std::vector<Attachment>		attachments;
	std::optional<MessageReference>	reference;

	bool from_webhook(void) const { return webhook_id != 0; }
	bool is_reply(void) const { return reference.has_value(); }
};

/*
 * MESSAGE_DELETE carries ONLY these ids -- never the content. A consumer that
 * wants to render what was deleted must have persisted it at ingest time.
 */
struct MessageDelete {
	Snowflake	id = 0;
	Snowflake	channel_id = 0;
	Snowflake	guild_id = 0;
};

struct Ready {
	User		user;
	std::string	session_id;
	std::string	resume_gateway_url;
};

} /* namespace gwdiscord */

#endif /* #ifndef GWDISCORD__EVENTS_HPP */
