// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef GWMCP__JSON_HPP
#define GWMCP__JSON_HPP

#include <nlohmann/json.hpp>

namespace gwmcp {

/*
 * The JSON type, named once so every other header agrees.
 *
 * Unlike gwdiscord -- which hides its JSON library behind plain structs --
 * gwmcp exposes the type deliberately. MCP is JSON to its core: a tool's
 * inputSchema IS JSON Schema, its arguments are arbitrary caller-supplied
 * JSON, and its result is arbitrary JSON. Hiding that behind an abstraction
 * would mean reinventing a JSON value type for no gain, so the dependency is
 * admitted at the boundary instead of pretending it is not there.
 */
using Json = nlohmann::json;

} /* namespace gwmcp */

#endif /* #ifndef GWMCP__JSON_HPP */
