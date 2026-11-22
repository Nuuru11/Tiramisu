#!/usr/bin/env python3
"""Deploy Tiramisu and a staged game to an Xbox in Developer Mode.

    python3 tools/console.py install  --console 192.168.1.50 --user U --password P
    python3 tools/console.py upload   --game staging/Factorio --console ...
    python3 tools/console.py launch   --console ...
    python3 tools/console.py proof    --game staging/Factorio --console ...

Commands
    info      console identity (auth + CSRF sanity check)
    packages  installed packages
    install   upload out/Tiramisu.msix (+ certificate) and wait for the result
    upload    push a staged game into the app's LocalState\\games\\<Name>
    launch    start the app (it lists the shelf and launches the game)
    stop      force-stop the app
    logs      fetch \\LocalState\\tiramisu.log into logs/
    shot      save a screenshot of the TV into logs/
    proof     shot + logs + proof.json into docs/proof/<name>/, and print the
              title-matrix row that evidence supports
    all       install, upload, launch

The console IP/user/password can also come from XBOX_IP, XBOX_USER,
XBOX_PASSWORD. Never hardcode credentials in a command you will paste into a
chat log.
"""

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from devportal import DevicePortal, DevicePortalError  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP_NAME = "Xan.Tiramisu"          # package/AppxManifest.xml <Identity Name>
DEFAULT_PORT = 11443


def log(message):
    print(message, flush=True)


def build_parser():
    parser = argparse.ArgumentParser(
        description="Xbox Device Portal deployment for Tiramisu",
        epilog="Run `python3 tools/console.py` with no command for the list.")
    parser.add_argument("command", nargs="?", default="",
                        choices=["", "info", "packages", "install", "upload",
                                 "launch", "stop", "logs", "shot", "proof",
                                 "all"])
    parser.add_argument("--console", default=os.environ.get("XBOX_IP"),
                        help="console IP (Dev Home shows it), or XBOX_IP")
    parser.add_argument("--user", default=os.environ.get("XBOX_USER"))
    parser.add_argument("--password", default=os.environ.get("XBOX_PASSWORD"))
    parser.add_argument("--port", type=int,
                        default=int(os.environ.get("XBOX_PORT", DEFAULT_PORT)))
    parser.add_argument("--app", default=APP_NAME,
                        help=f"package family prefix (default {APP_NAME})")
    parser.add_argument("--msix", default=os.path.join(REPO, "out", "Tiramisu.msix"))
    parser.add_argument("--cert", default=os.path.join(REPO, "out", "dev.cer"))
    parser.add_argument("--extra", action="append", default=[],
                        help="extra .appx/.cer to install alongside (repeatable)")
    parser.add_argument("--game", default=None,
                        help="staged game folder, e.g. staging/Factorio")
    parser.add_argument("--remote", default=None,
                        help=r"override the LocalState target, e.g. "
                             r"\LocalState\games\Factorio")
    parser.add_argument("--wait", type=float, default=20.0,
                        help="seconds to let the app render before a screenshot")
    parser.add_argument("--out", default=None,
                        help="proof output folder (default docs/proof/<name>)")
    return parser


def portal(args):
    if not args.console:
        sys.exit("ERROR: pass --console <xbox ip> or set XBOX_IP")
    return DevicePortal(args.console, args.user, args.password, port=args.port,
                        log=log)


def game_paths(args):
    if not args.game:
        sys.exit("ERROR: --game is required (see tools/game-stage.py)")
    local = os.path.abspath(os.path.expanduser(args.game))
    if not os.path.isdir(local):
        sys.exit(f"ERROR: no such staged game: {local}")
    name = os.path.basename(local.rstrip(os.sep))
    remote = args.remote or f"\\LocalState\\games\\{name}"
    return local, name, remote


def staging_record(local):
    path = os.path.join(local, "staging.json")
    if not os.path.isfile(path):
        return {}
    try:
        with open(path, encoding="utf-8") as handle:
            return json.load(handle)
    except ValueError:
        return {}


def require_package(wdp, args):
    package = wdp.find_package(args.app)
    if not package:
        sys.exit(f"ERROR: {args.app} is not installed: run `console.py install`")
    return package


def ensure_local_state(wdp, package):
    """A freshly installed package has no LocalState until it runs once, and
    the folder API cannot create it. So run the app, wait, stop it."""
    if wdp.listing(package["PackageFullName"], "\\LocalState") is not None:
        return
    log("first run, to create the app data folder ...")
    wdp.launch(package)
    time.sleep(20)
    wdp.stop(package)
    time.sleep(3)
    if wdp.listing(package["PackageFullName"], "\\LocalState") is None:
        sys.exit("ERROR: the app did not create LocalState - check the TV")


def cmd_info(wdp, args):
    info = wdp.connect()
    for key in ("ComputerName", "OsVersion", "Platform", "SystemInfo"):
        value = info.get(key)
        if isinstance(value, dict):
            value = ", ".join(f"{k}={v}" for k, v in value.items())
        if value:
            log(f"{key}: {value}")


def cmd_packages(wdp, args):
    packages = wdp.packages()
    log(f"{len(packages)} installed packages")
    for package in packages:
        log(f"  {package.get('PackageFullName')}")


def cmd_install(wdp, args):
    if not os.path.isfile(args.msix):
        sys.exit(f"ERROR: {args.msix} not found: run tools/build-host.sh then "
                 f"tools/pack.sh")
    extras = list(args.extra)
    if os.path.isfile(args.cert):
        extras.append(args.cert)
    existing = wdp.find_package(args.app)
    if existing:
        wdp.stop(existing)
        log(f"stopping {existing.get('PackageFullName')}")
    log(f"installing {os.path.basename(args.msix)} ...")
    wdp.install(args.msix, extras)
    package = wdp.find_package(args.app)
    log(f"installed: {package['PackageFullName'] if package else '(not listed?)'}")


