// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef GWDISCORD__EVENT_PARSE_HPP
#define GWDISCORD__EVENT_PARSE_HPP

/*
 * INTERNAL. The JSON library is an implementation detail: it appears in this
 * header and in the .cpp files that include it, never in a public header, so
 * consumers of gwdiscord do not compile against jsoncpp.
 */
#include <json/json.h>

#include <gwdiscord/Events.hpp>

#include <string>

namespace gwdiscord {

/* Discord sends ids as JSON strings; absent/null yields 0. */
Snowflake	parse_snowflake(const Json::Value &v);
std::string	json_str(const Json::Value &v, const char *key);

User		parse_user(const Json::Value &v);
Attachment	parse_attachment(const Json::Value &v);
Message		parse_message(const Json::Value &v);
MessageDelete	parse_message_delete(const Json::Value &v);
Ready		parse_ready(const Json::Value &v);

/* Parse a whole gateway frame. Returns false if it is not valid JSON. */
bool parse_json(const std::string &raw, Json::Value &out, std::string *err);

/* Serialise compactly (no indentation, no trailing newline). */
std::string dump_json(const Json::Value &v);

} /* namespace gwdiscord */

#endif /* #ifndef GWDISCORD__EVENT_PARSE_HPP */
