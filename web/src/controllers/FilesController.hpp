// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_FILESCONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_FILESCONTROLLER_HPP

#include <drogon/HttpController.h>

namespace tgweb::controllers {

/* Browse the stored (de-duplicated) telegram_files. Requires a session. */
class FilesController : public drogon::HttpController<FilesController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(FilesController::list, "/files", drogon::Get,
		      "tgweb::auth::AuthFilter");
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_FILESCONTROLLER_HPP */
