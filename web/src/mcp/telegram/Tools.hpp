// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_MCP_TELEGRAM_TOOLS_HPP
#define TGLOGGERD_WEB_MCP_TELEGRAM_TOOLS_HPP

#include <drogon/orm/DbClient.h>
#include <gwmcp/ToolRegistry.hpp>

/*
 * The Telegram half of the MCP tool set.
 *
 * Kept in its own directory so a Discord half can be added beside it without
 * either knowing about the other: registration is one call per platform, and
 * this file is the only thing that knows Telegram's schema.
 *
 * EXPOSURE. Every query here is gated to groups listed in
 * telegram_public_groups. The gate lives in one helper used by every query
 * rather than being repeated per tool, because forgetting it in a single code
 * path is the entire failure mode. telegram_private_messages -- direct
 * messages -- is never referenced by any file in this directory, which is a
 * grep-checkable invariant, not a convention.
 */
namespace tgweb::mcp::telegram {

/*
 * Register the Telegram tools. `db` is the read-only client; handlers use its
 * BLOCKING API, so they must be dispatched on a thread where blocking is
 * acceptable -- see McpController, which runs them on a dedicated loop rather
 * than on an HTTP thread.
 */
void registerTools(gwmcp::ToolRegistry &registry, drogon::orm::DbClientPtr db);

} /* namespace tgweb::mcp::telegram */

#endif /* TGLOGGERD_WEB_MCP_TELEGRAM_TOOLS_HPP */
