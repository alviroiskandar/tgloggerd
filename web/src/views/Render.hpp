// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_VIEWS_RENDER_HPP
#define TGLOGGERD_WEB_VIEWS_RENDER_HPP

#include <nlohmann/json.hpp>

#include <string>

namespace tgweb::views {

/*
 * Server-side HTML rendering over inja templates.
 *
 * Escaping is a data-layer invariant, not a template concern: inja does NOT
 * auto-escape, so every attacker-controlled string (message text, names,
 * titles, file names, ...) MUST be passed through Render::esc() when building
 * the template context. Templates then interpolate the already-safe values.
 */
class Render {
public:
	/*
	 * Configure the template directory and application/brand name. Call once
	 * at startup before serving requests.
	 */
	static void init(const std::string &templateDir,
			 const std::string &appName);

	/* HTML-escape untrusted text (&, <, >, ", '). THE escaping invariant. */
	static std::string esc(const std::string &s);

	/*
	 * Like esc(), but also turns line breaks into <br> so multi-line text
	 * (bios, descriptions, ...) keeps its line structure in HTML. The text is
	 * fully escaped first, so the only markup in the result is the <br> tags.
	 */
	static std::string escMultiline(const std::string &s);

	/*
	 * Render a content template and wrap it in the base layout. The layout
	 * shows the top bar with a sign-out form when data contains "username";
	 * that data must then also carry a "csrf" token.
	 */
	static std::string page(const std::string &contentTemplate,
				nlohmann::json data);

	/* Render a template without the layout wrapper. */
	static std::string fragment(const std::string &templateName,
				    const nlohmann::json &data);

private:
	static std::string renderNamed(const std::string &name,
				       const nlohmann::json &data);
};

} /* namespace tgweb::views */

#endif /* TGLOGGERD_WEB_VIEWS_RENDER_HPP */
