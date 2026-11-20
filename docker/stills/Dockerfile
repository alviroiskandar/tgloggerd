# syntax=docker/dockerfile:1
#
# The stills service (docker-compose.yml, service stills): takes a still
# frame from a stored video for the daemon, with Debian's ffmpeg, behind
# the small socket server in server.py.
#
# The server is Python because the protocol is JSON and the jobs are
# ffprobe and ffmpeg runs with a time limit, which the standard library
# covers (json, socket, subprocess). python3-minimal alone has no json
# module, so the image takes python3 (its standard library, a few MB)
# without recommends; nothing else is added. The base image is pinned by
# digest, so a rebuild gets the same Debian release.
FROM debian:bookworm-slim@sha256:734a72b3d6142a71993b4bc580ca01d8ca687f77a2c422d96a961d3376b179bc

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
		ffmpeg python3 \
	&& rm -rf /var/lib/apt/lists/*

COPY server.py /usr/local/lib/tgloggerd-stills/server.py

# docker-compose.yml runs it as the daemon's uid; nobody otherwise.
USER 65534:65534
ENTRYPOINT ["python3", "-I", "/usr/local/lib/tgloggerd-stills/server.py"]
