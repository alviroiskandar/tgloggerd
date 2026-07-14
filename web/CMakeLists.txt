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

add_executable(tgloggerd_web
	src/main.cpp)

# src/ is the web app's own tree; ../src exposes the reusable tgloggerd model
# headers (plain value types) should we want them.
target_include_directories(tgloggerd_web PRIVATE src ../src)
target_link_libraries(tgloggerd_web PRIVATE drogon)
target_compile_options(tgloggerd_web PRIVATE -Wall -Wextra)
