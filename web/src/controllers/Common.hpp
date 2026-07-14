// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_COMMON_HPP
#define TGLOGGERD_WEB_CONTROLLERS_COMMON_HPP

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <string>

#include "auth/Csrf.hpp"
#include "auth/Session.hpp"
#include "views/Render.hpp"

namespace tgweb::controllers {

/*
 * Context every authenticated page needs for the layout: the signed-in
 * username (escaped), a CSRF token for the sign-out form, and the admin flag.
 * Page controllers merge their own data into this object.
 */
inline nlohmann::json pageBase(const drogon::HttpRequestPtr &req)
{
	const auto &s = req->session();
	nlohmann::json j;
	j["username"] = views::Render::esc(
		s->getOptional<std::string>(auth::session::kUsername).value_or(""));
	j["csrf"] = auth::csrf::ensure(s);
	j["is_admin"] = auth::session::isAdmin(s);
	return j;
}

/* Build a text/html 200 response from a rendered body. */
inline drogon::HttpResponsePtr htmlPage(const std::string &body)
{
	auto resp = drogon::HttpResponse::newHttpResponse();
	resp->setContentTypeCode(drogon::CT_TEXT_HTML);
	resp->setBody(body);
	return resp;
}

/* Render a small status page (e.g. 404) through the layout. */
inline drogon::HttpResponsePtr renderStatus(const drogon::HttpRequestPtr &req,
					    drogon::HttpStatusCode code,
					    const std::string &heading,
					    const std::string &message)
{
	nlohmann::json data = pageBase(req);
	data["title"] = heading;
	data["heading"] = views::Render::esc(heading);
	data["message"] = views::Render::esc(message);

	auto resp = drogon::HttpResponse::newHttpResponse();
	resp->setStatusCode(code);
	resp->setContentTypeCode(drogon::CT_TEXT_HTML);
	resp->setBody(views::Render::page("message.html", data));
	return resp;
}

/* Parse a positive integer query parameter, clamped to [min, max]. */
inline int clampedIntParam(const drogon::HttpRequestPtr &req, const char *name,
			   int def, int lo, int hi)
{
	std::string v = req->getParameter(name);
	if (v.empty())
		return def;
	int n = atoi(v.c_str());
	return std::clamp(n, lo, hi);
}

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_COMMON_HPP */
