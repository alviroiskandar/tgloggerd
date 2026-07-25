// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_COMMON_HPP
#define TGLOGGERD_WEB_CONTROLLERS_COMMON_HPP

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/Utilities.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <string>

#include "auth/Csrf.hpp"
#include "auth/FileToken.hpp"
#include "auth/Session.hpp"
#include "views/Render.hpp"

namespace tgweb::controllers {

/*
 * Turn the raw file id in each search result row's image cell into an opaque
 * /files/<token> URL the browser can load (or "" when there is none). Handles
 * the scalar "photo"/"filethumb" cells (users/groups/files) and the "party"
 * object cells (message rows), whose `photo` member is a raw file id. Shared by
 * the SSR page and the JSON API so both render identical rows. Safe to call on
 * an error result (no cols/rows).
 */
inline void enrichSearchPhotos(nlohmann::json &result)
{
	if (!result.is_object() || !result.contains("cols") ||
	    !result.contains("rows"))
		return;

	auto tokenize = [](nlohmann::json &v) {
		if (v.is_number() && v.get<int64_t>() != 0)
			v = std::string("/files/") +
			    auth::filetoken::encrypt(v.get<uint64_t>());
		else
			v = std::string();
	};

	const auto &cols = result["cols"];
	for (size_t i = 0; i < cols.size(); i++) {
		std::string t = cols[i].value("type", std::string());
		bool scalar = (t == "photo" || t == "filethumb");
		bool party  = (t == "party");
		if (!scalar && !party)
			continue;
		for (auto &row : result["rows"]) {
			if (!row.is_array() || i >= row.size())
				continue;
			if (scalar)
				tokenize(row[i]);
			else if (party && row[i].is_object() &&
				 row[i].contains("photo"))
				tokenize(row[i]["photo"]);
		}
	}
}

/*
 * Context every authenticated page needs for the layout: the signed-in
 * username (escaped), a CSRF token for the sign-out form, and the admin flag.
 * Page controllers merge their own data into this object.
 */
inline nlohmann::json pageBase(const drogon::HttpRequestPtr &req)
{
	auto s = auth::session::current(req);
	nlohmann::json j;
	j["username"] = views::Render::esc(s ? s->username : std::string());
	j["csrf"] = auth::csrf::forSession(req);
	j["is_admin"] = s && s->role == "admin";
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
	resp->setBody(views::Render::page("status.html", data));
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
