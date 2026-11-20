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
	src/tgloggerd/models/ContactName.hpp
	src/tgloggerd/models/ContactName.cpp
	src/tgloggerd/models/ContactLookup.cpp
	src/tgloggerd/models/File.hpp
	src/tgloggerd/models/File.cpp
	src/tgloggerd/models/FileAccess.cpp
	src/tgloggerd/models/FileEvict.cpp
	src/tgloggerd/models/FileFetch.cpp
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
	src/tgloggerd/models/Poll.hpp
	src/tgloggerd/models/Poll.cpp
	src/tgloggerd/models/ForumTopic.hpp
	src/tgloggerd/models/ForumTopic.cpp
	src/tgloggerd/models/Emoji.hpp
	src/tgloggerd/models/Emoji.cpp
	src/tgloggerd/models/Community.hpp
	src/tgloggerd/models/Community.cpp
	src/tgloggerd/models/Backfill.cpp
	src/tgloggerd/helpers/common.c
	src/tgloggerd/helpers/common.h
	src/tgloggerd/helpers/log.c
	src/tgloggerd/helpers/log.h
	src/tgloggerd/ThreadPool.cpp
	src/tgloggerd/ThreadPool.hpp
	src/tgloggerd/PhotoApi.cpp
	src/tgloggerd/PhotoApi.hpp
	src/tgloggerd/EmojiApi.cpp
	src/tgloggerd/EmojiApi.hpp
	src/tgloggerd/EmojiBackend.cpp
	src/tgloggerd/MessageExtract.cpp
	src/tgloggerd/MessageExtract.hpp
	src/tgloggerd/EmojiExtract.cpp
	src/tgloggerd/EmojiExtract.hpp
	src/tgloggerd/CommunityExtract.cpp
	src/tgloggerd/CommunityExtract.hpp
	src/tgloggerd/TDLib.cpp
	src/tgloggerd/TDLib.hpp
	src/tgloggerd/FileToken.cpp
	src/tgloggerd/FileToken.hpp
	src/tgloggerd/InternalApi.cpp
	src/tgloggerd/InternalApi.hpp
	src/tgloggerd/FileApi.cpp
	src/tgloggerd/FileApi.hpp
	src/tgloggerd/BotWatch.cpp
	src/tgloggerd/BotWatch.hpp
	src/tgloggerd/EventBus.cpp
	src/tgloggerd/EventBus.hpp
	src/tgloggerd/EventApi.cpp
	src/tgloggerd/EventApi.hpp
	src/tgloggerd/FetchState.cpp
	src/tgloggerd/FetchState.hpp
	src/tgloggerd/FileFetcher.cpp
	src/tgloggerd/FileFetcher.hpp
	src/tgloggerd/FileIndex.hpp
	src/tgloggerd/ChatRef.cpp
	src/tgloggerd/ChatRef.hpp
	src/tgloggerd/ChatApi.cpp
	src/tgloggerd/ChatApi.hpp
	src/tgloggerd/ChatRecheck.cpp
	src/tgloggerd/ChatRecheck.hpp
	src/tgloggerd/RecheckApi.cpp
	src/tgloggerd/RecheckApi.hpp
	src/tgloggerd/RecheckBackend.cpp
	src/tgloggerd/ContactLookup.cpp
	src/tgloggerd/ContactLookup.hpp
	src/tgloggerd/ContactApi.cpp
	src/tgloggerd/ContactApi.hpp
	src/tgloggerd/DiscordClient.cpp
	src/tgloggerd/DiscordClient.hpp
	src/tgloggerd/DiscordForwarder.cpp
	src/tgloggerd/DiscordForwarder.hpp
	src/tgloggerd/ForwardFilter.cpp
	src/tgloggerd/ForwardFilter.hpp
	src/tgloggerd/StoragePath.hpp
	src/tgloggerd/StorageJanitor.cpp
	src/tgloggerd/StorageJanitor.hpp
	src/tgloggerd/StorageApi.cpp
	src/tgloggerd/StorageApi.hpp
	src/tgloggerd/StatusApi.cpp
	src/tgloggerd/StatusApi.hpp
	src/tgloggerd/StoreHolds.hpp
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
# libsodium: the daemon mints the web's /files/<token> URLs for Discord
# avatar/media links, using the same construction as web/src/auth/FileToken.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED libsodium)
# std::thread in the background worker/file pools needs the platform threads
# library explicitly, rather than relying on it arriving transitively.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
# The internal API (src/tgloggerd/InternalApi.cpp) serves HTTP with
# Boost.Beast/Asio, which are header-only: nothing links a Boost binary.
# Only InternalApi.cpp includes them. Its JSON is nlohmann-json.
find_package(Boost QUIET)
if(NOT Boost_FOUND)
	# Ubuntu's libboost-dev does not always ship a CMake config; the
	# headers are all that is needed, so locate them directly.
	find_path(Boost_INCLUDE_DIRS boost/beast/http.hpp)
	if(NOT Boost_INCLUDE_DIRS)
		message(FATAL_ERROR
			"Boost.Beast headers not found. Install libboost-dev.")
	endif()