def cmd_upload(wdp, args):
    local, _, remote = game_paths(args)
    package = require_package(wdp, args)
    ensure_local_state(wdp, package)
    total = sum(len(files) for _, _, files in os.walk(local))
    size = sum(os.path.getsize(os.path.join(here, name))
               for here, _, files in os.walk(local) for name in files)
    log(f"uploading {local} -> {remote} ({total} files, {size / 2 ** 20:.0f} MB)")
    sent, sent_bytes, skipped, seconds = wdp.push_tree(
        package["PackageFullName"], local, remote)
    log(f"done: {sent} files ({sent_bytes / 2 ** 20:.0f} MB) in "
        f"{seconds:.0f} s, {skipped} already current")


def cmd_launch(wdp, args):
    package = require_package(wdp, args)
    if args.game:
        _, name, _ = game_paths(args)
        log(f"the shelf will list `{name}` - turn the controller on BEFORE it starts")
    wdp.launch(package)
    log("launched")


def cmd_stop(wdp, args):
    wdp.stop(require_package(wdp, args))


def fetch_logs(wdp, args, target_dir, stamp):
    os.makedirs(target_dir, exist_ok=True)
    package = require_package(wdp, args)
    fetched = []
    remote_dir, name = "\\LocalState", "tiramisu.log"
    data = wdp.get_file(package["PackageFullName"], remote_dir, name)
    path = os.path.join(target_dir, f"{stamp}-tiramisu.log")
    if data is not None:
        with open(path, "wb") as handle:
            handle.write(data)
        fetched.append(path)
        log(f"  {path} ({len(data)} bytes)")
    else:
        log(f"  no {name} yet")
    return fetched


def cmd_logs(wdp, args):
    fetch_logs(wdp, args, os.path.join(REPO, "logs"),
               time.strftime("%Y%m%d-%H%M%S"))


def take_shot(wdp, path):
    data = wdp.screenshot()
    if data is None:
        log("  screenshot failed (portal returned no image)")
        return None
    with open(path, "wb") as handle:
        handle.write(data)
    log(f"  {path} ({len(data)} bytes)")
    return path


def cmd_shot(wdp, args):
    os.makedirs(os.path.join(REPO, "logs"), exist_ok=True)
    if args.game:
        # launch first so the frame on screen is the game, not the shelf
        cmd_launch(wdp, args)
        time.sleep(args.wait)
    take_shot(wdp, os.path.join(REPO, "logs",
                                f"screen-{time.strftime('%Y%m%d-%H%M%S')}.png"))


def cmd_all(wdp, args):
    cmd_install(wdp, args)
    if args.game:
        cmd_upload(wdp, args)
        cmd_launch(wdp, args)
        time.sleep(args.wait)
        os.makedirs(os.path.join(REPO, "logs"), exist_ok=True)
        take_shot(wdp, os.path.join(
            REPO, "logs", f"screen-{time.strftime('%Y%m%d-%H%M%S')}.png"))


def cmd_proof(wdp, args):
    local, name, remote = game_paths(args)
    package = require_package(wdp, args)
    info = wdp.connect()
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out = os.path.abspath(args.out or os.path.join(
        REPO, "docs", "proof", name))
    os.makedirs(out, exist_ok=True)
    log(f"proof for {name} -> {out}")

    shot = os.path.join(out, f"screen-{stamp}.png")
    log("screenshot:")
    take_shot(wdp, shot)
    log("log:")
    logs = fetch_logs(wdp, args, out, stamp)

    record = staging_record(local)
    proof = {
        "title": name,
        "captured_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "console": info.get("ComputerName"),
        "os_version": info.get("OsVersion"),
        "package": package.get("PackageFullName"),
        "package_version": package.get("Version"),
        "exe": record.get("exe"),
        "machine": record.get("machine_label"),
        "exe_sha256": record.get("exe_sha256"),
        "args": record.get("args", ""),
        "remote": remote,
        "screenshot": os.path.basename(shot) if os.path.isfile(shot) else None,
        "logs": [os.path.basename(p) for p in logs],
    }
    with open(os.path.join(out, "proof.json"), "w", encoding="utf-8") as handle:
        json.dump(proof, handle, indent=2)
        handle.write("\n")

    # The matrix row itself is generated from this record, never typed by
    # hand. Only the columns a machine can answer are filled; the rest are
    # written down by whoever watched the TV.
    log("")
    log(f"wrote {os.path.join(out, 'proof.json')}")
    log("next: keep an entry for this title in docs/proof/titles.json "
        f"(\"dir\": \"{name}\"), then run `python3 tools/title-matrix.py`")


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    if not args.command:
        parser.print_help()
        return 0
    wdp = portal(args)
    handler = {
        "info": cmd_info,
        "packages": cmd_packages,
        "install": cmd_install,
        "upload": cmd_upload,
        "launch": cmd_launch,
        "stop": cmd_stop,
        "logs": cmd_logs,
        "shot": cmd_shot,
        "proof": cmd_proof,
        "all": cmd_all,
    }[args.command]
    try:
        if args.command != "info":
            wdp.connect()
        handler(wdp, args)
    except DevicePortalError as exc:
        # A portal that refuses (wrong password, CSRF, install failure) is a
        # named error, not a traceback.
        sys.exit(f"ERROR: {exc}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
