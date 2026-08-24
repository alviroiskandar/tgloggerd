// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_MCP_FILEURL_HPP
#define TGLOGGERD_WEB_MCP_FILEURL_HPP

#include <cstdint>
#include <string>

/*
 * Absolute, publicly fetchable URLs for stored files, so an MCP client can
 * actually retrieve an image rather than being handed an opaque internal id.
 *
 * These are the same /files/<token> links the web UI uses: the token is a
 * deterministic authenticated encryption of the file id under WEB_APP_KEY, so
 * it is stable, fixed-length, non-enumerable and unforgeable (see
 * auth/FileToken.hpp). The route needs no session, which is what makes the link
 * usable by a client that only holds an MCP token.
 *
 * Note what that implies: a file URL is a bearer credential for that one file
 * and is independent of the MCP token that produced it. Revoking an MCP token
 * does not invalidate links already handed out; only rotating WEB_APP_KEY does,
 * and that invalidates every session too.
 *
 * Shared rather than Telegram-specific because the Discord tools will need the
 * identical thing.
 */
namespace tgweb::mcp::fileurl {

/*
 * The configured public base URL, without a trailing slash, or "" when none is
 * set. Read from WEB_PUBLIC_URL, falling back to TG_DISCORD_PUBLIC_URL -- the
 * daemon already uses that one to build these very links for Discord, so an
 * existing deployment needs no new configuration.
 */
const std::string &baseUrl(void);

/*
 * Absolute URL for a stored file, or "" when no base URL is configured (in
 * which case callers should omit the field rather than emit a relative link an
 * MCP client has no way to resolve).
 */
std::string forFile(uint64_t fileId);

} /* namespace tgweb::mcp::fileurl */

#endif /* TGLOGGERD_WEB_MCP_FILEURL_HPP */
