# tgloggerd_web — a server-rendered web UI to browse the tgloggerd database.
#
# This is a STANDALONE CMake project, intentionally NOT added by the root
# CMakeLists: the root unconditionally builds TDLib and the JDBC connector,
# neither of which the web app needs. It links Drogon (async, non-blocking)
# and talks to MySQL through Drogon's own DbClient — never the daemon's
# blocking src/mysql/ layer.
cmake_minimum_required(VERSION 3.16)
project(tgloggerd_web VERSION 0.1.0 LANGUAGES CXX)

# C++20 for Drogon's coroutine DB API (co_await execSqlCoro). Independent of
# the daemon's C++17 build, since this is a separate project.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# Vendored MariaDB Connector/C.
#
# Drogon's async MySQL client needs MariaDB Connector/C, whose system package
# (libmariadb-dev) conflicts on Debian/Ubuntu with the Oracle libmysqlclient-dev
# the daemon's vendored mysql-connector-cpp requires. To let both build on one
# machine we vendor the connector here instead of using the system package, and
# point Drogon at it (see cmake/FindMySQL.cmake). Static libraries linked into
# the PIE executable need position-independent code.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Trim the connector to just the client library.
set(WITH_UNIT_TESTS    OFF CACHE BOOL "" FORCE)
set(WITH_CURL          OFF CACHE BOOL "" FORCE)  # avoids a libcurl dependency
set(WITH_EXTERNAL_ZLIB ON  CACHE BOOL "" FORCE)  # use the system zlib
set(WITH_SSL           ON  CACHE BOOL "" FORCE)  # OpenSSL
# Do not verify the server certificate by default. Connector/C 3.4 verifies by
# default and so rejects a self-signed server cert; the system libmariadb and
# the daemon's Oracle client do not verify, and a deployment terminates TLS at
# the proxy anyway. (A future WEB_DB_TLS option could re-enable verification.)
set(DEFAULT_SSL_VERIFY_SERVER_CERT OFF CACHE BOOL "" FORCE)
# Embed the authentication plugins into the client so no plugin .so is needed at
# runtime. caching_sha2_password is the MySQL 8/9 default and defaults to a
# dynamic module; sha256_password backs it over a non-TLS connection.
set(CLIENT_PLUGIN_CACHING_SHA2_PASSWORD STATIC CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_SHA256_PASSWORD       STATIC CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_MYSQL_NATIVE_PASSWORD STATIC CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_MYSQL_CLEAR_PASSWORD  STATIC CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_DIALOG                OFF    CACHE STRING "" FORCE)
# Turn off the remaining dynamic plugins (MariaDB-only auth and zstd
# compression); unused here and their install rules break the subproject build.
set(CLIENT_PLUGIN_CLIENT_ED25519        OFF    CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_PARSEC                OFF    CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_ZSTD                  OFF    CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_AUTH_GSSAPI_CLIENT    OFF    CACHE STRING "" FORCE)
add_subdirectory(../submodules/mariadb-connector-c
	${CMAKE_BINARY_DIR}/mariadb-connector-c EXCLUDE_FROM_ALL)

# Make Drogon's find_package(MySQL) resolve to the vendored client above.
set(TGWEB_MARIADB_INCLUDE_DIRS
	${CMAKE_CURRENT_SOURCE_DIR}/../submodules/mariadb-connector-c/include
	${CMAKE_BINARY_DIR}/mariadb-connector-c/include)
list(PREPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
# mysql_optionsv exists in the vendored connector; skip the configure-time probe
# (the library is not built yet when Drogon runs it).
set(HAS_MYSQL_OPTIONSV TRUE CACHE INTERNAL "vendored mariadb has mysql_optionsv")

# Vendored Drogon (carries trantor as its own submodule). Trim its extras to
# keep the build lean; its MySQL DbClient is enabled against the vendored
# MariaDB Connector/C above.
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_CTL      OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING  OFF CACHE BOOL "" FORCE)
add_subdirectory(../submodules/drogon ${CMAKE_BINARY_DIR}/drogon EXCLUDE_FROM_ALL)

# libsodium provides argon2id password hashing (crypto_pwhash_str); its ISC
# license is GPL-2.0 compatible.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED libsodium)

add_executable(tgloggerd_web
	src/main.cpp
	src/Config.hpp
	src/auth/Password.cpp
	src/auth/Password.hpp
	src/auth/Token.cpp
	src/auth/Token.hpp
	src/auth/FileToken.cpp
	src/auth/FileToken.hpp
	src/auth/Session.cpp
	src/auth/Session.hpp
	src/auth/Csrf.cpp
	src/auth/Csrf.hpp
	src/auth/AuthFilter.cpp
	src/auth/AuthFilter.hpp
	src/auth/AdminFilter.cpp
	src/auth/AdminFilter.hpp
	src/auth/RateLimiter.cpp
	src/auth/RateLimiter.hpp
	src/dao/Accounts.cpp
	src/dao/Accounts.hpp
	src/dao/Audit.cpp
	src/dao/Audit.hpp
	src/dao/Browse.cpp
	src/dao/Browse.hpp
	src/dao/Search.cpp
	src/dao/Search.hpp
	src/views/Render.cpp
	src/views/Render.hpp
	src/controllers/Common.hpp
	src/controllers/AuthController.cpp
	src/controllers/AuthController.hpp
	src/controllers/DashboardController.cpp
	src/controllers/DashboardController.hpp
	src/controllers/UsersController.cpp
	src/controllers/UsersController.hpp
	src/controllers/GroupsController.cpp
	src/controllers/GroupsController.hpp
	src/controllers/MessagesController.cpp
	src/controllers/MessagesController.hpp
	src/controllers/FilesController.cpp
	src/controllers/FilesController.hpp
	src/controllers/ApiController.cpp
	src/controllers/ApiController.hpp
	src/controllers/SearchController.cpp
	src/controllers/SearchController.hpp
	src/controllers/MediaController.cpp
	src/controllers/MediaController.hpp)

# src/ is the web app's own tree; ../src exposes the reusable tgloggerd model
# headers (plain value types) should we want them. third_party/ holds the
# vendored header-only inja template engine and its nlohmann/json dependency.
target_include_directories(tgloggerd_web PRIVATE
	src ../src third_party ${SODIUM_INCLUDE_DIRS})
target_link_libraries(tgloggerd_web PRIVATE drogon ${SODIUM_LIBRARIES})
target_compile_options(tgloggerd_web PRIVATE -Wall -Wextra)
