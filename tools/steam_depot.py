#!/usr/bin/env python3
"""
G6 client-less Steam sideload helper for Tiramisu UWP.

Stages an owned Steam app from a PC depot directory (or drives DepotDownloader /
steamctl if installed with the user's own Steam credentials/QR session) into a
clean console-ready folder outside git, checks x64 PE readiness and Steam DRM
exposure, and prints exact Xbox Dev Portal sideload steps.

No DRM bypass, no hardcoded credentials, never copies/commits secrets.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path


def inspect_pe(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    if len(data) < 0x100 or data[:2] != b"MZ":
        return {"pe": False}
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if e_lfanew + 24 > len(data) or data[e_lfanew : e_lfanew + 4] != b"PE\x00\x00":
        return {"pe": False}
    machine, num_sec = struct.unpack_from("<HH", data, e_lfanew + 4)
    opt_off = e_lfanew + 24
    opt_magic = struct.unpack_from("<H", data, opt_off)[0]
    subsys = 0
    if opt_magic in (0x20B, 0x10B):
        subsys = struct.unpack_from("<H", data, opt_off + 68)[0]
    arch = "x64" if machine == 0x8664 else ("x86" if machine == 0x014C else hex(machine))
    has_stub = b".bind" in data[: min(len(data), 0x4000)]
    return {
        "pe": True,
        "arch": arch,
        "sections": num_sec,
        "subsystem": subsys,
        "steamstub_bind_section": has_stub,
    }


def cmd_check(root: Path) -> int:
    if not root.is_dir():
        print(f"error: not a directory: {root}", file=sys.stderr)
        return 2
    exes = sorted(p for p in root.rglob("*.exe") if p.is_file())
    dlls = sorted(p for p in root.rglob("*.dll") if p.is_file())
    steam_dlls = [p for p in dlls if "steam_api" in p.name.lower()]
    report = {
        "root": str(root),
        "exe_count": len(exes),
        "dll_count": len(dlls),
        "executables": [],
        "steam_api_dlls": [str(p.relative_to(root)) for p in steam_dlls],
    }
    ok = True
    for exe in exes:
        info = inspect_pe(exe)
        info["path"] = str(exe.relative_to(root))
        report["executables"].append(info)
        if info.get("arch") != "x64":
            ok = False
    print(json.dumps(report, indent=2))
    if not exes:
        print("warn: no .exe found in directory", file=sys.stderr)
        return 1
    if not ok:
        print("note: x86-32 executables are unsupported on Xbox Series S/X UWP (WOW64 unavailable)", file=sys.stderr)
    return 0 if ok else 1


def cmd_stage(src: Path, dst: Path, app_id: str | None) -> int:
    if not src.is_dir():
        print(f"error: source directory missing: {src}", file=sys.stderr)
        return 2
    dst.mkdir(parents=True, exist_ok=True)
    for item in src.iterdir():
        target = dst / item.name
        if item.is_dir():
            shutil.copytree(item, target, dirs_exist_ok=True)
        else:
            shutil.copy2(item, target)
    if app_id:
        (dst / "steam_appid.txt").write_text(f"{app_id}\n", encoding="utf-8")
    print(f"staged {src} -> {dst}")
    return cmd_check(dst)


def cmd_download(app_id: str, out_dir: Path, username: str | None) -> int:
    dl = shutil.which("DepotDownloader") or shutil.which("depotdownloader")
    sctl = shutil.which("steamctl")
    out_dir.mkdir(parents=True, exist_ok=True)
    if dl:
        cmd = [dl, "-app", app_id, "-os", "windows", "-osarch", "64", "-dir", str(out_dir)]
        if username:
            cmd += ["-username", username]
        else:
            cmd += ["-qr"]
        print("running:", " ".join(cmd))
        return subprocess.call(cmd)
    if sctl:
        cmd = [sctl, "depot", "download", "--app", app_id, "--os", "windows", "-o", str(out_dir)]
        print("running:", " ".join(cmd))
        return subprocess.call(cmd)
    print(
        "DepotDownloader / steamctl not installed on PATH.\n"
        "Install DepotDownloader (x64 Linux) or download on PC via Steam, then run:\n"
        f"  {sys.argv[0]} stage <pc-game-dir> {out_dir} --app-id {app_id}\n"
        "Then copy the staged folder onto USB NTFS (D:\\Games\\...) or deploy via "
        "tools/deploy.sh for LocalFolder Z:\\ testing.",
        file=sys.stderr,
    )
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description="Tiramisu client-less Steam sideload tool (G6)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p_chk = sub.add_parser("check", help="audit an extracted game directory for x64 / SteamStub")
    p_chk.add_argument("dir", type=Path)
    p_stg = sub.add_parser("stage", help="copy an owned build outside git & check x64 readiness")
    p_stg.add_argument("src", type=Path)
    p_stg.add_argument("dst", type=Path)
    p_stg.add_argument("--app-id", default=None)
    p_dl = sub.add_parser("download", help="download an owned Steam depot via DepotDownloader QR/auth")
    p_dl.add_argument("app_id")
    p_dl.add_argument("out_dir", type=Path)
    p_dl.add_argument("--username", default=None)
    args = ap.parse_args()
    if args.cmd == "check":
        return cmd_check(args.dir)
    if args.cmd == "stage":
        return cmd_stage(args.src, args.dst, args.app_id)
    if args.cmd == "download":
        return cmd_download(args.app_id, args.out_dir, args.username)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
