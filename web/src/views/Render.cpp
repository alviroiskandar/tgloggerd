// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "views/Render.hpp"

#include "auth/FileToken.hpp"

#include <inja/inja.hpp>

#include <unordered_map>

namespace tgweb::views {

namespace {

std::string g_templateDir;
std::string g_appName;

/*
 * inja::Environment is not shared across threads; each event-loop thread keeps
 * its own environment and a cache of parsed templates (parsing reads the file
 * from disk, so we do it once per template per thread).
 */
std::string renderThreadLocal(const std::string &name,
			      const nlohmann::json &data)
{
	thread_local inja::Environment env(g_templateDir + "/");
	thread_local std::unordered_map<std::string, inja::Template> cache;
	thread_local bool configured = false;

	if (!configured) {
		/* {{ media(id) }} -> the public, opaque URL for a stored file.
		 * The id is encrypted so the templates never expose a raw,
		 * enumerable file id. Returns only [/files] + hex, so it needs
		 * no escaping. */
		env.add_callback("media", 1, [](inja::Arguments &args)
				 -> nlohmann::json {
			const nlohmann::json *a = args.at(0);
			if (!a->is_number())
				return std::string();
			uint64_t id = a->get<uint64_t>();
			return std::string("/files/") +
			       auth::filetoken::encrypt(id);
		});
		configured = true;
	}

	auto it = cache.find(name);
	if (it == cache.end())
		it = cache.emplace(name, env.parse_template(name)).first;

	return env.render(it->second, data);
}

} /* namespace */

void Render::init(const std::string &templateDir, const std::string &appName)
{
	g_templateDir = templateDir;
	g_appName = appName;
}

std::string Render::esc(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		switch (c) {
		case '&':  out += "&amp;";  break;
		case '<':  out += "&lt;";   break;
		case '>':  out += "&gt;";   break;
		case '"':  out += "&quot;"; break;
		case '\'': out += "&#39;";  break;
		default:   out += c;        break;
		}
	}
	return out;
}

std::string Render::escMultiline(const std::string &s)
{
	/* Escape first (leaves \n and \r untouched), then turn line breaks into
	 * <br>; a lone \r or the \r of a \r\n pair is dropped. */
	std::string e = esc(s);
	std::string out;
	out.reserve(e.size());
	for (char c : e) {
		if (c == '\n')
			out += "<br>";
		else if (c != '\r')
			out += c;
	}
	return out;
}

std::string Render::renderNamed(const std::string &name,
				const nlohmann::json &data)
{
	return renderThreadLocal(name, data);
}

std::string Render::fragment(const std::string &templateName,
			     const nlohmann::json &data)
{
	return renderNamed(templateName, data);
}

std::string Render::page(const std::string &contentTemplate,
			 nlohmann::json data)
{
	data["app"] = g_appName;
	if (!data.contains("title"))
		data["title"] = g_appName;

	std::string content = renderNamed(contentTemplate, data);

	nlohmann::json layout;
	layout["app"] = g_appName;
	layout["title"] = data["title"];
	layout["content"] = content;
	layout["logged_in"] = data.contains("username");
	if (data.contains("username"))
		layout["username"] = data["username"];
	if (data.contains("csrf"))
		layout["csrf"] = data["csrf"];

	return renderNamed("layout.html", layout);
}

} /* namespace tgweb::views */
