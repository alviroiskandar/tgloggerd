// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_MCP_TOKEN_HPP
#define TGLOGGERD_WEB_MCP_TOKEN_HPP

#include <string>

/*
 * MCP bearer tokens: minting, hashing, and pulling one out of a request header.
 *
 * Shared by the admin page (which mints) and the /mcp endpoint (which verifies),
 * so the two can never disagree about the format or the digest.
 */
namespace tgweb::mcp::token {

/* Every token starts with this, so one is recognisable in a log or a paste. */
constexpr const char *PREFIX = "tgmcp_";

/*
 * A fresh token: PREFIX followed by 32 random bytes, hex-encoded. High enough
 * entropy that guessing is not a threat model -- which is why the digest below
 * can be a fast hash rather than a password hash.
 */
std::string mint(void);

/*
 * Raw 32-byte SHA-256 of `plaintext`, as a std::string of bytes suitable for
 * binding to the BINARY(32) column. Not hex: the column is binary, and keeping
 * it that way halves the index and removes any case-sensitivity question.
 */
std::string digest(const std::string &plaintext);

/*
 * Extract the credential from an Authorization header value. Accepts
 * "Bearer <token>" case-insensitively on the scheme, tolerates extra spaces,
 * and returns "" when the header is absent or not a bearer credential.
 */
std::string fromAuthorizationHeader(const std::string &header);

/*
 * The query-string parameter that may carry a token instead of the header, for
 * clients that accept only a URL. Weaker than the header -- see the comment on
 * credential() in McpController.cpp -- but the only option for some clients.
 */
constexpr const char *QUERY_PARAM = "key";

/* Shape check only -- says nothing about whether the token exists. */
bool looksLikeToken(const std::string &s);

} /* namespace tgweb::mcp::token */

#endif /* TGLOGGERD_WEB_MCP_TOKEN_HPP */
