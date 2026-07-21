// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/SearchController.hpp"

#include "auth/Session.hpp"
#include "controllers/Common.hpp"
#include "dao/Search.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>

namespace tgweb::controllers {

namespace {

drogon::HttpResponsePtr jsonResp(const nlohmann::json &body,
				 drogon::HttpStatusCode code)
{
	auto resp = drogon::HttpResponse::newHttpResponse();
	resp->setStatusCode(code);
	resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
	resp->setBody(body.dump());
	return resp;
}

drogon::HttpResponsePtr jsonError(const std::string &msg,
				  drogon::HttpStatusCode code)
{
	return jsonResp(nlohmann::json{ { "error", msg } }, code);
}

} /* namespace */

drogon::Task<drogon::HttpResponsePtr>
SearchController::search(drogon::HttpRequestPtr req, std::string entity)
{
	const dao::search::SearchSchema *schema = dao::search::schemaByName(entity);
	if (!schema)
		co_return jsonError("unknown search entity: '" + entity + "'",
				    drogon::k404NotFound);

	dao::search::Request sreq;
	sreq.limit  = clampedIntParam(req, "limit", 10, 1,
				      dao::search::MAX_LIMIT);
	sreq.offset = clampedIntParam(req, "offset", 0, 0,
				      dao::search::MAX_OFFSET);
	sreq.sort   = req->getParameter("sort");
	sreq.order  = req->getParameter("order");
	sreq.debug  = (req->getParameter("debug") == "1") &&
		      auth::session::isAdmin(req);

	std::string err;
	if (!dao::search::parseConditions(req->getParameter("search"),
					  sreq.conds, err))
		co_return jsonError(err, drogon::k400BadRequest);

	auto db = drogon::app().getDbClient("ro");
	nlohmann::json result = co_await dao::search::run(db, *schema,
							  std::move(sreq));

	if (result.contains("error"))
		co_return jsonError(result["error"].get<std::string>(),
				    drogon::k400BadRequest);

	enrichSearchPhotos(result);
	co_return jsonResp(result, drogon::k200OK);
}

} /* namespace tgweb::controllers */
