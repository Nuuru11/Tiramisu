#!/usr/bin/env python3
"""Offline tests for the opt-in Microsoft Edge identity spoofer.

Run:  python3 -m unittest tests.py_tools.test_edge_spoof -v
  or  python3 tests/py_tools/test_edge_spoof.py
"""
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import edge_spoof as es  # noqa: E402

SOURCE = (REPO / "package" / "AppxManifest.xml").read_text(encoding="utf-8")


def unspoofed() -> str:
    """Build an unspoofed fixture; the live manifest is now spoofed by default."""
    xml = re.sub(
        r"<Identity\b[^>]*?/>",
        '<Identity Name="Xan.Tiramisu" Publisher="CN=TiramisuDev" '
        'Version="0.0.0.0" ProcessorArchitecture="x64" />',
        SOURCE,
        count=1,
    )
    return re.sub(
        r"(<PublisherDisplayName\s*>).*?(</PublisherDisplayName\s*>)",
        r"\1TiramisuDev\2",
        xml,
        count=1,
        flags=re.S,
    )


class EdgeSpoofTest(unittest.TestCase):
    def test_pfn_is_the_well_known_edge_family_name(self) -> None:
        self.assertEqual(es.EDGE_PFN, "Microsoft.MicrosoftEdge_8wekyb3d8bbwe")
        self.assertEqual(es.EDGE_PUBLISHER_ID, "8wekyb3d8bbwe")

    def test_patch_rewrites_identity_and_publisher_display(self) -> None:
        source = unspoofed()
        out = es.spoof_manifest(source)
        self.assertIn('Name="Xan.Tiramisu"', source)
        self.assertIn(f'Name="{es.EDGE_IDENTITY_NAME}"', out)
        self.assertIn(f'Publisher="{es.EDGE_PUBLISHER}"', out)
        self.assertIn(
            f"<PublisherDisplayName>{es.EDGE_PUBLISHER_DISPLAY}</PublisherDisplayName>",
            out,
        )
        es.check_manifest(out)

    def test_patch_preserves_version_arch_and_everything_else(self) -> None:
        source = unspoofed()
        out = es.spoof_manifest(source)
        self.assertIn('Version="0.0.0.0"', out)
        self.assertIn('ProcessorArchitecture="x64"', out)
        # Non-identity lines survive byte-for-byte.
        self.assertIn('<Capability Name="codeGeneration" />', out)
        self.assertIn("Executable=\"tiramisu.exe\" EntryPoint=\"tiramisu.App\"", out)
        self.assertEqual(len(out.splitlines()), len(source.splitlines()))

    def test_check_rejects_unspoofed_manifest(self) -> None:
        with self.assertRaises(ValueError):
            es.check_manifest(unspoofed())

    def test_cert_subject_uses_st_and_reversed_rdn_order(self) -> None:
        # OpenSSL drops /S=, so the state must be /ST=. The MSIX tool reads the
        # X.500 name back in reverse, so C must come first to render the
        # Publisher string in the manifest's CN..C order. This is the exact
        # mismatch that made makemsix unpack fail with 0x8bad0043.
        self.assertEqual(
            es.EDGE_CERT_SUBJ,
            "/C=US/ST=Washington/L=Redmond/"
            "O=Microsoft Corporation/CN=Microsoft Corporation",
        )
        self.assertNotIn("/S=", es.EDGE_CERT_SUBJ)

    def test_patch_rejects_manifest_without_identity(self) -> None:
        with self.assertRaises(ValueError):
            es.spoof_manifest("<Package></Package>")

    def test_cli_print_pfn_and_check(self) -> None:
        self.assertEqual(es.main(["--print-pfn"]), 0)
        import io
        import contextlib

        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            es.main(["--print-pfn"])
        self.assertEqual(buf.getvalue().strip(), es.EDGE_PFN)

        import tempfile

        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "source.xml"
            src.write_text(unspoofed(), encoding="utf-8")
            dst = Path(td) / "AppxManifest.xml"
            self.assertEqual(es.main([str(src), str(dst)]), 0)
            es.check_manifest(dst.read_text(encoding="utf-8"))
            self.assertEqual(es.main(["--check", str(dst)]), 0)
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(es.main(["--check", str(src)]), 1)


if __name__ == "__main__":
    unittest.main()
