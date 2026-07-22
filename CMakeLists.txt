cmake_minimum_required(VERSION 3.16)

project(tgloggerd
	VERSION 0.1.0
	DESCRIPTION "Telegram logger daemon"
	LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# Build TDLib from the submodule.
add_subdirectory(submodules/td EXCLUDE_FROM_ALL)

# Build MySQL Connector/C++ from the submodule. Only the legacy JDBC API
# (connector-jdbc, classic protocol) is needed; it wraps libmysqlclient.
set(WITH_JDBC ON CACHE BOOL "Build the Connector/C++ legacy JDBC API" FORCE)
set(WITH_SSL system CACHE STRING "Use the system OpenSSL for Connector/C++" FORCE)
add_subdirectory(submodules/mysql-connector-cpp EXCLUDE_FROM_ALL)
# When consumed via add_subdirectory (rather than an install), the JDBC
# public headers are not staged, so expose the in-tree header locations
# (both the source headers and the generated cppconn headers) to anything
# that links connector-jdbc.
target_include_directories(connector-jdbc INTERFACE
	"${MySQL_CONCPP_SOURCE_DIR}/jdbc"
	"${MySQL_CONCPP_SOURCE_DIR}/jdbc/driver"
	"${MySQL_CONCPP_BINARY_DIR}/jdbc"
	"${MySQL_CONCPP_BINARY_DIR}/jdbc/cppconn")

add_executable(tgloggerd
	src/main.c
	src/mysql/ConnectionPool.cpp
	src/mysql/ConnectionPool.hpp
	src/mysql/Database.cpp
	src/mysql/Database.hpp
	src/tgloggerd/DB.cpp
	src/tgloggerd/DB.hpp
	src/tgloggerd/models/User.hpp
	src/tgloggerd/models/User.cpp
	src/tgloggerd/models/File.hpp
	src/tgloggerd/models/File.cpp
	src/tgloggerd/models/Group.hpp
	src/tgloggerd/models/Group.cpp
	src/tgloggerd/models/Message.hpp
	src/tgloggerd/models/Message.cpp
	src/tgloggerd/models/PrivateMessage.hpp
	src/tgloggerd/models/PrivateMessage.cpp
	src/tgloggerd/models/GroupMessage.hpp
	src/tgloggerd/models/GroupMessage.cpp
	src/tgloggerd/models/GroupAdmin.hpp
	src/tgloggerd/models/GroupAdmin.cpp
	src/tgloggerd/models/Backfill.hpp
	src/tgloggerd/models/Backfill.cpp
	src/tgloggerd/helpers/common.c
	src/tgloggerd/helpers/common.h
	src/tgloggerd/helpers/log.c
	src/tgloggerd/helpers/log.h
	src/tgloggerd/ThreadPool.cpp
	src/tgloggerd/ThreadPool.hpp
	src/tgloggerd/TDLib.cpp
	src/tgloggerd/TDLib.hpp
	src/tgloggerd/DiscordClient.cpp
	src/tgloggerd/DiscordClient.hpp
	src/tgloggerd/DiscordForwarder.cpp
	src/tgloggerd/DiscordForwarder.hpp
	src/tgloggerd/tgloggerd_extern.cpp
	src/tgloggerd/tgloggerd_extern.h
	src/tgloggerd/TgLoggerd.cpp
	src/tgloggerd/TgLoggerd.hpp)

target_include_directories(tgloggerd PRIVATE src)
target_compile_definitions(tgloggerd PRIVATE _GNU_SOURCE)
target_compile_options(tgloggerd PRIVATE
	-Wall
	-Wextra)

# Link against the static TDLib C++ client interface (td::ClientManager).
# This also propagates the TDLib include directories so its interface
# headers can be included from TDLib.cpp. connector-jdbc is the MySQL
# Connector/C++ legacy JDBC library. OpenSSL::Crypto provides SHA-256 for
# hashing downloaded files.
find_package(OpenSSL REQUIRED)
# libcurl (HTTPS) for the Discord webhook forwarder.
find_package(CURL REQUIRED)
# std::thread in the background worker/file pools needs the platform threads
# library explicitly, rather than relying on it arriving transitively.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(tgloggerd PRIVATE Td::TdStatic connector-jdbc OpenSSL::Crypto CURL::libcurl Threads::Threads)
