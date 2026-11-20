# Shadow Drogon's bundled cmake_modules/FindMySQL.cmake.
#
# This directory is prepended to CMAKE_MODULE_PATH before Drogon is added, so
# Drogon's find_package(MySQL) resolves here. Instead of searching the system
# for a MySQL/MariaDB client (which on this box would wrongly pick up the Oracle
# libmysqlclient the daemon uses), point Drogon at the vendored MariaDB
# Connector/C built in-tree (target mariadbclient).
#
# Drogon expects: MySQL_FOUND, and an imported/interface target MySQL_lib that
# carries the include directories and links the client library.

if(TARGET mariadbclient)
    set(MySQL_FOUND TRUE)
    if(NOT TARGET MySQL_lib)
        # MySQL_lib must be IMPORTED: Drogon links it into a target it exports
        # (install(EXPORT DrogonTargets)), and a non-imported interface library
        # would be rejected as "not in any export set". Linking the real
        # mariadbclient target through the interface still carries its
        # transitive system dependencies (OpenSSL, zlib, pthread, ...).
        #
        # The connector is built WITH_SSL, so OpenSSL is always linked; 3.5 at
        # least, since 1.1.1 and earlier are under the OpenSSL/SSLeay licence,
        # which no version of the GPL can be combined with, and 3.0 to 3.4 are
        # out of upstream support. The connector and Drogon look OpenSSL up
        # first and FindOpenSSL caches the result, so this checks the library
        # they link as well.
        find_package(OpenSSL 3.5 REQUIRED)
        set(_mysql_link mariadbclient ${CMAKE_DL_LIBS} OpenSSL::SSL OpenSSL::Crypto)
        add_library(MySQL_lib INTERFACE IMPORTED)
        set_target_properties(MySQL_lib PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${TGWEB_MARIADB_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES "${_mysql_link}")
    endif()
else()
    set(MySQL_FOUND FALSE)
endif()