endif()
find_package(nlohmann_json 3 REQUIRED)
target_link_libraries(tgloggerd PRIVATE Td::TdStatic tdjson_private connector-jdbc OpenSSL::Crypto CURL::libcurl ${SODIUM_LIBRARIES} Threads::Threads nlohmann_json::nlohmann_json)
target_include_directories(tgloggerd PRIVATE ${SODIUM_INCLUDE_DIRS} ${Boost_INCLUDE_DIRS})

# BotWatch over a fake read of the bots; not part of the default build:
#   cmake --build build --target bot_watch_test && build/bot_watch_test
add_executable(bot_watch_test EXCLUDE_FROM_ALL
	src/tgloggerd/tests/bot_watch_test.cpp
	src/tgloggerd/BotWatch.cpp
	src/tgloggerd/helpers/log.c)
target_include_directories(bot_watch_test PRIVATE src)
target_link_libraries(bot_watch_test PRIVATE connector-jdbc
	nlohmann_json::nlohmann_json Threads::Threads)

# GET /v1/status over a real InternalApi on loopback with a fake TDLib
# state; not part of the default build:
#   cmake --build build --target status_api_test && build/status_api_test
add_executable(status_api_test EXCLUDE_FROM_ALL
	src/tgloggerd/tests/status_api_test.cpp
	src/tgloggerd/StatusApi.cpp
	src/tgloggerd/InternalApi.cpp
	src/tgloggerd/ThreadPool.cpp
	src/tgloggerd/helpers/log.c)
target_include_directories(status_api_test PRIVATE src
	${SODIUM_INCLUDE_DIRS} ${Boost_INCLUDE_DIRS})
target_compile_options(status_api_test PRIVATE -Wall -Wextra)
target_link_libraries(status_api_test PRIVATE connector-jdbc
	${SODIUM_LIBRARIES} nlohmann_json::nlohmann_json Threads::Threads)

# message_extra_files() over hand-built TDLib messages; not part of the
# default build:
#   cmake --build build --target extra_files_test && build/extra_files_test
add_executable(extra_files_test EXCLUDE_FROM_ALL
	src/tgloggerd/tests/extra_files_test.cpp
	src/tgloggerd/MessageExtract.cpp)
target_include_directories(extra_files_test PRIVATE src)
target_compile_options(extra_files_test PRIVATE -Wall -Wextra)
target_link_libraries(extra_files_test PRIVATE Td::TdStatic tdjson_private
	nlohmann_json::nlohmann_json Threads::Threads)

# Which parts of a message each forwarding flag lets through to Discord
# (no TDLib, no database); not part of the default build:
#   cmake --build build --target forward_filter_test && build/forward_filter_test
add_executable(forward_filter_test EXCLUDE_FROM_ALL
	src/tgloggerd/tests/forward_filter_test.cpp
	src/tgloggerd/ForwardFilter.cpp)
target_include_directories(forward_filter_test PRIVATE src)
target_compile_options(forward_filter_test PRIVATE -Wall -Wextra)

# How a rich message is stored (text, content_extra, the media in its
# blocks) over hand-built TDLib objects; not part of the default build:
#   cmake --build build --target rich_message_test && build/rich_message_test
add_executable(rich_message_test EXCLUDE_FROM_ALL
	src/tgloggerd/tests/rich_message_test.cpp
	src/tgloggerd/MessageExtract.cpp)
target_include_directories(rich_message_test PRIVATE src)
target_compile_options(rich_message_test PRIVATE -Wall -Wextra)
target_link_libraries(rich_message_test PRIVATE Td::TdStatic tdjson_private
	nlohmann_json::nlohmann_json Threads::Threads)

