#!/usr/bin/env python3
"""Stage a game you own for Tiramisu on an Xbox in Developer Mode.

    python3 tools/game-stage.py "~/Games/Factorio_2.0.77" --out staging/Factorio

What it does, and why each part exists:

  * copies the game tree unchanged into `staging/<Name>/` (no game file is
    modified; nothing is downloaded or bundled);
  * finds the launch exe, and refuses by default when it is PE32 (x86-32):
    Tiramisu runs only PE32+ guest code today, so uploading 1.5 GB of a 32-bit
    title to find that out on the TV is a waste of an evening;
  * writes `tiramisu_args.txt` next to the exe - the host reads it on launch
    and feeds the lines to the guest's command line (this is how a game that
    needs `--load-game save.zip` gets its arguments);
  * writes `staging.json` with the machine type, sizes and a sha256 of the exe,
    so a later proof record can name exactly what ran;
  * `--set key=value` writes a `tiramisu.profile.ini` next to the exe. That is
    the host's per-game settings override, and it is the only way to turn on an
    experimental path (e.g. `--set fsb_decode=1`) on a console, where there is
    no environment to inherit.

Everything is offline: this never talks to a console (see tools/console.py).
"""

import argparse
import hashlib
import json
import os
import shutil
import struct
import sys
import time

MACHINES = {
    0x014C: ("PE32 (x86-32)", False),
    0x8664: ("PE32+ (x64)", True),
    0xAA64: ("PE32+ (ARM64)", True),
    0xA641: ("PE32+ (ARM64EC)", True),
    0x0200: ("PE32+ (IA64)", True),
}

SKIP_DIRS = {".git", "__pycache__", ".svn", "node_modules"}


def read_pe_machine(path):
    """Return (machine, description, supported) for a PE, or None if not a PE."""
    try:
        with open(path, "rb") as handle:
            dos = handle.read(64)
            if len(dos) < 64 or dos[:2] != b"MZ":
                return None
            e_lfanew = struct.unpack_from("<I", dos, 0x3C)[0]
            handle.seek(e_lfanew)
            head = handle.read(6)
    except OSError:
        return None
    if len(head) < 6 or head[:4] != b"PE\0\0":
        return None
    machine = struct.unpack_from("<H", head, 4)[0]
    label, supported = MACHINES.get(machine, (f"0x{machine:04x}", False))
    return machine, label, supported


def find_exes(root, max_depth=4):
    """Candidate launch executables, shallowest first."""
    found = []
    base_depth = root.rstrip(os.sep).count(os.sep)
    for here, dirs, files in os.walk(root):
        depth = here.count(os.sep) - base_depth
        if depth >= max_depth:
            dirs[:] = []
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for name in sorted(files):
            if name.lower().endswith(".exe"):
                path = os.path.join(here, name)
                found.append((depth, os.path.relpath(path, root), path))
    found.sort(key=lambda item: (item[0], item[1]))
    return [(rel, path) for _, rel, path in found]


