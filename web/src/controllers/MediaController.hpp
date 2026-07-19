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
 * Serve a stored file at /files/<token>. The token is the encrypted file id
 * (see auth::filetoken), so this route is public -- no session is required and
 * the file id is never exposed in the clear or enumerable. The on-disk path is
 * resolved from files.id -> SHA-256 -> the daemon's 5-level hex fan-out, all
 * derived from the database, never from the request.
 */
class MediaController : public drogon::HttpController<MediaController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(MediaController::serve, "/files/{1}", drogon::Get);
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr> serve(drogon::HttpRequestPtr req,
						    std::string token);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_MEDIACONTROLLER_HPP */
