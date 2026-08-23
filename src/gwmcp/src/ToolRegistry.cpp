// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include <gwmcp/ToolRegistry.hpp>

#include <utility>

namespace gwmcp {

bool ToolRegistry::add(Tool tool)
{
	if (tool.name.empty() || !tool.handler)
		return false;
	if (tools_.count(tool.name))
		return false;

	const std::string key = tool.name;
	tools_.emplace(key, std::move(tool));
	return true;
}

bool ToolRegistry::has(const std::string &name) const
{
	return tools_.count(name) != 0;
}

const Tool *ToolRegistry::find(const std::string &name) const
{
	auto it = tools_.find(name);
	return it == tools_.end() ? nullptr : &it->second;
}

std::vector<std::string> ToolRegistry::names(void) const
{
	std::vector<std::string> out;
	out.reserve(tools_.size());
	for (const auto &kv : tools_)
		out.push_back(kv.first);
	return out;
}

Json ToolRegistry::listJson(void) const
{
	Json arr = Json::array();

	for (const auto &kv : tools_) {
		const Tool &t = kv.second;
		Json j;
		j["name"] = t.name;
		if (!t.title.empty())
			j["title"] = t.title;
		if (!t.description.empty())
			j["description"] = t.description;

		/*
		 * inputSchema is required by the spec and must be an object
		 * schema. A tool that declared none still has to advertise
		 * something well-formed, so fall back to "takes no arguments"
		 * rather than emitting null and letting the client choke.
		 */
		if (t.inputSchema.is_object())
			j["inputSchema"] = t.inputSchema;
		else
			j["inputSchema"] = Json{ { "type", "object" },
						 { "properties", Json::object() } };

		if (t.outputSchema.is_object())
			j["outputSchema"] = t.outputSchema;

		j["annotations"] = Json{ { "readOnlyHint", t.readOnlyHint } };
		arr.push_back(std::move(j));
	}
	return arr;
}

} /* namespace gwmcp */
