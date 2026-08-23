# SPDX-License-Identifier: GPL-2.0-or-later
#
# gwdiscord -- a small Discord bot library (gateway + minimal REST).
#
# This is a STANDALONE CMake project that is also add_subdirectory()-able:
#
#   standalone   cmake -S src/gwdiscord -B build/gwdiscord && cmake --build build/gwdiscord
#   embedded     add_subdirectory(../gwdiscord gwdiscord)   # from src/discordd
#                target_link_libraries(discordd PRIVATE gwdiscord)
#
# It deliberately has NO dependency on tgloggerd: no SQL, no tgloggerd headers,
# no shared helpers. That is what keeps it extractable into its own repository.
# See README.md.
cmake_minimum_required(VERSION 3.16)
project(gwdiscord VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build the examples only when this is the top-level project, so embedding it
# in discordd does not silently add binaries to the parent build.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	set(GWDISCORD_IS_TOPLEVEL ON)
else()
	set(GWDISCORD_IS_TOPLEVEL OFF)
endif()
option(GWDISCORD_BUILD_EXAMPLES "Build the gwdiscord example programs"
       ${GWDISCORD_IS_TOPLEVEL})

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies.
#
# Boost is needed ONLY by the transport backend below, and only for headers:
# Beast and Asio are header-only, so nothing links against a Boost binary and
# the resulting library has no Boost runtime dependency at all.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

find_package(Boost QUIET)
if(NOT Boost_FOUND)
	# Ubuntu's libboost-dev does not always ship a CMake config; the
	# headers are all we need, so locate them directly.
	find_path(Boost_INCLUDE_DIRS boost/beast/websocket.hpp)
	if(NOT Boost_INCLUDE_DIRS)
		message(FATAL_ERROR
			"Boost.Beast headers not found. Install libboost-dev, "
			"or build a different transport backend (see README.md).")
	endif()
endif()

# ---------------------------------------------------------------------------
# THE TRANSPORT BACKEND.
#
# Exactly one file here implements gwdiscord::WebSocket + gwdiscord::HttpClient
# against a real network stack, and it is the ONLY file that includes Boost.
# To swap the stack, write a sibling implementation and change this one
# variable -- nothing else in the build or the source needs to move.
# ---------------------------------------------------------------------------
set(GWDISCORD_TRANSPORT_SRC src/BeastTransport.cpp
    CACHE STRING "Source file implementing the gwdiscord transport backend")

add_library(gwdiscord STATIC
	${GWDISCORD_TRANSPORT_SRC}
	src/Gateway.cpp
	src/EventParse.cpp
	src/EventParse.hpp
	src/MockTransport.cpp
	src/Rest.cpp
	include/gwdiscord/Events.hpp
	include/gwdiscord/Gateway.hpp
	include/gwdiscord/Log.hpp
	include/gwdiscord/MockTransport.hpp
	include/gwdiscord/Rest.hpp
	include/gwdiscord/Transport.hpp)

# PUBLIC: consumers get gwdiscord's own headers, which name no third-party
# type. PRIVATE: Boost and jsoncpp stop at this library's boundary, so
# discordd never compiles against either.
target_include_directories(gwdiscord
	PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
		${Boost_INCLUDE_DIRS}
		${JSONCPP_INCLUDE_DIRS})

target_compile_options(gwdiscord PRIVATE -Wall -Wextra)

target_link_libraries(gwdiscord
	PUBLIC  Threads::Threads
	PRIVATE OpenSSL::SSL OpenSSL::Crypto ${JSONCPP_LIBRARIES})
target_link_directories(gwdiscord PRIVATE ${JSONCPP_LIBRARY_DIRS})

if(GWDISCORD_BUILD_EXAMPLES)
	add_executable(gw_tail examples/gw_tail.cpp)
	target_link_libraries(gw_tail PRIVATE gwdiscord)
	target_compile_options(gw_tail PRIVATE -Wall -Wextra)

	add_executable(gw_mock_test examples/gw_mock_test.cpp)
	target_link_libraries(gw_mock_test PRIVATE gwdiscord)
	target_compile_options(gw_mock_test PRIVATE -Wall -Wextra)

	enable_testing()
	add_test(NAME gw_mock_test COMMAND gw_mock_test)
endif()
