# syntax=docker/dockerfile:1
#
# Container image for the tgloggerd web interface.
#
# The build context is the REPOSITORY ROOT (not web/), because the web project
# builds the vendored Drogon and MariaDB Connector/C from ../submodules. Build
# it from the repo root, e.g.:
#
#     docker build -f web/Dockerfile -t tgloggerd-web .
#
# or via the "web" service in docker-compose.example.yml.

# ---------------------------------------------------------------------------
# Build stage: compile the web binary (statically links the MariaDB client).
# ---------------------------------------------------------------------------
FROM ubuntu:22.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
	build-essential cmake git ca-certificates pkg-config \
	libsodium-dev libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev libbrotli-dev \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /src
# Only the pieces the web app needs (the daemon-only submodules td and
# mysql-connector-cpp are excluded via .dockerignore and never copied).
COPY submodules/drogon              submodules/drogon
COPY submodules/mariadb-connector-c submodules/mariadb-connector-c
COPY web                            web

WORKDIR /src/web
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
	&& cmake --build build -j"$(nproc)"

# ---------------------------------------------------------------------------
# Runtime stage: only the shared libraries the binary links at run time.
# ---------------------------------------------------------------------------
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
	libsodium23 libjsoncpp25 libuuid1 zlib1g libssl3 libbrotli1 \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/web/build/tgloggerd_web /app/tgloggerd_web
COPY --from=build /src/web/views               /app/views
COPY --from=build /src/web/static              /app/static

# Listen on all interfaces inside the container. Terminate TLS in a reverse
# proxy in front of it (see README.web); WEB_SECURE_COOKIE stays 1 there.
ENV WEB_LISTEN_ADDR=0.0.0.0 \
	WEB_LISTEN_PORT=8080 \
	WEB_TEMPLATE_DIR=/app/views/templates \
	WEB_STATIC_DIR=/app/static \
	WEB_STORAGE_DIR=/data/storage/files

EXPOSE 8080

# Runs as root so it can read the daemon's storage files (typically mode 0640)
# mounted read-only. To harden, run with a user that can read that mount, e.g.
# `docker run --user "$(id -u):$(id -g)" ...` or `user:` in compose.
ENTRYPOINT ["/app/tgloggerd_web"]
