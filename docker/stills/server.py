# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
#
# The stills service: takes one still frame from a video of the file store
# for the daemon, over a Unix socket (docker-compose.yml, service stills;
# decision docs/design/decisions/2026-11-21-video-stills-format.md).
#
# One request per connection, one JSON line:
#
#   {"path": "<blob path relative to the store>",
#    "kind": "video|animation|video_note|sticker", "duration": <seconds>}
#
# The answer is one JSON header line, then the image bytes:
#
#   {"ok": true, "format": "jpg|png", "size": <bytes>}
#   {"ok": false, "error": "<code>", "message": "<text>"}
#
# Connections are served one after the other, so one job runs at a time.
# ffprobe and ffmpeg are each killed after 20 seconds.
import json
import os
import socket
import subprocess
import sys
import time

STORE = os.environ.get("STILLS_STORE", "/store")
SOCKET = os.environ.get("STILLS_SOCKET", "/run/tgloggerd-stills/stills.sock")

KINDS = ("video", "animation", "video_note", "sticker")
TOOL_TIMEOUT = 20
# The long side of a still, in pixels; smaller videos are not upscaled.
MAX_SIDE = 320
# ffmpeg's MJPEG -q:v 5: its quantization table is the nearest to
# libjpeg's quality 80 without being coarser (see the decision file).
JPEG_QSCALE = 5
MAX_OUTPUT = 200 * 1024
MAX_REQUEST = 4096
# The demuxers a stored video comes in (ffprobe's format_name lists
# several names for one demuxer); anything else, such as a playlist that
# names other files, is refused.
FORMATS = {"mov", "mp4", "m4a", "3gp", "3g2", "mj2", "matroska", "webm",
           "gif", "avi", "flv", "mpegts", "ogg"}


