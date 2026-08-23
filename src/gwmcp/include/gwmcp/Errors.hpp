// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef GWMCP__ERRORS_HPP
#define GWMCP__ERRORS_HPP

#include <exception>
#include <string>
#include <utility>

#include "Json.hpp"

namespace gwmcp {

/* JSON-RPC 2.0 error codes. */
namespace rpc {
constexpr int PARSE_ERROR      = -32700;
constexpr int INVALID_REQUEST  = -32600;
constexpr int METHOD_NOT_FOUND = -32601;
constexpr int INVALID_PARAMS   = -32602;
constexpr int INTERNAL_ERROR   = -32603;
} /* namespace rpc */

/*
 * MCP has TWO error channels and conflating them is a spec violation, so they
 * are two distinct C++ types here:
 *
 *   RpcError  -> {"error": {...}}                    the request was malformed:
 *                                                    unknown method, unknown
 *                                                    tool, bad params shape.
 *
 *   ToolError -> {"result": {..., "isError": true}}  the request was fine and
 *                                                    the tool ran, but the work
 *                                                    failed: no such user, the
 *                                                    query timed out, a value
 *                                                    was out of range.
 *
 * The distinction matters to a client: an RpcError says "you called me wrong",
 * a ToolError says "your call was fine, here is what went wrong" -- and only
 * the latter is something a model can usefully read and retry differently.
 */
class RpcError : public std::exception {
public:
	RpcError(int code, std::string message)
		: code_(code), msg_(std::move(message))
	{
	}

	RpcError(int code, std::string message, Json data)
		: code_(code), msg_(std::move(message)), data_(std::move(data)),
		  hasData_(true)
	{
	}

	const char *what(void) const noexcept override { return msg_.c_str(); }

	int code(void) const { return code_; }
	const std::string &message(void) const { return msg_; }
	bool hasData(void) const { return hasData_; }
	const Json &data(void) const { return data_; }

private:
	int		code_;
	std::string	msg_;
	Json		data_;
	bool		hasData_ = false;
};

/* Thrown by a tool handler when the work fails. */
class ToolError : public std::exception {
public:
	explicit ToolError(std::string message) : msg_(std::move(message)) {}

	const char *what(void) const noexcept override { return msg_.c_str(); }
	const std::string &message(void) const { return msg_; }

private:
	std::string msg_;
};

} /* namespace gwmcp */

#endif /* #ifndef GWMCP__ERRORS_HPP */
