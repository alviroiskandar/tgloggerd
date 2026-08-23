# SPDX-License-Identifier: GPL-2.0-or-later
#
# gwmcp -- a small Model Context Protocol server library.
#
# This is a STANDALONE CMake project that is also add_subdirectory()-able:
#
#   standalone   cmake -S src/gwmcp -B build/gwmcp && cmake --build build/gwmcp
#   embedded     add_subdirectory(../gwmcp gwmcp)
#                target_link_libraries(<consumer> PRIVATE gwmcp)
#
# It deliberately has NO dependency on tgloggerd: no SQL, no tgloggerd headers,
# no shared helpers, and no knowledge of Telegram or Discord. That is what
# keeps it extractable into its own repository. See README.md.
cmake_minimum_required(VERSION 3.16)
project(gwmcp VERSION 0.1.0 LANGUAGES CXX)

# C++17 is enough for everything here. The web app that consumes this builds at
# C++20; the standard is directory-scoped, so the two do not fight.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build the examples only when this is the top-level project, so embedding it
# does not silently add binaries to the parent build.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	set(GWMCP_IS_TOPLEVEL ON)
else()
	set(GWMCP_IS_TOPLEVEL OFF)
endif()
option(GWMCP_BUILD_EXAMPLES "Build the gwmcp example programs"
       ${GWMCP_IS_TOPLEVEL})

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# The web app is built with position-independent code; match it so this library
# can be linked into it either way.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Dependencies: nlohmann/json only, and header-only at that.
#
# Unlike gwdiscord -- which hides jsoncpp behind an abstraction -- the JSON type
# is part of this library's public API, because MCP is JSON to its core (see
# include/gwmcp/Json.hpp). So the include directory is PUBLIC.
#
# GWMCP_NLOHMANN_INCLUDE_DIR lets a consumer point at a copy it already vendors
# instead of installing the system package; the versions must match.
# ---------------------------------------------------------------------------
set(GWMCP_NLOHMANN_INCLUDE_DIR "" CACHE PATH
    "Directory containing nlohmann/json.hpp; empty = find the system package")

if(GWMCP_NLOHMANN_INCLUDE_DIR)
	set(GWMCP_JSON_INCLUDE "${GWMCP_NLOHMANN_INCLUDE_DIR}")
else()
	find_package(nlohmann_json QUIET)
	if(nlohmann_json_FOUND)
		get_target_property(GWMCP_JSON_INCLUDE
				    nlohmann_json::nlohmann_json
				    INTERFACE_INCLUDE_DIRECTORIES)
	else()
		find_path(GWMCP_JSON_INCLUDE nlohmann/json.hpp)
	endif()
	if(NOT GWMCP_JSON_INCLUDE)
		message(FATAL_ERROR
			"nlohmann/json.hpp not found. Install nlohmann-json3-dev, "
			"or set GWMCP_NLOHMANN_INCLUDE_DIR to a vendored copy.")
	endif()
endif()

add_library(gwmcp STATIC
	src/Server.cpp
	src/ToolRegistry.cpp
	src/Protocol.cpp
	src/Protocol.hpp
	include/gwmcp/Errors.hpp
	include/gwmcp/Json.hpp
	include/gwmcp/Server.hpp
	include/gwmcp/Tool.hpp
	include/gwmcp/ToolRegistry.hpp)

# PUBLIC: our own headers, plus the JSON headers they name in their signatures.
# PRIVATE: src/, so internal headers such as Protocol.hpp stay unreachable from
# a consumer.
target_include_directories(gwmcp
	PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
		${GWMCP_JSON_INCLUDE}
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_options(gwmcp PRIVATE -Wall -Wextra)

if(GWMCP_BUILD_EXAMPLES)
	add_executable(mcp_selftest examples/mcp_selftest.cpp)
	target_link_libraries(mcp_selftest PRIVATE gwmcp)
	target_compile_options(mcp_selftest PRIVATE -Wall -Wextra)

	add_executable(mcp_stdio examples/mcp_stdio.cpp)
	target_link_libraries(mcp_stdio PRIVATE gwmcp)
	target_compile_options(mcp_stdio PRIVATE -Wall -Wextra)

	enable_testing()
	add_test(NAME mcp_selftest COMMAND mcp_selftest)
endif()
