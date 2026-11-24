"""Offline tests for tools/console_type.py (console-generation detection)."""
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import console_type as ct  # noqa: E402


def _os_info(model):
    return {"Platform": {"Model": model}}


class ConsoleTypeTest(unittest.TestCase):
    def test_detect_generation(self):
        cases = [
            ("Xbox Series X", "series_x"),
            ("xbox series x", "series_x"),
            ("Series X", "series_x"),
            ("Xbox Series S", "series_s"),
            ("xbox series s", "series_s"),
            ("One X", "one_x"),
            ("xbox one x", "one_x"),
            ("One S", "one_s"),
            ("xbox one s", "one_s"),
            ("handheld unixoid box", None),
        ]
        for model, expected in cases:
            family, raw = ct.detect_generation(_os_info(model))
            assert family == expected, (model, family)
            assert raw == model

    def test_fallback_keys(self):
        assert ct.detect_generation({"ConsoleType": "series_s"})[0] == "series_s"
        assert ct.detect_generation({"ProductId": "0.xbn.zip"})[0] in (None, "0.xbn.zip")

    def test_tweaks_cover_all_detectable_generations(self):
        for family in ("series_x", "series_s", "one_x", "one_s"):
            family2, tweak = ct.tweak_for(_os_info(ct.TWEAKS[family]["console"]))
            assert family2 == family
            assert tweak["family"] == family
            assert "settings" in tweak and tweak["settings"].get("memory_budget_mb")
            assert tweak["settings"].get("os_build")

    def test_one_s_is_more_conservative_than_one_x(self):
        _, one_x = ct.tweak_for(_os_info("One X"))
        _, one_s = ct.tweak_for(_os_info("One S"))
        assert one_x["settings"]["memory_budget_mb"] > one_s["settings"]["memory_budget_mb"]

    def test_unknown_is_honest(self):
        family, tweak = ct.tweak_for(_os_info("SerialAsciiControllerNo idea"))
        assert family is None
        assert tweak["family"] == "unknown"
        assert tweak["settings"] == {}
