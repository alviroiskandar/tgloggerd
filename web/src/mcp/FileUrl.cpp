// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "mcp/FileUrl.hpp"

#include "auth/FileToken.hpp"

#include <cstdlib>

namespace tgweb::mcp::fileurl {

const std::string &baseUrl(void)
{
	/* Read once: the environment does not change under a running process,
	 * and this is called per row of a result set. */
	static const std::string cached = [] {
		const char *v = getenv("WEB_PUBLIC_URL");
		if (!v || !*v)
			v = getenv("TG_DISCORD_PUBLIC_URL");
		if (!v || !*v)
			return std::string();

		std::string s = v;
		while (!s.empty() && (s.back() == '/' || s.back() == ' '))
			s.pop_back();
		return s;
	}();
	return cached;
}

std::string forFile(uint64_t fileId)
{
	if (!fileId)
		return std::string();

	const std::string &base = baseUrl();
	if (base.empty())
		return std::string();

	return base + "/files/" + auth::filetoken::encrypt(fileId);
}

} /* namespace tgweb::mcp::fileurl */
