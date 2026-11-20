#!/usr/bin/env bash
#
# Test of the stills service (docker/stills/, docker-compose.yml service
# stills) as it is deployed: builds its image, runs the server's unit
# tests in it, makes sample files with the image's own ffmpeg (an MPEG-4
# video, a transparent VP9 WebM sticker, a 0.3 s clip, a file that is no
# video, a noisy sticker whose still is over the size cap), starts the
# server with the compose service's sandbox (no network, read-only store
# and root, no capabilities, the same limits) and checks every answer
# over its socket, including path escapes.
#
# Everything runs under a work directory and container names of its own,
# so it never touches a running deployment; all of it is removed on exit,
# the image too unless KEEP_IMAGE=1. Set TMPDIR to choose where the work
# directory goes, STILLS_TEST_IMAGE to name the image.
#
# Usage: scripts/test-stills.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/tgld-stills-test.XXXXXX")
NAME="tgld-stills-test-$(basename "$WORK" | tr 'A-Z' 'a-z' | tr -dc 'a-z0-9' | tail -c 12)"
IMAGE=${STILLS_TEST_IMAGE:-$NAME:local}
U="$(id -u):$(id -g)"

cleanup() {
	docker rm -f "$NAME" >/dev/null 2>&1 || true
	[ "${KEEP_IMAGE:-0}" = 1 ] || docker rmi -f "$IMAGE" >/dev/null 2>&1 || true
	rm -rf "${WORK:?}"
}
trap cleanup EXIT

fail() {
	echo "FAIL: $*" >&2
	docker logs "$NAME" 2>&1 | tail -n 30 >&2 || true
	exit 1
}

# A one-off container of the image, as us, without a network.
tool() {
	local ep=$1
	shift
	docker run --rm --network none --user "$U" --entrypoint "$ep" \
		-v "$WORK":/work -w /work "$IMAGE" "$@"
}

echo "== building $IMAGE"
docker build -q -t "$IMAGE" "$ROOT/docker/stills" >/dev/null

echo "== unit tests"
docker run --rm --network none --user "$U" --entrypoint python3 \
	-v "$ROOT/docker/stills":/src:ro -e PYTHONDONTWRITEBYTECODE=1 \
	-w /src "$IMAGE" -m unittest -q test_server

echo "== sample files"
mkdir -p "$WORK/store/0a/53" "$WORK/run" "$WORK/outside"
chmod 0777 "$WORK/run"
tool sh -c '
set -e
ff() { ffmpeg -nostdin -v error -y "$@"; }
s=store/0a/53
ff -f lavfi -i testsrc2=size=640x360:rate=25:duration=5 -c:v libx264 -pix_fmt yuv420p $s/video.mp4
ff -f lavfi -i testsrc2=size=180x240:rate=25:duration=3 -c:v libx264 -pix_fmt yuv420p $s/small.mp4
ff -f lavfi -i testsrc2=size=320x320:rate=30:duration=0.3 -c:v libx264 -pix_fmt yuv420p $s/short.mp4
ff -f lavfi -i "color=c=red@0.0:size=512x512:rate=30:duration=2,format=rgba,drawbox=x=128:y=128:w=256:h=256:color=blue@1.0:t=fill" \
	-c:v libvpx-vp9 -pix_fmt yuva420p -auto-alt-ref 0 $s/sticker.webm
ff -f lavfi -i "nullsrc=size=512x512:rate=30:duration=1,geq=random(1)*255:random(2)*255:random(3)*255,format=yuva420p" \
	-c:v libvpx-vp9 -lossless 1 -pix_fmt yuva420p -auto-alt-ref 0 $s/noise.webm
printf "not a video\n" > $s/text.mp4
printf "secret\n" > outside/secret.mp4
ln -s ../../../outside/secret.mp4 $s/escape.mp4
' sh

echo "== starting the server"
docker run -d --name "$NAME" --user "$U" --init --network none --read-only \
	--tmpfs /tmp --cap-drop ALL --security-opt no-new-privileges:true \
	--pids-limit 64 --memory 256m --cpus 0.5 \
	-v "$WORK/store":/store:ro -v "$WORK/run":/run/tgloggerd-stills \
	"$IMAGE" >/dev/null
timeout 30 bash -c "until [ -S '$WORK/run/stills.sock' ]; do sleep 0.5; done" ||
	fail "no socket after 30 s"

# The client: one request per case, the answer summed up as one JSON line
# (the header, and for an image its size, dimensions and PNG colour type).
cat >"$WORK/client.py" <<'EOF'
import json, socket, struct, sys, zlib

