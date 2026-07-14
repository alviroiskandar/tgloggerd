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

# Vendored Drogon (carries trantor as its own submodule). Trim its extras to
# keep the build lean; its MySQL DbClient is enabled automatically because
# libmariadb-dev is installed (see README).
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
	src/auth/Password.cpp
	src/auth/Csrf.cpp
	src/auth/AuthFilter.cpp
	src/auth/AdminFilter.cpp
	src/auth/RateLimiter.cpp
	src/dao/Accounts.cpp
	src/dao/Audit.cpp
	src/dao/Browse.cpp
	src/views/Render.cpp
	src/controllers/AuthController.cpp
	src/controllers/DashboardController.cpp
	src/controllers/UsersController.cpp
	src/controllers/GroupsController.cpp)

# src/ is the web app's own tree; ../src exposes the reusable tgloggerd model
# headers (plain value types) should we want them. third_party/ holds the
# vendored header-only inja template engine and its nlohmann/json dependency.
target_include_directories(tgloggerd_web PRIVATE
	src ../src third_party ${SODIUM_INCLUDE_DIRS})
target_link_libraries(tgloggerd_web PRIVATE drogon ${SODIUM_LIBRARIES})
target_compile_options(tgloggerd_web PRIVATE -Wall -Wextra)