# How a link preview's type is stored (extra, its own medium, the target
# of a message link) over hand-built TDLib objects; not part of the
# default build:
#   cmake --build build --target link_preview_test && build/link_preview_test
add_executable(link_preview_test EXCLUDE_FROM_ALL
	src/tgloggerd/tests/link_preview_test.cpp
	src/tgloggerd/MessageExtract.cpp)
target_include_directories(link_preview_test PRIVATE src)
target_compile_options(link_preview_test PRIVATE -Wall -Wextra)
target_link_libraries(link_preview_test PRIVATE Td::TdStatic tdjson_private
	nlohmann_json::nlohmann_json Threads::Threads)

# The custom emoji and message effect routes over a real listener, with
# stand-ins for the tables and TDLib; not part of the default build:
#   cmake --build build --target emoji_api_test && build/emoji_api_test
add_executable(emoji_api_test EXCLUDE_FROM_ALL
	src/tgloggerd/tests/emoji_api_test.cpp
	src/tgloggerd/EmojiApi.cpp
	src/tgloggerd/EmojiExtract.cpp
	src/tgloggerd/ChatRef.cpp
	src/tgloggerd/InternalApi.cpp
	src/tgloggerd/ThreadPool.cpp
	src/tgloggerd/helpers/log.c)
target_include_directories(emoji_api_test PRIVATE src
	${SODIUM_INCLUDE_DIRS} ${Boost_INCLUDE_DIRS})
target_compile_options(emoji_api_test PRIVATE -Wall -Wextra)
target_link_libraries(emoji_api_test PRIVATE Td::TdStatic tdjson_private
	connector-jdbc ${SODIUM_LIBRARIES} nlohmann_json::nlohmann_json
	Threads::Threads)

# How a chat background change and an animated chat photo are stored
# (content_extra, the files beyond the main one) over hand-built TDLib
# objects; not part of the default build:
#   cmake --build build --target chat_background_test && build/chat_background_test
add_executable(chat_background_test EXCLUDE_FROM_ALL
	src/tgloggerd/tests/chat_background_test.cpp
	src/tgloggerd/MessageExtract.cpp)
target_include_directories(chat_background_test PRIVATE src)
target_compile_options(chat_background_test PRIVATE -Wall -Wextra)
target_link_libraries(chat_background_test PRIVATE Td::TdStatic tdjson_private
	nlohmann_json::nlohmann_json Threads::Threads)

# How gift service messages are stored (curated keys, their stickers)
# over hand-built TDLib objects; not part of the default build:
#   cmake --build build --target gift_test && build/gift_test
add_executable(gift_test EXCLUDE_FROM_ALL
	src/tgloggerd/tests/gift_test.cpp
	src/tgloggerd/MessageExtract.cpp)
target_include_directories(gift_test PRIVATE src)
target_compile_options(gift_test PRIVATE -Wall -Wextra)
target_link_libraries(gift_test PRIVATE Td::TdStatic tdjson_private
	nlohmann_json::nlohmann_json Threads::Threads)

# What the community tables keep of TDLib's community, its full info
# and the service lines naming one, over hand-built TDLib objects; not
# part of the default build:
#   cmake --build build --target community_test && build/community_test
add_executable(community_test EXCLUDE_FROM_ALL
	src/tgloggerd/tests/community_test.cpp
	src/tgloggerd/CommunityExtract.cpp)
target_include_directories(community_test PRIVATE src)
target_compile_options(community_test PRIVATE -Wall -Wextra)
target_link_libraries(community_test PRIVATE Td::TdStatic tdjson_private
	Threads::Threads)

