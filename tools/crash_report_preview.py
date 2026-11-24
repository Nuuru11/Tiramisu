#!/usr/bin/env python3
"""Render a sample Tiramisu crash-report entry for review (plan step 3).

This prints what a user would send with `crash_report=1` before any network
code exists. The real payload is written by `crash_report_write` in
`src/host/crash_report.cpp`; this preview reuses its allowlisted shape so the
two cannot drift in spirit.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys_path = REPO / "tools"
import sys  # noqa: E402
sys.path.insert(0, str(sys_path))

# Keep this in sync with crash_report_input.h.
ALLOWLIST_KEYS = (
    "app_version", "build_date", "os_build", "arch",
    "crash_signature", "active_subsystems", "shader_census",
)


def render(
    app_version="0.0.0",
    build_date="unknown",
    os_build="19045",
    arch="x64",
    reason="host exception",
    detail="(no registers/stack captured in preview)",
    settings_ini="[empty]",
    census_json="[]",
    log_tail="(no log tail)",
) -> str:
    """Return the human-readable crash entry the real path writes."""
    allow = ", ".join(ALLOWLIST_KEYS)
    parts = [
        "=== crash_report.txt (dry run preview) ===",
        f"app_version={app_version}",
        f"build_date={build_date}",
        f"os_build={os_build}",
        f"arch={arch}",
        f"reason={reason}",
        f"detail={detail}",
        f"settings_ini:\n{settings_ini}",
        f"active_subsystems=[]",
        f"shader_census={census_json}",
        f"log_tail:\n{log_tail}",
        f"allowlist_keys={allow}",
    ]
    return "\n".join(parts) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--app-version", default="0.0.0")
    ap.add_argument("--build-date", default=os.environ.get("TIRAMISU_BUILD_DATE", "unknown"))
    ap.add_argument("--os-build", default="19045")
    ap.add_argument("--arch", default="x64")
    ap.add_argument("--reason", default="host exception")
    ap.add_argument("--game", default="", help="game dir to scan (optional)")
    args = ap.parse_args(argv)

    census = "[]"
    if args.game:
        import subprocess
        path = args.game
        if os.path.isfile(os.path.join(path, "game.exe")):
            census = subprocess.run(
                [sys.executable, str(REPO / "tools" / "auto-compat.py"), path],
                capture_output=True, text=True).stdout
        else:
            census = f"[scan skipped: {path}]"

    text = render(
        app_version=args.app_version,
        build_date=args.build_date,
        os_build=args.os_build,
        arch=args.arch,
        reason=args.reason,
        census_json=census,
    )
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
