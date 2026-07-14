// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_MEDIACONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_MEDIACONTROLLER_HPP

#include <drogon/HttpController.h>

#include <string>

namespace tgweb::controllers {

/*
 * Serve a stored file by id. Requires a session. The file is located by
 * resolving files.id -> SHA-256 -> the daemon's 5-level hex fan-out path;
 * the path is derived entirely from the database, never from the request.
 */
class MediaController : public drogon::HttpController<MediaController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(MediaController::serve, "/media/{1}", drogon::Get,
		      "tgweb::auth::AuthFilter");
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr> serve(drogon::HttpRequestPtr req,
						    std::string id);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_MEDIACONTROLLER_HPP */