# The community upserts and chat-list replacement against a throwaway
# MySQL with every migration applied (TGL_TEST_MYSQL_* in the test's
# header); not part of the default build:
#   cmake --build build --target community_db_test && build/community_db_test
file(GLOB TGLOGGERD_MODEL_SOURCES src/tgloggerd/models/*.cpp)
add_executable(community_db_test EXCLUDE_FROM_ALL
	src/tgloggerd/tests/community_db_test.cpp
	src/tgloggerd/DB.cpp
	${TGLOGGERD_MODEL_SOURCES}
	src/mysql/ConnectionPool.cpp
	src/mysql/Database.cpp)
target_include_directories(community_db_test PRIVATE src)
target_compile_options(community_db_test PRIVATE -Wall -Wextra)
target_link_libraries(community_db_test PRIVATE connector-jdbc
	nlohmann_json::nlohmann_json Threads::Threads)

# The chat recheck's queue limits, deletion selection and restart
# catch-up over plain data; not part of the default build:
#   cmake --build build --target chat_recheck_test && build/chat_recheck_test
add_executable(chat_recheck_test EXCLUDE_FROM_ALL
	src/tgloggerd/tests/chat_recheck_test.cpp
	src/tgloggerd/ChatRecheck.cpp)
target_include_directories(chat_recheck_test PRIVATE src)
target_compile_options(chat_recheck_test PRIVATE -Wall -Wextra)

# POST /v1/chats/{chat_id}/recheck over a real loopback listener with
# stand-ins for TDLib, the database and the queue's clock; not part of
# the default build:
#   cmake --build build --target recheck_api_test && build/recheck_api_test
add_executable(recheck_api_test EXCLUDE_FROM_ALL
	src/tgloggerd/tests/recheck_api_test.cpp
	src/tgloggerd/RecheckApi.cpp
	src/tgloggerd/ChatRecheck.cpp
	src/tgloggerd/InternalApi.cpp
	src/tgloggerd/ThreadPool.cpp
	src/tgloggerd/helpers/log.c)
target_include_directories(recheck_api_test PRIVATE src
	${SODIUM_INCLUDE_DIRS} ${Boost_INCLUDE_DIRS})
target_compile_options(recheck_api_test PRIVATE -Wall -Wextra)
target_link_libraries(recheck_api_test PRIVATE connector-jdbc
	${SODIUM_LIBRARIES} nlohmann_json::nlohmann_json Threads::Threads)

# What a chat recheck reads and marks in the message tables, and the
# plans of its reads, against a throwaway MySQL with every migration
# applied (TGL_TEST_MYSQL_* as community_db_test); not part of the
# default build:
#   cmake --build build --target chat_recheck_db_test && build/chat_recheck_db_test
add_executable(chat_recheck_db_test EXCLUDE_FROM_ALL
	src/tgloggerd/tests/chat_recheck_db_test.cpp
	src/tgloggerd/DB.cpp
	${TGLOGGERD_MODEL_SOURCES}
	src/mysql/ConnectionPool.cpp
	src/mysql/Database.cpp)
target_include_directories(chat_recheck_db_test PRIVATE src)
target_compile_options(chat_recheck_db_test PRIVATE -Wall -Wextra)
target_link_libraries(chat_recheck_db_test PRIVATE connector-jdbc
	nlohmann_json::nlohmann_json Threads::Threads)

# The video of an animated profile or group photo in the photo history,
# against a throwaway MySQL with every migration applied
# (TGL_TEST_MYSQL_* in the test's header); not part of the default build:
#   cmake --build build --target photo_history_db_test && build/photo_history_db_test
add_executable(photo_history_db_test EXCLUDE_FROM_ALL
	src/tgloggerd/tests/photo_history_db_test.cpp
	src/tgloggerd/DB.cpp
	${TGLOGGERD_MODEL_SOURCES}
	src/mysql/ConnectionPool.cpp
	src/mysql/Database.cpp)
target_include_directories(photo_history_db_test PRIVATE src)
target_compile_options(photo_history_db_test PRIVATE -Wall -Wextra)
target_link_libraries(photo_history_db_test PRIVATE connector-jdbc
	nlohmann_json::nlohmann_json Threads::Threads)

# Which messages the old-row thumbnail pass re-reads, and the plan of
# that read, against a throwaway MySQL with every migration applied
# (TGL_TEST_MYSQL_* as community_db_test); not part of the default build:
#   cmake --build build --target thumbnails_db_test && build/thumbnails_db_test
add_executable(thumbnails_db_test EXCLUDE_FROM_ALL
	src/tgloggerd/tests/thumbnails_db_test.cpp
	src/tgloggerd/DB.cpp
	${TGLOGGERD_MODEL_SOURCES}
	src/mysql/ConnectionPool.cpp
	src/mysql/Database.cpp)
target_include_directories(thumbnails_db_test PRIVATE src)
target_compile_options(thumbnails_db_test PRIVATE -Wall -Wextra)
target_link_libraries(thumbnails_db_test PRIVATE connector-jdbc
	nlohmann_json::nlohmann_json Threads::Threads)

# discordd -- the Discord daemon (logs Discord messages, forwards them to
# Telegram). Added last: it links Td::TdStatic and connector-jdbc from the
# subprojects above, and pulls in the self-contained gwdiscord library. Its
# own CMakeLists owns those details.
add_subdirectory(src/discordd)
