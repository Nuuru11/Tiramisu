#!/usr/bin/env python3
"""Copy app state between two package identities on the same Xbox console.

Changing the package Identity (e.g. the Edge identity used for hardware mouse)
changes the package family, so the app gets a NEW LocalState folder. Games staged
under the old identity disappear from the shelf. This tool walks the old
LocalState through Device Portal, downloads it to a scratch dir, then uploads it
to the new identity. Files already present at the same size are skipped, so a
rerun resumes after an interruption.

Usage:
  tools/migrate-localstate.py --from-pfn OLD_FULL_NAME --to-pfn NEW_FULL_NAME
  tools/migrate-localstate.py ... --only games,profiles --dry-run

Credentials come from XBOX_IP/XBOX_USER/XBOX_PASSWORD/DP_PORT like console.py.
Device Portal on a dev console is often unauthenticated; pass --password none.
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))

from devportal import DevicePortal  # noqa: E402

DEFAULT_ITEMS = ["games", "profiles", "covers", "drive_c", "incoming", "settings.ini"]


def walk_remote(portal: DevicePortal, pfn: str, remote: str):
    """Yield (relative_path, size, is_dir) for everything under remote."""
    for name, item in (portal.listing(pfn, remote) or {}).items():
        is_dir = bool(int(item.get("Type", 0)) & 16)
        rel = name if is_dir else name
        yield rel, int(item.get("FileSize", 0) or 0), is_dir
        if is_dir:
            for sub, size, sub_is_dir in walk_remote(portal, pfn, remote + "\\" + name):
                yield name + "\\" + sub, size, sub_is_dir


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--console", default=os.environ.get("XBOX_IP", "10.0.0.23"))
    ap.add_argument("--user", default=os.environ.get("XBOX_USER", "jewboy420"))
    ap.add_argument("--password", default=os.environ.get("XBOX_PASSWORD", "none"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("XBOX_PORT", "11443")))
    ap.add_argument("--from-pfn", required=True, help="old package FULL name")
    ap.add_argument("--to-pfn", required=True, help="new package FULL name")
    ap.add_argument("--root", default=r"\LocalState")
    ap.add_argument("--only", default=",".join(DEFAULT_ITEMS))
    ap.add_argument("--scratch", default=None)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    items = [i.strip() for i in args.only.split(",") if i.strip()]
    scratch = Path(args.scratch or tempfile.mkdtemp(prefix="tiramisu-migrate-"))
    scratch.mkdir(parents=True, exist_ok=True)
    portal = DevicePortal(args.console, args.user, args.password, port=args.port)
    print(f"console={args.console} {args.from_pfn} -> {args.to_pfn}", flush=True)
    print(f"scratch={scratch}", flush=True)

    total = 0
    count = 0
    try:
        for item in items:
            src = args.root + "\\" + item
            listing = portal.listing(args.from_pfn, src)
            if listing is None:
                # A single file (settings.ini) still needs a per-file fetch.
                parent_listing = portal.listing(args.from_pfn, args.root) or {}
                if item not in parent_listing:
                    print(f"SKIP {item}: not in old LocalState", flush=True)
                    continue
                if args.dry_run:
                    print(f"FILE {item}", flush=True)
                    continue
                data = portal.get_file(args.from_pfn, args.root, item)
                if data is None:
                    print(f"WARN could not download {item}", flush=True)
                    continue
                (scratch / item).write_bytes(data)
                portal.ensure_dir(args.to_pfn, args.root)
                portal.put_files(args.to_pfn, args.root, [str(scratch / item)])
                print(f"FILE {item}: {len(data)} bytes", flush=True)
                continue

            entries = list(walk_remote(portal, args.from_pfn, src))
            files = [(rel, size) for rel, size, is_dir in entries if not is_dir]
            size_sum = sum(size for _, size in files)
            total += size_sum
            count += len(files)
            print(f"{item}: {len(files)} files, {size_sum / 2**20:.1f} MiB", flush=True)
            if args.dry_run:
                continue

            local_root = scratch / item
            for rel, _ in files:
                dest = local_root / rel.replace("\\", os.sep)
                dest.parent.mkdir(parents=True, exist_ok=True)
                remote_dir = src + ("\\" + rel.rsplit("\\", 1)[0] if "\\" in rel else "")
                name = rel.rsplit("\\", 1)[-1]
                data = portal.get_file(args.from_pfn, remote_dir, name)
                if data is None:
                    print(f"  WARN download failed: {remote_dir}\\{name}", flush=True)
                    continue
                dest.write_bytes(data)
            portal.push_tree(args.to_pfn, str(local_root), src)
            print(f"  migrated {item}", flush=True)
    finally:
        if not args.dry_run and os.environ.get("KEEP_SCRATCH") != "1":
            shutil.rmtree(scratch, ignore_errors=True)

    print(f"DONE: {count} files, {total / 2**20:.1f} MiB", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
