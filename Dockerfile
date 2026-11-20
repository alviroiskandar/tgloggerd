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
#
# Ubuntu 26.04 is the base for its OpenSSL 3.5, the current long-term-support
# series; 3.0, which Ubuntu 24.04 ships, is out of upstream support.
FROM ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive

# CMake 4.x refuses to configure a project that declares a minimum version
# below 3.5. MySQL Connector/C++ configures its bundled protobuf as an
# external build whose CMakeLists.txt declares cmake_minimum_required(3.1),
# so the daemon's tree would not configure at all. CMake reads this variable
# from the environment, which also reaches that nested configure; it applies
# the 3.5 policies to such projects instead of stopping. Projects that
# declare 3.5 or later are unaffected.
ENV CMAKE_POLICY_VERSION_MINIMUM=3.5

# Toolchain + dependencies for BOTH components:
#
#   daemon: TDLib (gperf, OpenSSL, zlib) and MySQL Connector/C++'s legacy JDBC
#           API, which wraps the Oracle client -- libmysqlclient-dev. Do NOT
#           add libmariadb-dev: it replaces the Oracle headers and breaks the
#           connector build (see README).
#   web:    Drogon plus the vendored MariaDB Connector/C (libsodium, jsoncpp,
#           uuid, brotli), sharing zlib and OpenSSL with the daemon. It also
#           links gwmcp, whose public API names nlohmann/json; the packaged
#           version (3.11.3) matches the copy web/ vendors, so the type crosses
#           that library boundary unconverted.
#   discordd: gwdiscord speaks the Discord Gateway over a WebSocket, which the
#           system libcurl cannot do -- Ubuntu builds it without websocket
#           support. Boost.Beast provides it; only headers are needed, since
#           Beast and Asio are header-only, so nothing links a Boost binary.
#
# git is needed because run.sh initializes the vendored submodules on first
# start; wget fetches golang-migrate below.
RUN apt-get update && apt-get install -y --no-install-recommends \
		build-essential cmake git ca-certificates pkg-config wget \
		gperf libssl-dev zlib1g-dev libmysqlclient-dev libcurl4-openssl-dev \
		libsodium-dev libjsoncpp-dev uuid-dev libbrotli-dev \
		libboost-dev nlohmann-json3-dev \
	&& rm -rf /var/lib/apt/lists/*

# golang-migrate applies the SQL migrations on container start. The release
# tarball's binary already bundles the MySQL driver, so it is not built
# from source.
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

# The Go toolchain builds the Go web service (webapi/) on container start,
# the way the C++ components are built. The version is pinned and the
# tarball checked against the checksum go.dev/dl publishes for it, so a
# rebuilt image gets the same compiler. GOTOOLCHAIN=local keeps the go
# command from downloading another toolchain when a go.mod asks for a
# newer one: the build fails instead, and the pin here is raised.
ARG GO_VERSION=1.27.4
ARG GO_SHA256=7226736c2339719cda28fbf3a54d71aa63bcefe376e98cef00034268dab24840
RUN set -eux; \
	wget -qO /tmp/go.tgz "https://go.dev/dl/go${GO_VERSION}.linux-amd64.tar.gz"; \
	echo "${GO_SHA256}  /tmp/go.tgz" | sha256sum -c -; \
	tar -xzf /tmp/go.tgz -C /usr/local; \
	rm -f /tmp/go.tgz; \
	/usr/local/go/bin/go version
ENV PATH="/usr/local/go/bin:${PATH}" \
	GOTOOLCHAIN=local

# The repository is bind-mounted here at run time; run.sh lives at its root.
WORKDIR /workspace
ENTRYPOINT ["./run.sh"]
