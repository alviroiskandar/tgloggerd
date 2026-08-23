# SPDX-License-Identifier: GPL-2.0-only
#
# discordd -- the Discord daemon: logs Discord messages and forwards them to
# Telegram.
#
# Added by the root CMakeLists AFTER the TDLib and MySQL Connector/C++
# subprojects, because it links Td::TdStatic and connector-jdbc from them.
#
# gwdiscord is pulled in from its own directory as a self-contained project.
# It is intentionally a separate build with no tgloggerd dependency, so it can
# be lifted into its own repository later -- see src/gwdiscord/README.md.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../gwdiscord gwdiscord)

add_executable(discordd
	main.cpp
	DiscordD.cpp
	DiscordD.hpp
	DB.cpp
	DB.hpp
	TelegramSender.cpp
	TelegramSender.hpp
	# The generic MySQL layer is shared with tgloggerd. It is compiled in
	# here rather than linked from the tgloggerd target, which is an
	# executable; the two files are small and carry no daemon specifics.
	${CMAKE_SOURCE_DIR}/src/mysql/ConnectionPool.cpp
	${CMAKE_SOURCE_DIR}/src/mysql/Database.cpp)

# Land the binary in build/ alongside tgloggerd rather than in
# build/src/discordd/, so run.sh and the compose service use one obvious path.
set_target_properties(discordd PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

target_include_directories(discordd PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(discordd PRIVATE _GNU_SOURCE)
target_compile_options(discordd PRIVATE -Wall -Wextra)

# gwdiscord exposes only its own headers: Boost and jsoncpp are PRIVATE to it,
# so nothing here compiles against either.
target_link_libraries(discordd PRIVATE
	gwdiscord
	connector-jdbc
	Td::TdStatic
	Threads::Threads)
