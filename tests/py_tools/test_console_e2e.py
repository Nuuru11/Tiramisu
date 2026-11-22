"""End-to-end test: tools/console.py against a Device Portal over real HTTPS.

Everything here is offline. The fake portal signs its own certificate, enforces
the CSRF cookie/header dance and basic auth, and stores uploads in memory, so a
regression in the client's cookie handling, multipart encoding, folder walk or
proof export fails here instead of on a TV.

    python3 -m unittest discover -s tests/py_tools
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from fake_portal import FakePortal  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
CONSOLE = os.path.join(REPO, "tools", "console.py")


def run(*args, expect=0):
    proc = subprocess.run([sys.executable, CONSOLE, *args],
                          capture_output=True, text=True, timeout=180)
    if expect is not None and proc.returncode != expect:
        raise AssertionError(
            f"console.py {args[0]} exited {proc.returncode}, expected {expect}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
    return proc


class ConsolePipelineTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="console-e2e-")
        self.msix = os.path.join(self.tmp, "Tiramisu.msix")
        self.cert = os.path.join(self.tmp, "dev.cer")
        with open(self.msix, "wb") as handle:
            handle.write(b"PK\x03\x04 fake msix payload" * 64)
        with open(self.cert, "wb") as handle:
            handle.write(b"-----BEGIN CERTIFICATE-----\nfake\n")

        # A staged game, laid out the way game-stage.py leaves it.
        self.game = os.path.join(self.tmp, "Factorio")
        os.makedirs(os.path.join(self.game, "bin", "x64"))
        os.makedirs(os.path.join(self.game, "data"))
        with open(os.path.join(self.game, "bin", "x64", "factorio.exe"),
                  "wb") as handle:
            handle.write(b"MZ fake exe" * 32)
        with open(os.path.join(self.game, "data", "core.dat"), "wb") as handle:
            handle.write(b"x" * 4096)
        with open(os.path.join(self.game, "staging.json"), "w",
                  encoding="utf-8") as handle:
            json.dump({"exe": "bin/x64/factorio.exe",
                       "machine_label": "PE32+ (x64)",
                       "exe_sha256": "deadbeef",
                       "args": "--load-game save.zip"}, handle)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def common(self, portal):
        return ["--console", "127.0.0.1", "--port", str(portal.port),
                "--user", "dev", "--password", "dev", "--app", "Xan.Tiramisu"]

    def test_full_pipeline(self):
        with FakePortal(user="dev", password="dev") as portal:
            base = self.common(portal)

            # 1. identity: proves auth + the CSRF handshake both work.
            info = run("info", *base)
            self.assertIn("FakeXbox", info.stdout)
            self.assertIn("10.0.26100.1234", info.stdout)

            # 2. install: multipart msix + cert, then poll the state endpoint.
            run("install", *base, "--msix", self.msix, "--cert", self.cert)
            self.assertEqual(portal.state.packages[0]["Name"], "Xan.Tiramisu")
            self.assertIn("Tiramisu.msix", portal.state.last_install)

            # 3. upload: folder creation + batched multipart file upload.
            run("upload", *base, "--game", self.game)
            package = portal.state.package_full()
            stored = {name: data for (pkg, folder, name), data
                      in portal.state.files.items()
                      if pkg == package}
            self.assertEqual(sorted(stored),
                             ["core.dat", "factorio.exe", "staging.json"])
            self.assertEqual(stored["core.dat"], b"x" * 4096)
            self.assertIn((package, "\\LocalState\\games\\Factorio"),
                          portal.state.folders)
            self.assertIn((package, "\\LocalState\\games\\Factorio\\bin"),
                          portal.state.folders)

            # 4. rerun: the same files are skipped, not re-sent.
            again = run("upload", *base, "--game", self.game)
            self.assertIn("3 already current", again.stdout)

            # 5. launch: the package full name is base64 in both query params.
            run("launch", *base, "--game", self.game)
            self.assertEqual(portal.state.launches[-1], package)

            # 6. proof: screenshot + log + proof.json + a matrix row.
            portal.state.files[(package, "\\LocalState", "tiramisu.log")] = (
                b"tiramisu: boot\ntiramisu: game fps 59.9\n")
            out = os.path.join(self.tmp, "proof-out")
            result = run("proof", *base, "--game", self.game, "--out", out)
            with open(os.path.join(out, "proof.json"),
                      encoding="utf-8") as handle:
                record = json.load(handle)
            self.assertEqual(record["title"], "Factorio")
            self.assertEqual(record["console"], "FakeXbox")
            self.assertEqual(record["os_version"], "10.0.26100.1234")
            self.assertEqual(record["package"], package)
            self.assertEqual(record["machine"], "PE32+ (x64)")
            self.assertEqual(record["exe_sha256"], "deadbeef")
            self.assertTrue(os.path.isfile(
                os.path.join(out, record["screenshot"])))
            self.assertIn("tiramisu.log", record["logs"][0])
            self.assertIn("docs/proof/titles.json", result.stdout)
            self.assertIn("title-matrix.py", result.stdout)

    def test_missing_csrf_is_rejected(self):
        """The portal refuses a POST without the cookie-derived header. The
        client only ever learns that token from /api/os/info, so a portal that
        does not hand it out must produce a named 403, not a silent install."""
        with FakePortal(user="dev", password="dev") as portal:
            portal.state.send_csrf_cookie = False
            proc = run("install", *self.common(portal), "--msix", self.msix,
                       expect=None)
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("403", proc.stderr)
            self.assertEqual(portal.state.packages, [])

    def test_bad_password_is_named(self):
        with FakePortal(user="dev", password="dev") as portal:
            proc = run("info", "--console", "127.0.0.1", "--port",
                       str(portal.port), "--user", "dev",
                       "--password", "wrong", expect=None)
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("401", proc.stderr)

    def test_install_failure_is_named(self):
        with FakePortal(user="dev", password="dev") as portal:
            portal.state.install_ok = False
            proc = run("install", *self.common(portal), "--msix", self.msix,
                       expect=None)
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("install failed", proc.stderr)

    def test_game_must_exist(self):
        with FakePortal(user="dev", password="dev") as portal:
            proc = run("upload", *self.common(portal), "--game",
                       os.path.join(self.tmp, "nope"), expect=None)
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("no such staged game", proc.stderr)


if __name__ == "__main__":
    unittest.main()
