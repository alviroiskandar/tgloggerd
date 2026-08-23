#!/usr/bin/env bash
#
# Unified container entrypoint for tgloggerd.
#
# It initializes the vendored git submodules the requested component needs,
# builds it (incrementally, into a bind-mounted build/ directory), applies the
# database migrations, and then execs the binary. All configuration comes from
# the environment -- see .env.example.
#
# Usage: run.sh <daemon|web|discord>
#
#   daemon  Build and run the logger daemon (src/, build/). On first login it
#           prompts on stdin for the phone number, code and 2FA password, so
#           run it interactively the first time: `docker compose run --rm
#           tgloggerd`.
#   web     Build and run the web interface (web/, web/build/).
#   discord Build and run discordd, the Discord daemon: logs Discord
#           messages and forwards them to Telegram. Shares build/ with
#           the daemon so TDLib is compiled only once.
#
set -euo pipefail

MODE="${1:-daemon}"

# This script lives at the repository root, which is the bind mount; run
# everything from there so the relative paths below (build/, migrations/, ...)
# and the *_DIR environment variables resolve consistently for both modes.
cd "$(dirname "$(readlink -f "$0")")"

# The container may run as a uid with no /etc/passwd entry (see TG_UID in the
# compose file), in which case HOME defaults to "/" and is not writable. Point
# it somewhere writable so `git config --global` below can store its config.
if [ ! -w "${HOME:-/}" ]; then
	export HOME=/tmp
fi

log() { printf '\n\033[1;32m=== %s ===\033[0m\n' "$*"; }

NPROC="$(nproc)"

# Compile without starving the rest of the host. nice needs no privileges;
# swallow a failure just in case it is unavailable.
build_cmd() { nice -n 19 bash -c "$1" || bash -c "$1"; }

# cmake bakes absolute paths into <dir>/CMakeCache.txt. If the directory was
# configured for a different path -- e.g. a native `cmake -B build` run on the
# host before switching to this container, where the same files are mounted at
# a different location -- cmake refuses to reuse the cache. Detect that and
# start clean so the build is not wedged.
prepare_build_dir() {
	local dir="$1" cache="$1/CMakeCache.txt"
	[ -f "$cache" ] || return 0
	if ! grep -qxF "CMAKE_CACHEFILE_DIR:INTERNAL=${PWD}/${dir}" "$cache"; then
		log "Discarding stale CMake cache in ${dir}/ (configured for another path)"
		rm -rf "$dir"
	fi
}

# Populate the submodules a component builds from. A fresh checkout leaves them
# empty, so cmake would fail with "does not contain a CMakeLists.txt file".
# Serialize with a lock on the shared .git so the daemon and web containers,
# which may both start cold, do not race on the git index.
ensure_submodules() {
	local need=0 p
	for p in "$@"; do
		[ -e "$p/CMakeLists.txt" ] || need=1
	done
	[ "$need" -eq 0 ] && return 0

	# This container runs as root while the bind-mounted tree is owned by the
	# host user; without this git refuses to operate ("dubious ownership").
	git config --global --add safe.directory '*' 2>/dev/null || true

	log "Initializing git submodules: $*"
	flock .git/tgld-submodule.lock \
		git submodule update --init --recursive "$@"
}

# Block until the MySQL server accepts TCP connections. migrate and the app
# both retry, but waiting here keeps the logs clean and the migration ordering
# deterministic.
wait_for_mysql() {
	local host="$1" port="$2" i
	log "Waiting for MySQL at ${host}:${port}"
	for i in $(seq 1 60); do
		if (exec 3<>"/dev/tcp/${host}/${port}") 2>/dev/null; then
			exec 3>&- 3<&-
			return 0
		fi
		sleep 2
	done
	echo "run.sh: MySQL at ${host}:${port} did not come up" >&2
	return 1
}

case "$MODE" in
daemon)
	: "${TG_DB_HOST:?TG_DB_HOST is not set}" "${TG_DB_PORT:?TG_DB_PORT is not set}"
	: "${TG_DB_USER:?}" "${TG_DB_PASSWORD:?}" "${TG_DB_NAME:?}"

	ensure_submodules submodules/td submodules/mysql-connector-cpp

	log "Building tgloggerd (this compiles TDLib on the first run and is slow)"
	prepare_build_dir build
	# --target tgloggerd, not everything: build/ also holds discordd, which
	# this container neither runs nor needs, and building it here would
	# make the daemon depend on discordd's own dependencies.
	build_cmd "cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target tgloggerd -j${NPROC}"

	wait_for_mysql "$TG_DB_HOST" "$TG_DB_PORT"
	log "Applying daemon migrations"
	migrate -path migrations \
		-database "mysql://${TG_DB_USER}:${TG_DB_PASSWORD}@tcp(${TG_DB_HOST}:${TG_DB_PORT})/${TG_DB_NAME}" up

	log "Starting tgloggerd"
	exec build/tgloggerd
	;;

web)
	: "${WEB_DB_HOST:?WEB_DB_HOST is not set}" "${WEB_DB_PORT:?WEB_DB_PORT is not set}"
	: "${WEB_DB_APP_USER:?}" "${WEB_DB_APP_PASSWORD:?}" "${WEB_DB_APP_NAME:?}"

	ensure_submodules submodules/drogon submodules/mariadb-connector-c

	log "Building tgloggerd_web (this compiles Drogon on the first run and is slow)"
	prepare_build_dir web/build
	build_cmd "cmake -S web -B web/build -DCMAKE_BUILD_TYPE=Release && cmake --build web/build -j${NPROC}"

	wait_for_mysql "$WEB_DB_HOST" "$WEB_DB_PORT"
	# web_app owns tgloggerd_web, so it can run the web migrations; at run time
	# it performs only DML.
	log "Applying web migrations"
	migrate -path web/migrations \
		-database "mysql://${WEB_DB_APP_USER}:${WEB_DB_APP_PASSWORD}@tcp(${WEB_DB_HOST}:${WEB_DB_PORT})/${WEB_DB_APP_NAME}" up

	log "Starting tgloggerd_web"
	exec web/build/tgloggerd_web
	;;

discord)
	: "${TG_DB_HOST:?TG_DB_HOST is not set}" "${TG_DB_PORT:?TG_DB_PORT is not set}"
	: "${TG_DB_USER:?}" "${TG_DB_PASSWORD:?}" "${TG_DB_NAME:?}"
	: "${DISCORD_BOT_TOKEN:?DISCORD_BOT_TOKEN is not set}"

	# discordd links TDLib (it sends to Telegram as a bot) and the JDBC
	# connector, so it needs the same submodules as the daemon.
	ensure_submodules submodules/td submodules/mysql-connector-cpp

	log "Building discordd (shares build/ with tgloggerd, so TDLib is compiled once)"
	prepare_build_dir build
	# The configure line must match the daemon's exactly: both containers
	# share build/, and a differing cache would make them reconfigure in a
	# loop against each other.
	build_cmd "cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target discordd -j${NPROC}"

	wait_for_mysql "$TG_DB_HOST" "$TG_DB_PORT"
	# The daemon container owns migrations; discordd only reads the schema
	# it produces. Waiting for the table it needs avoids a startup race on
	# a cold deployment where both containers come up together.
	log "Starting discordd"
	exec build/discordd
	;;

*)
	echo "Usage: run.sh <daemon|web|discord>" >&2
	exit 2
	;;
esac