class Refused(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


def resolve_path(store, rel):
    """The absolute path of blob @rel in @store, or Refused."""
    if not isinstance(rel, str) or not rel:
        raise Refused("bad_path", "path must be a non-empty string")
    if "\0" in rel or rel.startswith("/"):
        raise Refused("bad_path", "path must be relative to the store")
    parts = rel.split("/")
    if any(p in ("", ".", "..") for p in parts):
        raise Refused("bad_path", "path must not have empty, . or .. parts")
    root = os.path.realpath(store)
    full = os.path.realpath(os.path.join(root, rel))
    if not full.startswith(root + os.sep):
        raise Refused("bad_path", "path leads outside the store")
    if not os.path.isfile(full):
        raise Refused("not_found", "no such file in the store")
    return full


def parse_request(line):
    """(path, kind, duration) from one request line, or Refused."""
    try:
        req = json.loads(line)
    except ValueError:
        raise Refused("bad_request", "the request is not JSON")
    if not isinstance(req, dict):
        raise Refused("bad_request", "the request is not a JSON object")
    kind = req.get("kind")
    if kind not in KINDS:
        raise Refused("bad_request", "kind must be one of " + ", ".join(KINDS))
    duration = req.get("duration", 0)
    if isinstance(duration, bool) or not isinstance(duration, (int, float)) \
            or duration != duration or duration < 0:
        raise Refused("bad_request", "duration must be a number >= 0")
    return req.get("path"), kind, float(duration)


def frame_time(duration):
    """Where the still is taken: at 1 s, or halfway into a shorter clip."""
    if not duration > 0:
        return 0.0
    return min(1.0, duration / 2.0)


def output_format(kind):
    # A video sticker is a VP9 WebM that may be transparent: PNG keeps it.
    return "png" if kind == "sticker" else "jpg"


def scale_filter():
    # Fit MAX_SIDE on the long side, keeping the aspect ratio (-1), and
    # never upscale (min with the input size).
    s = str(MAX_SIDE)
    return ("scale=w='if(gte(iw,ih),min(%s,iw),-1)'"
            ":h='if(gte(iw,ih),-1,min(%s,ih))'" % (s, s))


def probe_args(path):
    return ["ffprobe", "-v", "error", "-protocol_whitelist", "file",
            "-show_entries", "stream=codec_type,codec_name:format=format_name,duration",
            "-of", "json", "file:" + path]


def ffmpeg_args(path, kind, at, codec):
    # One thread each for decoding, filtering and encoding (the -threads
    # before -i is the decoder's, the one after it the encoder's): left to
    # itself ffmpeg starts threads by the host's CPU count, past the
    # service's pids limit.
    args = ["ffmpeg", "-nostdin", "-v", "error", "-filter_threads", "1",
            "-threads", "1", "-protocol_whitelist", "file"]
    if kind == "sticker" and codec in ("vp8", "vp9"):
        # FFmpeg's own VP8/VP9 decoders drop the alpha channel; libvpx's
        # keep it.
        args += ["-c:v", "libvpx-vp9" if codec == "vp9" else "libvpx"]
    args += ["-ss", "%.3f" % at, "-i", "file:" + path,
             "-map", "0:v:0", "-frames:v", "1", "-an", "-sn", "-dn",
             "-vf", scale_filter(), "-threads", "1"]
    if output_format(kind) == "png":
        args += ["-c:v", "png", "-pix_fmt", "rgba"]
    else:
        args += ["-c:v", "mjpeg", "-q:v", str(JPEG_QSCALE),
                 "-pix_fmt", "yuvj420p"]
    return args + ["-f", "image2pipe", "pipe:1"]


def run_tool(args):
    try:
        p = subprocess.run(args, stdin=subprocess.DEVNULL,
                           capture_output=True, timeout=TOOL_TIMEOUT)
    except subprocess.TimeoutExpired:
        # subprocess.run has killed it.
        raise Refused("timeout", "%s ran past %d s" % (args[0], TOOL_TIMEOUT))
    return p


def probe(path):
    """(video codec, duration in seconds or 0) of @path, or Refused."""
    p = run_tool(probe_args(path))
    if p.returncode != 0:
        raise Refused("no_video", "ffprobe cannot read the file")
    try:
        info = json.loads(p.stdout or b"{}")
    except ValueError:
        raise Refused("failed", "ffprobe gave no JSON")
    fmt = info.get("format") or {}
    names = set(str(fmt.get("format_name", "")).split(","))
    if not names & FORMATS:
        raise Refused("no_video", "not a video container: %s"
                      % fmt.get("format_name", "unknown"))
    video = [s for s in info.get("streams") or []
             if s.get("codec_type") == "video"]
    if not video:
        raise Refused("no_video", "the file has no video stream")
    try:
        duration = float(fmt.get("duration") or 0)
    except ValueError:
        duration = 0.0
    return str(video[0].get("codec_name", "")), max(duration, 0.0)


def make_still(store, path, kind, duration):
    """(format, bytes) of the still of blob @path, or Refused."""
    full = resolve_path(store, path)
    codec, probed = probe(full)
    if not duration > 0:
        duration = probed
    at = frame_time(duration)
    out = b""
    # A duration that overstates the clip leaves nothing at the seek
    # point: take the first frame then.
    for t in ((at, 0.0) if at > 0 else (0.0,)):
        p = run_tool(ffmpeg_args(full, kind, t, codec))
        if p.returncode != 0:
            err = p.stderr.decode("utf-8", "replace").strip().splitlines()
            raise Refused("failed", "ffmpeg failed: " +
                          (err[-1] if err else "exit %d" % p.returncode))
        out = p.stdout
        if out:
            break
    if not out:
        raise Refused("failed", "ffmpeg wrote no frame")
    if len(out) > MAX_OUTPUT:
        raise Refused("too_large", "the still is %d bytes, over %d"
                      % (len(out), MAX_OUTPUT))
    return output_format(kind), out


def read_line(conn):
    buf = b""
    while b"\n" not in buf:
        chunk = conn.recv(1024)
        if not chunk:
            break
        buf += chunk
        if len(buf) > MAX_REQUEST:
            raise Refused("bad_request", "the request line is too long")
    return buf.split(b"\n", 1)[0].decode("utf-8", "replace")


def header(obj):
    return (json.dumps(obj, separators=(",", ":")) + "\n").encode()


def serve_one(conn, store):
    conn.settimeout(10)
    started = time.monotonic()
    path = None
    try:
        path, kind, duration = parse_request(read_line(conn))
        fmt, data = make_still(store, path, kind, duration)
        reply = header({"ok": True, "format": fmt, "size": len(data)}) + data
        result = "%s %d bytes" % (fmt, len(data))
    except Refused as e:
        reply = header({"ok": False, "error": e.code, "message": e.message})
        result = "refused %s: %s" % (e.code, e.message)
    try:
        conn.sendall(reply)
    except OSError as e:
        result += " (not sent: %s)" % e
    log("%s: %s (%d ms)" % (path if isinstance(path, str) else "-", result,
                            (time.monotonic() - started) * 1000))


def log(msg):
    print("stills: " + msg, file=sys.stderr, flush=True)


def main():
    try:
        os.unlink(SOCKET)
    except FileNotFoundError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(SOCKET)
    os.chmod(SOCKET, 0o660)
    srv.listen(8)
    log("listening on %s, store %s" % (SOCKET, STORE))
    while True:
        conn, _ = srv.accept()
        with conn:
            try:
                serve_one(conn, STORE)
            except OSError as e:
                log("connection failed: %s" % e)


if __name__ == "__main__":
    main()
