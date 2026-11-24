#!/usr/bin/env python3
"""Offline tests for tools/game-stage.py and tools/devportal.py.

    python3 -m unittest discover -s tests/py_tools -v

No console, no network: the transport is faked, so these tests cover the
logic that decides what gets staged and what gets uploaded.
"""

import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(REPO, "tools")
sys.path.insert(0, TOOLS)

import devportal  # noqa: E402


def write_pe(path, machine=0x8664):
    """A minimal file that passes the header check in game-stage.py."""
    dos = bytearray(64)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x40)
    pe = b"PE\0\0" + struct.pack("<H", machine) + b"\0" * 16
    with open(path, "wb") as handle:
        handle.write(bytes(dos) + pe)


class TestGameStage(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="stage-")
        self.game = os.path.join(self.tmp, "Game")
        os.makedirs(os.path.join(self.game, "bin", "x64"))
        os.makedirs(os.path.join(self.game, "data"))
        write_pe(os.path.join(self.game, "bin", "x64", "game.exe"))
        with open(os.path.join(self.game, "data", "pack.dat"), "wb") as handle:
            handle.write(b"payload")

    def stage(self, *extra):
        return subprocess.run(
            [sys.executable, os.path.join(TOOLS, "game-stage.py"),
             self.game, "--out", os.path.join(self.tmp, "out"), *extra],
            capture_output=True, text=True)

    def test_stages_tree_and_writes_args(self):
        result = self.stage("--exe", "bin/x64/game.exe",
                            "--args", "--load-game save.zip")
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
        out = os.path.join(self.tmp, "out")
        self.assertTrue(os.path.isfile(os.path.join(out, "bin", "x64", "game.exe")))
        self.assertTrue(os.path.isfile(os.path.join(out, "data", "pack.dat")))
        with open(os.path.join(out, "tiramisu_args.txt"), encoding="utf-8") as f:
            body = f.read()
        self.assertIn("--load-game save.zip", body)
        with open(os.path.join(out, "staging.json"), encoding="utf-8") as f:
            record = json.load(f)
        self.assertEqual(record["machine"], "0x8664")
        self.assertEqual(record["exe"], "bin\\x64\\game.exe")
        self.assertEqual(len(record["exe_sha256"]), 64)

    def test_stages_pe32_via_jit(self):
        write_pe(os.path.join(self.game, "bin", "x64", "game.exe"), 0x014C)
        result = self.stage("--exe", "bin/x64/game.exe")
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
        with open(os.path.join(self.tmp, "out", "staging.json"), encoding="utf-8") as f:
            record = json.load(f)
        self.assertEqual(record["machine"], "0x014c")

    def test_refuses_arm64(self):
        write_pe(os.path.join(self.game, "bin", "x64", "game.exe"), 0xAA64)
        result = self.stage("--exe", "bin/x64/game.exe")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ARM64", result.stderr + result.stdout)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "out")))

    def test_force_32bit_stages_with_warning(self):
        # Keep the flag wired for scripts; PE32 is now the default, so force
        # applies to the remaining unsupported arch (ARM64).
        write_pe(os.path.join(self.game, "bin", "x64", "game.exe"), 0xAA64)
        result = self.stage("--exe", "bin/x64/game.exe", "--force-32bit")
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
        self.assertIn("WARNING", result.stdout)
        self.assertTrue(os.path.isdir(os.path.join(self.tmp, "out")))

    def test_rejects_out_inside_source(self):
        result = subprocess.run(
            [sys.executable, os.path.join(TOOLS, "game-stage.py"), self.game,
             "--out", os.path.join(self.game, "staged")],
            capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--out", result.stdout + result.stderr)

    def test_dry_run_copies_nothing(self):
        result = self.stage("--exe", "bin/x64/game.exe", "--dry-run")
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "out")))

    def test_set_writes_per_game_profile(self):
        """`--set` is how a console run gets an experimental host setting: a
        packaged app inherits no environment, so the profile ini is the only
        channel that reaches the host at launch."""
        result = self.stage("--exe", "bin/x64/game.exe",
                            "--set", "fsb_decode=1",
                            "--set", "frame_rate_cap=30")
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
        path = os.path.join(self.tmp, "out", "tiramisu.profile.ini")
        with open(path, encoding="utf-8") as handle:
            body = handle.read()
        self.assertIn("fsb_decode=1", body)
        self.assertIn("frame_rate_cap=30", body)
        with open(os.path.join(self.tmp, "out", "staging.json"),
                  encoding="utf-8") as handle:
            record = json.load(handle)
        self.assertEqual(record["settings"],
                         {"fsb_decode": "1", "frame_rate_cap": "30"})

    def test_set_rejects_a_bare_key(self):
        result = self.stage("--exe", "bin/x64/game.exe", "--set", "fsb_decode")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("key=value", result.stderr + result.stdout)


class FakePortal(devportal.DevicePortal):
    """Records portal traffic instead of talking to a console."""

    def __init__(self):
        super().__init__("fake", log=lambda _m: None)
        self.folders = {("\\LocalState"): {"Tiramisu": {"FileSize": 0}}}
        self.uploads = {}
        self.mkdirs = []

    def listing(self, package_full, remote):
        return self.folders.get(remote)

    def mkdir(self, package_full, parent, name):
        path = parent + "\\" + name
        self.folders.setdefault(parent, {})[name] = {"FileSize": 0}
        self.folders[path] = {}
        self.mkdirs.append(path)

    def put_files(self, package_full, remote, paths):
        for path in paths:
            self.uploads[remote + "\\" + os.path.basename(path)] = \
                os.path.getsize(path)
            self.folders.setdefault(remote, {})[os.path.basename(path)] = {
                "FileSize": os.path.getsize(path)}


class TestPushTree(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="push-")
        os.makedirs(os.path.join(self.tmp, "sub"))
        with open(os.path.join(self.tmp, "a.bin"), "wb") as handle:
            handle.write(b"a" * 10)
        with open(os.path.join(self.tmp, "sub", "b.bin"), "wb") as handle:
            handle.write(b"b" * 20)
        self.portal = FakePortal()

    def test_creates_folders_and_uploads(self):
        sent, size, skipped, _ = self.portal.push_tree(
            "pkg", self.tmp, "\\LocalState\\games\\Game", log=lambda _m: None)
        self.assertEqual((sent, size, skipped), (2, 30, 0))
        self.assertIn("\\LocalState\\games\\Game\\a.bin", self.portal.uploads)
        self.assertIn("\\LocalState\\games\\Game\\sub\\b.bin", self.portal.uploads)

    def test_rerun_skips_same_size(self):
        self.portal.push_tree("pkg", self.tmp, "\\LocalState\\games\\Game",
                              log=lambda _m: None)
        sent, size, skipped, _ = self.portal.push_tree(
            "pkg", self.tmp, "\\LocalState\\games\\Game", log=lambda _m: None)
        self.assertEqual((sent, size, skipped), (0, 0, 2))

    def test_changed_size_is_resent(self):
        self.portal.push_tree("pkg", self.tmp, "\\LocalState\\games\\Game",
                              log=lambda _m: None)
        with open(os.path.join(self.tmp, "a.bin"), "wb") as handle:
            handle.write(b"a" * 11)
        sent, _, skipped, _ = self.portal.push_tree(
            "pkg", self.tmp, "\\LocalState\\games\\Game", log=lambda _m: None)
        self.assertEqual((sent, skipped), (1, 1))


if __name__ == "__main__":
    unittest.main()
