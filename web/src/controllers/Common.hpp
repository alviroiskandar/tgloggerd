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
 * Turn the raw photo file id in each search result row (the cell of the "photo"
 * column) into an opaque /files/<token> URL the browser can load, or "" when
 * there is no photo. Shared by the SSR page and the JSON API so both render
 * identical rows. Safe to call on an error result (no cols/rows).
 */
inline void enrichSearchPhotos(nlohmann::json &result)
{
	if (!result.is_object() || !result.contains("cols") ||
	    !result.contains("rows"))
		return;
	int photoIdx = -1;
	const auto &cols = result["cols"];
	for (size_t i = 0; i < cols.size(); i++) {
		if (cols[i].value("type", std::string()) == "photo") {
			photoIdx = (int)i;
			break;
		}
	}
	if (photoIdx < 0)
		return;
	for (auto &row : result["rows"]) {
		if (!row.is_array() || photoIdx >= (int)row.size())
			continue;
		auto &cell = row[photoIdx];
		if (cell.is_number() && cell.get<int64_t>() != 0)
			cell = std::string("/files/") +
			       auth::filetoken::encrypt(cell.get<uint64_t>());
		else
			cell = std::string();
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

/*
 * Wire up the shared search box for a listing page. Reads "q" and "field",
 * validates the field against `fields` (an array of {value, label} options;
 * the first is the default when the request's field is missing or unknown),
 * and populates data["search"] = {action, q, placeholder, fields, field} for
 * the search.html component (q is HTML-escaped for the input value). Also sets
 * data["q_url"] / data["field_url"] with the URL-encoded values for carrying
 * the active search through pager links. Returns the raw query, and writes the
 * validated field to `field`, to hand to the DAO.
 */
inline std::string applySearch(nlohmann::json &data,
			       const drogon::HttpRequestPtr &req,
			       const std::string &action,
			       const std::string &placeholder,
			       nlohmann::json fields, std::string &field)
{
	std::string q = req->getParameter("q");

	std::string requested = req->getParameter("field");
	field = fields.empty() ? std::string("all")
			       : fields[0]["value"].get<std::string>();
	for (const auto &f : fields) {
		if (f["value"] == requested) {
			field = requested;
			break;
		}
	}

	nlohmann::json s;
	s["action"]      = action;
	s["q"]           = views::Render::esc(q);
	s["placeholder"] = placeholder;
	s["field"]       = field;
	s["fields"]      = std::move(fields);
	data["search"]   = std::move(s);
	data["q_url"]     = drogon::utils::urlEncodeComponent(q);
	data["field_url"] = drogon::utils::urlEncodeComponent(field);
	return q;
}

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_COMMON_HPP */
