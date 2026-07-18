# syntax=docker/dockerfile:1
#
# Toolchain image for tgloggerd (the daemon) and its web interface.
#
# This image carries ONLY the compiler toolchain and the build/runtime
# dependencies -- it does not contain any of the project's own source. The
# source tree and the build/ directories are bind-mounted at run time (see
# docker-compose.yml) and compiled by run.sh on container start, so editing
# the source never requires rebuilding this image. Both the daemon and the
# web app run from this same image.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Toolchain + dependencies for BOTH components:
#
#   daemon: TDLib (gperf, OpenSSL, zlib) and MySQL Connector/C++'s legacy JDBC
#           API, which wraps the Oracle client -- libmysqlclient-dev. Do NOT
#           add libmariadb-dev: it replaces the Oracle headers and breaks the
#           connector build (see README).
#   web:    Drogon plus the vendored MariaDB Connector/C (libsodium, jsoncpp,
#           uuid, brotli), sharing zlib and OpenSSL with the daemon.
#
# git is needed because run.sh initializes the vendored submodules on first
# start; wget fetches golang-migrate below.
RUN apt-get update && apt-get install -y --no-install-recommends \
		build-essential cmake git ca-certificates pkg-config wget \
		gperf libssl-dev zlib1g-dev libmysqlclient-dev \
		libsodium-dev libjsoncpp-dev uuid-dev libbrotli-dev \
	&& rm -rf /var/lib/apt/lists/*

# golang-migrate applies the SQL migrations on container start. The release
# tarball's binary already bundles the MySQL driver, so no Go toolchain is
# needed in the image.
ARG MIGRATE_VERSION=v4.18.1
RUN set -eux; \
	wget -qO /tmp/migrate.tgz \
		"https://github.com/golang-migrate/migrate/releases/download/${MIGRATE_VERSION}/migrate.linux-amd64.tar.gz"; \
	mkdir -p /tmp/migrate; \
	tar -xzf /tmp/migrate.tgz -C /tmp/migrate; \
	mv "$(find /tmp/migrate -type f -name 'migrate*' | head -n1)" /usr/local/bin/migrate; \
	chmod +x /usr/local/bin/migrate; \
	rm -rf /tmp/migrate /tmp/migrate.tgz; \
	migrate -version

# The repository is bind-mounted here at run time; run.sh lives at its root.
WORKDIR /workspace
ENTRYPOINT ["./run.sh"]
