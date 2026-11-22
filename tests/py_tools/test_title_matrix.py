"""The matrix must refuse to render a row whose evidence is not on disk.

That is the whole point of generating the table, so it gets a test rather than
a comment.
"""

import importlib.util
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
TOOL = os.path.join(REPO, "tools", "title-matrix.py")

DOC = """# Title matrix

## Measured

<!-- title-matrix:measured:begin -->
<!-- title-matrix:measured:end -->

## Queued

hand written
"""


def load_tool():
    spec = importlib.util.spec_from_file_location("title_matrix", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class TitleMatrixTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="title-matrix-")
        self.tool = load_tool()
        self.tool.REPO = self.tmp
        self.tool.MANIFEST = os.path.join(self.tmp, "docs", "proof",
                                          "titles.json")
        self.tool.DOC = os.path.join(self.tmp, "docs", "title-matrix.md")
        os.makedirs(os.path.join(self.tmp, "docs", "proof"))
        with open(self.tool.DOC, "w", encoding="utf-8") as handle:
            handle.write(DOC)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write_manifest(self, rows):
        with open(self.tool.MANIFEST, "w", encoding="utf-8") as handle:
            json.dump({"titles": rows}, handle)

    def test_missing_artifact_is_a_hard_error(self):
        self.write_manifest([{
            "title": "Ghost Title", "machine": "PE32+ x64",
            "evidence": ["docs/proof/ghost.png"]}])
        self.assertEqual(self.tool.main([]), 1)

    def test_row_renders_when_evidence_exists(self):
        shot = os.path.join(self.tmp, "docs", "proof", "game.png")
        open(shot, "wb").close()
        self.write_manifest([{
            "title": "Game", "build": "custom", "machine": "PE32+ x64",
            "boots": "yes", "renders": "yes", "audio": "yes", "input": "pad",
            "playable": "yes", "evidence": ["docs/proof/game.png"]}])
        self.assertEqual(self.tool.main([]), 0)
        with open(self.tool.DOC, encoding="utf-8") as handle:
            document = handle.read()
        self.assertIn("| Game | custom | PE32+ x64 |", document)
        self.assertIn("`docs/proof/game.png`", document)
        self.assertIn("## Queued", document)
        # Second run: nothing to do, and --check agrees.
        self.assertEqual(self.tool.main([]), 0)
        self.assertEqual(self.tool.main(["--check"]), 0)

    def test_proof_json_overrides_machine_columns(self):
        folder = os.path.join(self.tmp, "docs", "proof", "Game")
        os.makedirs(folder)
        open(os.path.join(folder, "screen-1.png"), "wb").close()
        open(os.path.join(folder, "tiramisu.log"), "w").close()
        with open(os.path.join(folder, "proof.json"), "w",
                  encoding="utf-8") as handle:
            json.dump({"title": "Game", "machine": "PE32+ (x64)",
                       "console": "DevBox", "os_version": "10.0.1",
                       "screenshot": "screen-1.png",
                       "logs": ["tiramisu.log"], "exe_sha256": "abc123"}, handle)
        self.write_manifest([{
            "title": "Game", "dir": "Game", "machine": "stale",
            "evidence": []}])
        self.assertEqual(self.tool.main([]), 0)
        with open(self.tool.DOC, encoding="utf-8") as handle:
            document = handle.read()
        self.assertIn("PE32+ (x64)", document)
        self.assertNotIn("| stale |", document)
        self.assertIn("`docs/proof/Game/screen-1.png`", document)
        self.assertIn("console `DevBox`", document)

    def test_unlisted_proof_json_is_reported(self):
        folder = os.path.join(self.tmp, "docs", "proof", "stray")
        os.makedirs(folder)
        with open(os.path.join(folder, "proof.json"), "w",
                  encoding="utf-8") as handle:
            json.dump({"title": "stray"}, handle)
        self.write_manifest([])
        self.assertEqual(self.tool.main([]), 1)


if __name__ == "__main__":
    unittest.main()
