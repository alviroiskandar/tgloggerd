#!/bin/bash
set -xe;

# Listener. Bind to localhost and terminate TLS in a reverse proxy (see
# README.web); do not expose this port directly.
export WEB_LISTEN_ADDR="127.0.0.1";       # bind address
export WEB_LISTEN_PORT="8080";            # bind port
export WEB_THREADS="4";                   # event-loop threads

# Database server. WEB_DB_HOST/PORT fall back to TG_DB_HOST/PORT when unset,
# so a shared deployment can rely on the daemon's variables.
export WEB_DB_HOST="10.0.88.3";           # MySQL host
export WEB_DB_PORT="3306";                # MySQL port

# Read-only client over the logger's schema (GRANT SELECT ON tgloggerd.*).
export WEB_DB_RO_NAME="tgloggerd";        # logger database name
export WEB_DB_RO_USER="web_ro";           # read-only user
export WEB_DB_RO_PASSWORD="";             # read-only user password
export WEB_DB_RO_CONNS="4";               # connection pool size

# Read-write client over the web app's own schema (accounts, audit).
export WEB_DB_APP_NAME="tgloggerd_web";   # web database name
export WEB_DB_APP_USER="web_app";         # read-write user
export WEB_DB_APP_PASSWORD="";            # read-write user password
export WEB_DB_APP_CONNS="2";              # connection pool size

# Sessions and login.
export WEB_SESSION_TIMEOUT="43200";       # session idle timeout (s); 0 = never
export WEB_SECURE_COOKIE="1";             # 1 sets Secure on the cookie (needs HTTPS)
export WEB_LOGIN_MAX_ATTEMPTS="5";        # failed logins per (ip, username)...
export WEB_LOGIN_WINDOW="900";            # ...within this many seconds

# Assets and storage.
export WEB_TEMPLATE_DIR="views/templates"; # inja templates
export WEB_STATIC_DIR="static";            # served at / (style.css, ...)
export WEB_STORAGE_DIR="../data/storage/files"; # MUST equal the daemon's TG_STORAGE_DIR

chrt --idle 0 nice -n 19 ionice -c 3 bash -c "cmake -B build && cmake --build build -j$(nproc)";

# Create or reset the first admin, then start the server. Comment out the
# seeding line after the initial setup.
# ./build/tgloggerd_web --seed-admin admin;
exec build/tgloggerd_web;