def copy_tree(src, dst, excludes, log=print):
    total_files = total_bytes = 0
    for here, dirs, files in os.walk(src):
        rel = os.path.relpath(here, src)
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS and
                         not any(e in os.path.join(rel, d) for e in excludes))
        target = os.path.join(dst, rel) if rel != "." else dst
        os.makedirs(target, exist_ok=True)
        for name in sorted(files):
            rel_file = os.path.join(rel, name) if rel != "." else name
            if any(e in rel_file for e in excludes):
                continue
            source = os.path.join(here, name)
            if not os.path.isfile(source):
                continue
            shutil.copyfile(source, os.path.join(target, name))
            total_files += 1
            total_bytes += os.path.getsize(source)
        if total_files and total_files % 500 == 0:
            log(f"  copied {total_files} files, {total_bytes / 2 ** 20:.0f} MB")
    return total_files, total_bytes


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Stage an owned game folder for Tiramisu on Xbox Dev Mode")
    parser.add_argument("source", help="the game folder on this PC")
    parser.add_argument("--out", required=True,
                        help="staging output, e.g. staging/Factorio")
    parser.add_argument("--exe", help="launch exe relative to the game root")
    parser.add_argument("--args", default="",
                        help="guest command line, written to tiramisu_args.txt")
    parser.add_argument("--exclude", action="append", default=[],
                        help="substring of paths to skip (repeatable)")
    parser.add_argument("--force-32bit", action="store_true",
                        help="stage anyway when the exe is PE32 (it will not run)")
    parser.add_argument("--set", action="append", default=[],
                        metavar="KEY=VALUE",
                        help="per-game host setting, e.g. fsb_decode=1 "
                             "(repeatable; written to tiramisu.profile.ini)")
    parser.add_argument("--dry-run", action="store_true",
                        help="report what would happen, copy nothing")
    args = parser.parse_args(argv)

    source = os.path.abspath(os.path.expanduser(args.source))
    if not os.path.isdir(source):
        sys.exit(f"ERROR: not a folder: {source}")
    out = os.path.abspath(os.path.expanduser(args.out))
    if out == source or out.startswith(source + os.sep):
        sys.exit("ERROR: --out must not be inside the source game folder")

    exes = find_exes(source)
    if not exes:
        sys.exit(f"ERROR: no .exe under {source}")
    if args.exe:
        wanted = args.exe.replace("/", os.sep)
        match = next((path for rel, path in exes
                      if rel.replace(os.sep, "/").lower() ==
                      wanted.replace(os.sep, "/").lower()), None)
        if not match:
            sys.exit(f"ERROR: {args.exe} is not under {source}")
        exe_rel, exe_path = args.exe, match
    elif len(exes) == 1:
        exe_rel, exe_path = exes[0][0], exes[0][1]
    else:
        listing = "\n".join(f"  {rel}" for rel, _ in exes[:20])
        sys.exit("ERROR: several executables, pick one with --exe:\n" + listing)

    pe = read_pe_machine(exe_path)
    if pe is None:
        sys.exit(f"ERROR: {exe_rel} is not a PE image (Tiramisu needs a PE)")
    machine, label, supported = pe
    print(f"exe:      {exe_rel}")
    print(f"machine:  {label}  (0x{machine:04x})")
    if not supported:
        message = (f"ERROR: {label} guest code cannot run yet - Tiramisu "
                   f"executes PE32+ images only (see docs/04-support-roadmap.md, "
                   f"wall W1). Nothing is staged.")
        if not args.force_32bit:
            sys.exit(message)
        print("WARNING: " + message.split("ERROR: ", 1)[1])

    if args.dry_run:
        print("dry run: nothing copied")
        return 0

    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out, exist_ok=True)
    copied_files, copied_bytes = copy_tree(source, out, args.exclude)

    exe_out = os.path.join(out, exe_rel)
    if not os.path.isfile(exe_out):
        sys.exit(f"ERROR: {exe_rel} was not copied (check --exclude)")

    args_path = os.path.join(out, "tiramisu_args.txt")
    if args.args.strip():
        with open(args_path, "w", encoding="utf-8") as handle:
            handle.write("# Read by Tiramisu at launch; one argument per line, "
                         "# '#' starts a comment.\n")
            handle.write(args.args.strip() + "\n")
        print(f"args:     wrote tiramisu_args.txt ({args.args.strip()})")
    elif os.path.isfile(args_path):
        os.remove(args_path)

    profile_path = os.path.join(out, "tiramisu.profile.ini")
    if args.set:
        lines = ["# Per-game Tiramisu host settings (read at launch, on top of "
                 "settings.ini).\n"]
        for item in args.set:
            key, sep, value = item.partition("=")
            if not sep or not key.strip():
                sys.exit(f"ERROR: --set wants key=value, got '{item}'")
            lines.append(f"{key.strip()}={value.strip()}\n")
        with open(profile_path, "w", encoding="utf-8") as handle:
            handle.writelines(lines)
        print(f"profile:  wrote tiramisu.profile.ini ({', '.join(args.set)})")
    elif os.path.isfile(profile_path):
        os.remove(profile_path)

    record = {
        "source": source,
        "staged": out,
        "name": os.path.basename(out),
        "exe": exe_rel.replace(os.sep, "\\"),
        "machine": f"0x{machine:04x}",
        "machine_label": label,
        "exe_sha256": sha256_of(exe_out),
        "exe_bytes": os.path.getsize(exe_out),
        "args": args.args.strip(),
        "settings": dict(item.partition("=")[::2] for item in args.set),
        "files": copied_files,
        "bytes": copied_bytes,
        "staged_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }
    with open(os.path.join(out, "staging.json"), "w", encoding="utf-8") as handle:
        json.dump(record, handle, indent=2)
        handle.write("\n")

    print(f"staged:   {copied_files} files, {copied_bytes / 2 ** 20:.0f} MB -> {out}")
    print(f"exe sha256: {record['exe_sha256'][:16]}...")
    print()
    print("next:")
    print(f"  python3 tools/console.py upload --game {os.path.relpath(out)} \\")
    print("      --console <xbox ip> --user <user> --password <password>")
    return 0


if __name__ == "__main__":
    sys.exit(main())
