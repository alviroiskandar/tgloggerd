# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
#
# Unit tests of the stills server's pure parts: request validation, the
# path checks, the frame time and the ffmpeg arguments. Run them in the
# image (scripts/test-stills.sh does), or anywhere with Python 3:
#
#   python3 -m unittest discover -s docker/stills -p 'test_*.py'
import os
import tempfile
import unittest

import server


class ResolvePath(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.store = os.path.join(self.tmp.name, "store")
        os.makedirs(os.path.join(self.store, "0a", "53"))
        self.blob = os.path.join(self.store, "0a", "53", "ab.mp4")
        with open(self.blob, "wb") as f:
            f.write(b"x")
        with open(os.path.join(self.tmp.name, "secret"), "wb") as f:
            f.write(b"s")

    def tearDown(self):
        self.tmp.cleanup()

    def refused(self, rel, code="bad_path"):
        with self.assertRaises(server.Refused) as cm:
            server.resolve_path(self.store, rel)
        self.assertEqual(cm.exception.code, code)

    def test_blob(self):
        self.assertEqual(server.resolve_path(self.store, "0a/53/ab.mp4"),
                         os.path.realpath(self.blob))

    def test_absolute(self):
        self.refused(self.blob)
        self.refused("/etc/passwd")

    def test_dot_dot(self):
        self.refused("../secret")
        self.refused("0a/../../secret")
        self.refused("0a/53/../53/ab.mp4")

    def test_empty_and_odd_parts(self):
        self.refused("")
        self.refused("0a//53/ab.mp4")
        self.refused("./0a/53/ab.mp4")
        self.refused("0a/53/ab.mp4\0")
        self.refused(None)
        self.refused(42)

    def test_symlink_out(self):
        os.symlink(os.path.join(self.tmp.name, "secret"),
                   os.path.join(self.store, "0a", "link"))
        self.refused("0a/link")

    def test_missing_and_directory(self):
        self.refused("0a/53/none.mp4", "not_found")
        self.refused("0a/53", "not_found")


class ParseRequest(unittest.TestCase):
    def test_good(self):
        self.assertEqual(server.parse_request(
            '{"path": "0a/x.mp4", "kind": "video", "duration": 7}'),
            ("0a/x.mp4", "video", 7.0))
        self.assertEqual(server.parse_request(
            '{"path": "0a/x.webm", "kind": "sticker"}'),
            ("0a/x.webm", "sticker", 0.0))

    def bad(self, line):
        with self.assertRaises(server.Refused) as cm:
            server.parse_request(line)
        self.assertEqual(cm.exception.code, "bad_request")

    def test_bad(self):
        self.bad("not json")
        self.bad("[1, 2]")
        self.bad('{"path": "a", "kind": "photo"}')
        self.bad('{"path": "a"}')
        self.bad('{"path": "a", "kind": "video", "duration": -1}')
        self.bad('{"path": "a", "kind": "video", "duration": "3"}')
        self.bad('{"path": "a", "kind": "video", "duration": true}')
        self.bad('{"path": "a", "kind": "video", "duration": NaN}')


class FrameTime(unittest.TestCase):
    def test_frame_time(self):
        self.assertEqual(server.frame_time(10), 1.0)
        self.assertEqual(server.frame_time(2), 1.0)
        self.assertEqual(server.frame_time(1.5), 0.75)
        self.assertEqual(server.frame_time(0.3), 0.15)
        self.assertEqual(server.frame_time(0), 0.0)
        self.assertEqual(server.frame_time(float("nan")), 0.0)


class FfmpegArgs(unittest.TestCase):
    def test_seek_before_input(self):
        a = server.ffmpeg_args("/store/x.mp4", "video", 1.0, "h264")
        self.assertLess(a.index("-ss"), a.index("-i"))
        self.assertEqual(a[a.index("-ss") + 1], "1.000")
        self.assertEqual(a[a.index("-i") + 1], "file:/store/x.mp4")

    def test_jpeg(self):
        a = server.ffmpeg_args("/store/x.mp4", "video_note", 1.0, "h264")
        self.assertEqual(a[a.index("-c:v") + 1], "mjpeg")
        self.assertEqual(a[a.index("-q:v") + 1], "5")
        self.assertEqual(server.output_format("animation"), "jpg")

    def test_sticker_png_with_alpha_decoder(self):
        a = server.ffmpeg_args("/store/x.webm", "sticker", 0.5, "vp9")
        # The decoder comes before the input, the encoder after it.
        self.assertEqual(a[a.index("-c:v") + 1], "libvpx-vp9")
        self.assertLess(a.index("-c:v"), a.index("-i"))
        self.assertIn("png", a[a.index("-i"):])
        self.assertEqual(server.output_format("sticker"), "png")

    def test_scale_never_upscales(self):
        f = server.scale_filter()
        self.assertIn("min(320,iw)", f)
        self.assertIn("min(320,ih)", f)


if __name__ == "__main__":
    unittest.main()
