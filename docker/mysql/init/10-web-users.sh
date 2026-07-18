#!/bin/bash
#
# MySQL first-initialization hook: create the web interface's own database and
# its two restricted users from the .env credentials.
#
# The official MySQL image runs every executable file in
# /docker-entrypoint-initdb.d exactly once -- only while the data directory is
# empty, and after it has created MYSQL_DATABASE / MYSQL_USER. On later starts
# the data directory already exists, so this does not run again and the
# accounts keep whatever they were first initialized with.
set -euo pipefail

: "${MYSQL_ROOT_PASSWORD:?}" "${MYSQL_DATABASE:?}"
: "${WEB_DB_RO_USER:?}" "${WEB_DB_RO_PASSWORD:?}"
: "${WEB_DB_APP_USER:?}" "${WEB_DB_APP_PASSWORD:?}" "${WEB_DB_APP_NAME:?}"

mysql --protocol=socket -uroot -p"${MYSQL_ROOT_PASSWORD}" <<SQL
CREATE DATABASE IF NOT EXISTS \`${WEB_DB_APP_NAME}\`
  CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;

-- Read-only browsing over the logger's schema.
CREATE USER IF NOT EXISTS '${WEB_DB_RO_USER}'@'%' IDENTIFIED BY '${WEB_DB_RO_PASSWORD}';
GRANT SELECT ON \`${MYSQL_DATABASE}\`.* TO '${WEB_DB_RO_USER}'@'%';

-- Read-write over the web app's own schema (accounts, audit). ALL is needed to
-- run the web migrations; at run time the app performs only DML.
CREATE USER IF NOT EXISTS '${WEB_DB_APP_USER}'@'%' IDENTIFIED BY '${WEB_DB_APP_PASSWORD}';
GRANT ALL PRIVILEGES ON \`${WEB_DB_APP_NAME}\`.* TO '${WEB_DB_APP_USER}'@'%';

FLUSH PRIVILEGES;
SQL