def ask(line):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(60)
    s.connect("/run/tgloggerd-stills/stills.sock")
    s.sendall(line.encode() + b"\n")
    buf = b""
    while True:
        c = s.recv(65536)
        if not c:
            break
        buf += c
    s.close()
    head, _, body = buf.partition(b"\n")
    h = json.loads(head)
    if h.get("ok"):
        h["got"] = len(body)
        if body[:8] == b"\x89PNG\r\n\x1a\n":
            w, hh, depth, ctype = struct.unpack(">IIBB", body[16:26])
            h.update(width=w, height=hh, color_type=ctype)
            # The top left pixel's alpha: every PNG filter leaves the
            # first pixel of the first row as it is.
            idat, i = b"", 8
            while i < len(body):
                n, t = struct.unpack(">I4s", body[i:i + 8])
                if t == b"IDAT":
                    idat += body[i + 8:i + 8 + n]
                i += 12 + n
            if ctype == 6 and depth == 8:
                h["corner_alpha"] = zlib.decompress(idat)[4]
        elif body[:2] == b"\xff\xd8":
            i = 2
            while i < len(body):
                m, n = body[i + 1], struct.unpack(">H", body[i + 2:i + 4])[0]
                if 0xC0 <= m <= 0xC2:
                    hh, w = struct.unpack(">HH", body[i + 5:i + 9])
                    h.update(width=w, height=hh)
                    break
                i += 2 + n
    return h

for line in sys.stdin:
    print(json.dumps(ask(line.strip())), flush=True)
EOF

ask() {
	printf '%s\n' "$1" | docker run --rm -i --network none --user "$U" \
		--entrypoint python3 -v "$WORK/run":/run/tgloggerd-stills \
		-v "$WORK/client.py":/client.py:ro "$IMAGE" -I /client.py
}

# expect <request> <python condition on the answer h> <what>
expect() {
	local got
	got=$(ask "$1") || fail "$3: no answer"
	if printf '%s' "$got" | docker run --rm -i --network none \
		--entrypoint python3 "$IMAGE" -I -c \
		"import json,sys; h=json.load(sys.stdin); sys.exit(0 if ($2) else 1)"; then
		echo "  ok:   $3 ($got)"
	else
		fail "$3: $got"
	fi
}

echo "== answers"
p=0a/53
expect "{\"path\":\"$p/video.mp4\",\"kind\":\"video\",\"duration\":5}" \
	'h["ok"] and h["format"] == "jpg" and h["size"] == h["got"] and (h["width"], h["height"]) == (320, 180)' \
	"a 640x360 video gives a 320x180 JPEG"
expect "{\"path\":\"$p/video.mp4\",\"kind\":\"video\"}" \
	'h["ok"] and h["format"] == "jpg"' \
	"a video without a duration (ffprobe's is used)"
expect "{\"path\":\"$p/small.mp4\",\"kind\":\"video_note\",\"duration\":3}" \
	'h["ok"] and (h["width"], h["height"]) == (180, 240)' \
	"a video smaller than 320 px is not upscaled"
expect "{\"path\":\"$p/short.mp4\",\"kind\":\"animation\",\"duration\":0.3}" \
	'h["ok"] and h["format"] == "jpg" and h["got"] > 0' \
	"a 0.3 s clip"
expect "{\"path\":\"$p/short.mp4\",\"kind\":\"animation\",\"duration\":30}" \
	'h["ok"] and h["got"] > 0' \
	"a duration past the end falls back to the first frame"
expect "{\"path\":\"$p/sticker.webm\",\"kind\":\"sticker\",\"duration\":2}" \
	'h["ok"] and h["format"] == "png" and h["color_type"] == 6 and h["corner_alpha"] == 0 and (h["width"], h["height"]) == (320, 320)' \
	"a transparent WebM sticker gives a PNG that keeps its transparency"
expect "{\"path\":\"$p/noise.webm\",\"kind\":\"sticker\",\"duration\":1}" \
	'not h["ok"] and h["error"] == "too_large"' \
	"a still over 200 KB is refused"
expect "{\"path\":\"$p/text.mp4\",\"kind\":\"video\",\"duration\":1}" \
	'not h["ok"] and h["error"] == "no_video"' \
	"a file that is no video is refused"
expect "{\"path\":\"$p/escape.mp4\",\"kind\":\"video\"}" \
	'not h["ok"] and h["error"] == "bad_path"' \
	"a symlink out of the store is refused"
expect "{\"path\":\"../outside/secret.mp4\",\"kind\":\"video\"}" \
	'not h["ok"] and h["error"] == "bad_path"' \
	"a path with .. is refused"
expect "{\"path\":\"/store/$p/video.mp4\",\"kind\":\"video\"}" \
	'not h["ok"] and h["error"] == "bad_path"' \
	"an absolute path is refused"
expect "{\"path\":\"$p/none.mp4\",\"kind\":\"video\"}" \
	'not h["ok"] and h["error"] == "not_found"' \
	"a missing blob"
expect "{\"path\":\"$p/video.mp4\",\"kind\":\"photo\"}" \
	'not h["ok"] and h["error"] == "bad_request"' \
	"an unknown kind"
expect "garbage" \
	'not h["ok"] and h["error"] == "bad_request"' \
	"a request that is no JSON"

echo "== the sandbox"
docker inspect "$NAME" --format '{{.State.Running}}' | grep -qx true ||
	fail "the server is no longer running"
echo "  ok:   the server is still running"
echo "all checks passed"
