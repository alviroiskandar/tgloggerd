// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_DAO_MCP_HPP
#define TGLOGGERD_WEB_DAO_MCP_HPP

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

/*
 * Backing store for the MCP admin pages.
 *
 * Two schemas, two clients, deliberately not mixed:
 *
 *   telegram_public_groups  logger schema, "ro" client -- which groups the MCP
 *                           server may expose.
 *   web_mcp_tokens          web schema, "app" client -- who may call it.
 *
 * Strings placed on returned JSON are Render::esc()-escaped, as elsewhere in
 * the web app. (The MCP server itself does NOT escape -- see web/src/mcp --
 * because its output goes to a model, not a browser. These functions are for
 * the admin pages only.)
 */
namespace tgweb::dao::mcp {

/* ---- The exposure allowlist (logger schema, "ro" client) ---- */

/*
 * Allowed groups, newest first, with the live title and username resolved by
 * joining the logger schema. Each entry also carries `is_public_now`: whether
 * the group currently owns an active public username.
 *
 * That flag is a HINT for the admin, never a gate. It exists so a reviewer can
 * see at a glance that a group they exposed has since gone private -- the whole
 * reason the allowlist is curated rather than derived.
 */
drogon::Task<nlohmann::json> listAllowed(drogon::orm::DbClientPtr db);

/* True if the group exists in the logger schema at all. */
drogon::Task<bool> groupExists(drogon::orm::DbClientPtr db, int64_t groupId);

/* Already on the allowlist? Checked before adding, so a duplicate gets a clear
 * message rather than silently doing nothing. */
drogon::Task<bool> isAllowed(drogon::orm::DbClientPtr db, int64_t groupId);

drogon::Task<void> allowGroup(drogon::orm::DbClientPtr db, int64_t groupId,
			      std::string note, uint64_t addedBy);

drogon::Task<void> disallowGroup(drogon::orm::DbClientPtr db, int64_t groupId);

/*
 * Group search for the select2 picker. Returns RAW (un-escaped) text, because
 * select2 escapes result text itself. Each hit carries `is_public_now` so the
 * picker can mark which groups have a public username, without filtering the
 * others out -- an admin may have a good reason to expose a group that does
 * not, and the decision is theirs.
 */
drogon::Task<nlohmann::json> searchGroups(drogon::orm::DbClientPtr db,
					  std::string q, int limit);

/* ---- Bearer tokens (web schema, "app" client) ---- */

drogon::Task<nlohmann::json> listTokens(drogon::orm::DbClientPtr db);

/*
 * Store a new token. `sha256` is the raw 32-byte digest of the plaintext; the
 * plaintext itself is never passed here, so it cannot be persisted by accident.
 */
drogon::Task<uint64_t> createToken(drogon::orm::DbClientPtr db,
				   uint64_t webUserId, std::string name,
				   std::string sha256);

/* Revoke by stamping revoked_at, so the row stays auditable. */
drogon::Task<void> revokeToken(drogon::orm::DbClientPtr db, uint64_t id);

/*
 * Resolve a token digest to its owning account, or nullopt when the digest is
 * unknown or the token has been revoked. Used by the /mcp endpoint on every
 * request, so it is one lookup on the unique digest index.
 */
struct TokenOwner {
	uint64_t	tokenId = 0;
	uint64_t	webUserId = 0;
	std::string	username;
	std::string	role;
};

drogon::Task<std::optional<TokenOwner>> resolveToken(drogon::orm::DbClientPtr db,
						     std::string sha256);

/* Best-effort last_used_at stamp; failures are not worth failing a request. */
drogon::Task<void> touchToken(drogon::orm::DbClientPtr db, uint64_t tokenId);

} /* namespace tgweb::dao::mcp */

#endif /* TGLOGGERD_WEB_DAO_MCP_HPP */
