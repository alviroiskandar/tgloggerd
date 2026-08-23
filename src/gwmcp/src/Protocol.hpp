// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef GWMCP__PROTOCOL_HPP
#define GWMCP__PROTOCOL_HPP

/* INTERNAL. Not installed; consumers see only include/gwmcp/. */
#include <string>

#include <gwmcp/Json.hpp>
#include <gwmcp/Server.hpp> /* makeResult/makeError are declared public */

namespace gwmcp {

/* True when the message carries no id and therefore expects no reply. */
bool isNotification(const Json &message);

/* The id to echo, normalised to null when absent or of an illegal type. */
Json messageId(const Json &message);

/* Check the JSON-RPC envelope. False + `err` when malformed. */
bool validateEnvelope(const Json &message, std::string &err);

/* params as an object; an empty object when absent or positional. */
Json paramsObject(const Json &message);

} /* namespace gwmcp */

#endif /* #ifndef GWMCP__PROTOCOL_HPP */
