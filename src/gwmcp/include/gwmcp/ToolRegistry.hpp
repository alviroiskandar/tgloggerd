// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef GWMCP__TOOL_REGISTRY_HPP
#define GWMCP__TOOL_REGISTRY_HPP

#include <map>
#include <string>
#include <vector>

#include "Json.hpp"
#include "Tool.hpp"

namespace gwmcp {

/*
 * The set of tools a server exposes.
 *
 * Populate it once at startup, then treat it as immutable: lookups happen on
 * request threads with no locking, so mutating a live registry races. If tools
 * ever need to change at runtime, build a new registry and swap the Server.
 *
 * Ordering is by name (std::map), so tools/list is stable across restarts --
 * which keeps client-side caches and diffs sane.
 */
class ToolRegistry {
public:
	/*
	 * Register a tool. Returns false if the name is empty, the handler is
	 * unset, or the name is already taken; the registry is unchanged in
	 * that case. Callers that consider a duplicate a programming error
	 * should assert on the result.
	 */
	bool add(Tool tool);

	bool has(const std::string &name) const;

	/* nullptr when absent. Valid until the registry is modified. */
	const Tool *find(const std::string &name) const;

	size_t size(void) const { return tools_.size(); }

	/* Every tool as the JSON array tools/list returns, name-ordered. */
	Json listJson(void) const;

	std::vector<std::string> names(void) const;

private:
	std::map<std::string, Tool> tools_;
};

} /* namespace gwmcp */

#endif /* #ifndef GWMCP__TOOL_REGISTRY_HPP */
