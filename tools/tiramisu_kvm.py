#!/usr/bin/env python3
"""
Tiramisu PC-to-Xbox Live KVM Forwarder (evdev -> LAN + Xbox Guide WebSocket).

Toggles on F8 from any physical keyboard:
  - PAUSED (PC mode): Your PC works normally; evdev passively watches for F8.
  - ACTIVE (Xbox mode): Exclusively grabs (EVIOCGRAB) all physical PC keyboards
    and mice so zero input hits Wayland/X11/your PC:
      * Streams raw relative mouse deltas, clicks, wheel, and keyboard/text
        at ~120 Hz to Tiramisu (http://<host>:9127/api/controls/input).
      * Pressing Win (Left/Right Super) sends VK_LWIN directly to Xbox Device
        Portal (wss://<host>:11443/ext/remoteinput) to open/close the Xbox
        Guide / Dashboard (hold ~1s for Power options) and toggles Guide/OS
        keyboard navigation mode (use Shift+Win to switch local routing only).
Press F8 again (or Ctrl+C / stop service) at any time to ungrab and return to PC.
"""

from __future__ import annotations

import argparse
import atexit
import base64
import collections
import http.client
import json
import os
import select
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import threading
import time
from typing import Dict, List, Optional, Tuple

import evdev
from evdev import ecodes as E


DEFAULT_HOST = "10.0.0.23"
DEFAULT_PORT = 9127
DEFAULT_DP_PORT = 11443
DEFAULT_DP_USER = "dev"
DEFAULT_DP_PASS = "dev"
CONFIG_PATH = os.path.expanduser("~/.config/tiramisu-kvm.json")

# Linux evdev KEY_* -> Win32 Virtual-Key mapping.
EV_KEY_TO_VK: Dict[int, int] = {
    E.KEY_BACKSPACE: 0x08,
    E.KEY_TAB: 0x09,
    E.KEY_ENTER: 0x0D,
    E.KEY_KPENTER: 0x0D,
    E.KEY_PAUSE: 0x13,
    E.KEY_CAPSLOCK: 0x14,
    E.KEY_ESC: 0x1B,
    E.KEY_SPACE: 0x20,
    E.KEY_PAGEUP: 0x21,
    E.KEY_PAGEDOWN: 0x22,
    E.KEY_END: 0x23,
    E.KEY_HOME: 0x24,
    E.KEY_LEFT: 0x25,
    E.KEY_UP: 0x26,
    E.KEY_RIGHT: 0x27,
    E.KEY_DOWN: 0x28,
    E.KEY_SYSRQ: 0x2C,
    E.KEY_INSERT: 0x2D,
    E.KEY_DELETE: 0x2E,
    E.KEY_0: ord("0"),
    E.KEY_1: ord("1"),
    E.KEY_2: ord("2"),
    E.KEY_3: ord("3"),
    E.KEY_4: ord("4"),
    E.KEY_5: ord("5"),
    E.KEY_6: ord("6"),
    E.KEY_7: ord("7"),
    E.KEY_8: ord("8"),
    E.KEY_9: ord("9"),
    E.KEY_A: ord("A"),
    E.KEY_B: ord("B"),
    E.KEY_C: ord("C"),
    E.KEY_D: ord("D"),
    E.KEY_E: ord("E"),
    E.KEY_F: ord("F"),
    E.KEY_G: ord("G"),
    E.KEY_H: ord("H"),
    E.KEY_I: ord("I"),
    E.KEY_J: ord("J"),
    E.KEY_K: ord("K"),
    E.KEY_L: ord("L"),
    E.KEY_M: ord("M"),
    E.KEY_N: ord("N"),
    E.KEY_O: ord("O"),
    E.KEY_P: ord("P"),
    E.KEY_Q: ord("Q"),
    E.KEY_R: ord("R"),
    E.KEY_S: ord("S"),
    E.KEY_T: ord("T"),
    E.KEY_U: ord("U"),
    E.KEY_V: ord("V"),
    E.KEY_W: ord("W"),
    E.KEY_X: ord("X"),
    E.KEY_Y: ord("Y"),
    E.KEY_Z: ord("Z"),
    E.KEY_LEFTMETA: 0x5B,
    E.KEY_RIGHTMETA: 0x5C,
    E.KEY_COMPOSE: 0x5D,
    E.KEY_KP0: 0x60,
    E.KEY_KP1: 0x61,
    E.KEY_KP2: 0x62,
    E.KEY_KP3: 0x63,
    E.KEY_KP4: 0x64,
    E.KEY_KP5: 0x65,
    E.KEY_KP6: 0x66,
    E.KEY_KP7: 0x67,
    E.KEY_KP8: 0x68,
    E.KEY_KP9: 0x69,
    E.KEY_KPASTERISK: 0x6A,
    E.KEY_KPPLUS: 0x6B,
    E.KEY_KPMINUS: 0x6D,
    E.KEY_KPDOT: 0x6E,
    E.KEY_KPSLASH: 0x6F,
    E.KEY_F1: 0x70,
    E.KEY_F2: 0x71,
    E.KEY_F3: 0x72,
    E.KEY_F4: 0x73,
    E.KEY_F5: 0x74,
    E.KEY_F6: 0x75,
    E.KEY_F7: 0x76,
    E.KEY_F8: 0x77,
    E.KEY_F9: 0x78,
    E.KEY_F10: 0x79,
    E.KEY_F11: 0x7A,
    E.KEY_F12: 0x7B,
    E.KEY_NUMLOCK: 0x90,
    E.KEY_SCROLLLOCK: 0x91,
    E.KEY_LEFTSHIFT: 0xA0,
    E.KEY_RIGHTSHIFT: 0xA1,
    E.KEY_LEFTCTRL: 0xA2,
    E.KEY_RIGHTCTRL: 0xA3,
    E.KEY_LEFTALT: 0xA4,
    E.KEY_RIGHTALT: 0xA5,
    E.KEY_SEMICOLON: 0xBA,
    E.KEY_EQUAL: 0xBB,
    E.KEY_COMMA: 0xBC,
    E.KEY_MINUS: 0xBD,
    E.KEY_DOT: 0xBE,
    E.KEY_SLASH: 0xBF,
    E.KEY_GRAVE: 0xC0,
    E.KEY_LEFTBRACE: 0xDB,
    E.KEY_BACKSLASH: 0xDC,
    E.KEY_RIGHTBRACE: 0xDD,
    E.KEY_APOSTROPHE: 0xDE,
}

# Extra shortcuts when navigating the Xbox Guide / Home Dashboard via /ext/remoteinput.
GUIDE_EXTRA_VK: Dict[int, int] = {
    E.KEY_F1: 0xD0,       # VK_GAMEPAD_VIEW
    E.KEY_F2: 0xCF,       # VK_GAMEPAD_MENU
    E.KEY_COMPOSE: 0xCF,  # VK_GAMEPAD_MENU
    E.KEY_PAGEUP: 0xC8,   # VK_GAMEPAD_LEFT_SHOULDER (LB tab prev)
    E.KEY_PAGEDOWN: 0xC7, # VK_GAMEPAD_RIGHT_SHOULDER (RB tab next)
    E.KEY_INSERT: 0xC5,   # VK_GAMEPAD_X
    E.KEY_DELETE: 0xC6,   # VK_GAMEPAD_Y
}

# US-ANSI shifted/unshifted character map for `t <codepoint> 0` text injection.
KEY_CHAR_MAP: Dict[int, Tuple[str, str]] = {
    E.KEY_SPACE: (" ", " "),
    E.KEY_1: ("1", "!"),
    E.KEY_2: ("2", "@"),
    E.KEY_3: ("3", "#"),
    E.KEY_4: ("4", "$"),
    E.KEY_5: ("5", "%"),
    E.KEY_6: ("6", "^"),
    E.KEY_7: ("7", "&"),
    E.KEY_8: ("8", "*"),
    E.KEY_9: ("9", "("),
    E.KEY_0: ("0", ")"),
    E.KEY_MINUS: ("-", "_"),
    E.KEY_EQUAL: ("=", "+"),
    E.KEY_LEFTBRACE: ("[", "{"),
    E.KEY_RIGHTBRACE: ("]", "}"),
    E.KEY_BACKSLASH: ("\\", "|"),
    E.KEY_SEMICOLON: (";", ":"),
    E.KEY_APOSTROPHE: ("'", '"'),
    E.KEY_GRAVE: ("`", "~"),
    E.KEY_COMMA: (",", "<"),
    E.KEY_DOT: (".", ">"),
    E.KEY_SLASH: ("/", "?"),
}

for _offset, _ch in enumerate("abcdefghijklmnopqrstuvwxyz"):
    KEY_CHAR_MAP[E.KEY_A + _offset] = (_ch, _ch.upper())

MOUSE_BTN_MAP: Dict[int, int] = {
    E.BTN_LEFT: 0,
    E.BTN_RIGHT: 1,
    E.BTN_MIDDLE: 2,
    E.BTN_SIDE: 3,
    E.BTN_EXTRA: 4,
}


def ev_key_to_vk_and_char(
    code: int, shift: bool, caps: bool, ctrl: bool, alt: bool
) -> Tuple[Optional[int], Optional[int]]:
    """Map Linux evdev keycode + modifier state to (Win32 VK, Unicode codepoint)."""
    vk = EV_KEY_TO_VK.get(code)
    char_cp: Optional[int] = None
    if not ctrl and not alt and code in KEY_CHAR_MAP:
        unshifted, shifted = KEY_CHAR_MAP[code]
        is_alpha = E.KEY_A <= code <= E.KEY_Z
        use_shift = (shift ^ caps) if is_alpha else shift
        char_cp = ord(shifted if use_shift else unshifted)
    return vk, char_cp


def load_cache() -> dict:
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def save_cache(data: dict) -> None:
    try:
        os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
        with open(CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
    except Exception:
        pass


def notify_desktop(title: str, body: str) -> None:
    """Best-effort desktop notification so Wayland users see mode transitions."""
    exe = shutil.which("notify-send")
    if not exe:
        return
    try:
        subprocess.Popen(
            [exe, "-a", "Tiramisu KVM", "-t", "1600", "-u", "low", title, body],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except Exception:
        pass


class XboxPortalClient:
    """Persistent WebSocket client to Xbox Device Portal /ext/remoteinput (Xbox Guide & Dashboard)."""

    def __init__(self, host: str, port: int, user: str, password: str, enabled: bool = True, dry_run: bool = False) -> None:
        self.host = host
        self.port = port
        self.auth = base64.b64encode(f"{user}:{password}".encode("utf-8")).decode("ascii")
        self.enabled = enabled
        self.dry_run = dry_run
        self._lock = threading.Lock()
        self._tls: Optional[ssl.SSLSocket] = None
        self._ctx = ssl._create_unverified_context()

    def _connect(self) -> ssl.SSLSocket:
        sock = socket.create_connection((self.host, self.port), timeout=2.5)
        tls = self._ctx.wrap_socket(sock, server_hostname=self.host)
        ws_key = base64.b64encode(os.urandom(16)).decode("ascii")
        req = (
            "GET /ext/remoteinput HTTP/1.1\r\n"
            f"Host: {self.host}:{self.port}\r\n"
            f"Authorization: Basic {self.auth}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {ws_key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        tls.sendall(req.encode("ascii"))
        resp = tls.recv(4096).decode("utf-8", errors="replace")
        if "101 Switching Protocols" not in resp:
            tls.close()
            raise RuntimeError(f"/ext/remoteinput handshake rejected: {resp.splitlines()[0] if resp else 'EOF'}")
        return tls

    def _send_frame(self, payload: bytes) -> None:
        if not self.enabled or self.dry_run:
            return
        with self._lock:
            for attempt in range(2):
                try:
                    if self._tls is None:
                        self._tls = self._connect()
                    mask = os.urandom(4)
                    hdr = bytearray([0x82, 0x80 | len(payload)])
                    hdr.extend(mask)
                    masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
                    self._tls.sendall(bytes(hdr) + masked)
                    return
                except Exception as exc:
                    if self._tls is not None:
                        try:
                            self._tls.close()
                        except Exception:
                            pass
                        self._tls = None
                    if attempt == 1:
                        print(f"[tiramisu-kvm] Portal remoteinput error: {exc}", file=sys.stderr)

    def send_key(self, vk: int, down: bool) -> None:
        self._send_frame(bytes([0x01, vk & 0xFF, 0x01 if down else 0x00]))

    def clear_all(self) -> None:
        self._send_frame(bytes([0x04]))

    def close(self) -> None:
        with self._lock:
            if self._tls is not None:
                try:
                    self._tls.close()
                except Exception:
                    pass
                self._tls = None


class TiramisuSender:
    """Thread-safe ~120 Hz coalescing HTTP sender to Tiramisu /api/controls/input."""

    def __init__(self, host: str, port: int, token: str, dry_run: bool = False, on_auth_expired=None) -> None:
        self.host = host
        self.port = port
        self.token = token
        self.dry_run = dry_run
        self.on_auth_expired = on_auth_expired
        self._lock = threading.Lock()
        self._dx = 0
        self._dy = 0
        self._events: collections.deque[str] = collections.deque()
        self._active = False
        self._stop = False
        self._auth_notified = False
        self._last_tx = 0.0
        self._wakeup = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def update_token(self, token: str) -> None:
        with self._lock:
            self.token = token
            self._auth_notified = False
        self._wakeup.set()

    def set_active(self, active: bool) -> None:
        with self._lock:
            self._active = active
            if active:
                self._last_tx = time.monotonic()
        self._wakeup.set()

    def add_motion(self, dx: int, dy: int) -> None:
        if not dx and not dy:
            return
        with self._lock:
            self._dx += dx
            self._dy += dy
        self._wakeup.set()

    def add_line(self, line: str) -> None:
        with self._lock:
            self._events.append(line)
        self._wakeup.set()

    def flush_reset(self) -> None:
        with self._lock:
            self._dx = 0
            self._dy = 0
            self._events.clear()
            self._events.append("r 0 0")
            self._active = False
        self._wakeup.set()

    def stop(self) -> None:
        self.flush_reset()
        time.sleep(0.05)
        self._stop = True
        self._wakeup.set()
        self._thread.join(timeout=1.0)

    def _post(self, path: str, body: str) -> int:
        if self.dry_run or not self.token:
            return 200
        conn = http.client.HTTPConnection(self.host, self.port, timeout=1.5)
        try:
            conn.request(
                "POST",
                path,
                body=body.encode("utf-8"),
                headers={
                    "Authorization": f"Bearer {self.token}",
                    "Content-Type": "text/plain; charset=utf-8",
                },
            )
            resp = conn.getresponse()
            resp.read()
            return resp.status
        finally:
            conn.close()

    def _run(self) -> None:
        while not self._stop:
            self._wakeup.wait(timeout=0.008)
            self._wakeup.clear()

            lines: List[str] = []
            now = time.monotonic()
            with self._lock:
                dx = self._dx
                dy = self._dy
                self._dx = 0
                self._dy = 0
                while dx or dy:
                    cx = max(-2048, min(2048, dx))
                    cy = max(-2048, min(2048, dy))
                    lines.append(f"m {cx} {cy}")
                    dx -= cx
                    dy -= cy
                while self._events and len(lines) < 120:
                    lines.append(self._events.popleft())
                if not lines and self._active and (now - self._last_tx) >= 0.45:
                    lines.append("m 0 0")

            if not lines:
                continue

            payload = "\n".join(lines)
            try:
                status = self._post("/api/controls/input", payload)
                self._last_tx = time.monotonic()
                if status == 401 and not self._auth_notified:
                    self._auth_notified = True
                    print("\n[tiramisu-kvm] Session token expired (Xbox restarted?).", file=sys.stderr)
                    if self.on_auth_expired:
                        threading.Thread(target=self.on_auth_expired, daemon=True).start()
            except Exception:
                pass


def pair_if_needed(host: str, port: int, code: Optional[str], dry_run: bool) -> str:
    if dry_run:
        return "dry-run-token"

    cache = load_cache()
    key = f"{host}:{port}"
    cached_token = cache.get(key)
    if cached_token and not code:
        try:
            conn = http.client.HTTPConnection(host, port, timeout=1.5)
            conn.request(
                "POST",
                "/api/controls/input",
                body=b"m 0 0",
                headers={"Authorization": f"Bearer {cached_token}"},
            )
            r = conn.getresponse()
            r.read()
            conn.close()
            if r.status == 200:
                print(f"[tiramisu-kvm] Reused active session with {host}:{port}")
                return cached_token
        except Exception:
            pass

    if not code:
        print(f"Enter the 8-digit '/controls' code shown on the Xbox TV ({host}:{port}): ", end="", flush=True)
        code = sys.stdin.readline().strip()

    conn = http.client.HTTPConnection(host, port, timeout=3.0)
    try:
        conn.request("POST", "/api/controls/pair", body=code.encode("utf-8"))
        resp = conn.getresponse()
        body = resp.read().decode("utf-8", errors="replace").strip()
        if resp.status != 200 or not body:
            raise RuntimeError(f"Pairing failed: HTTP {resp.status} ({body})")
        cache[key] = body
        save_cache(cache)
        print(f"[tiramisu-kvm] Paired with {host}:{port}!")
        return body
    finally:
        conn.close()


def discover_input_devices() -> Dict[str, Tuple[evdev.InputDevice, bool, bool]]:
    """Return {path: (InputDevice, is_kbd, is_mouse)} for real physical keyboards and mice."""
    found: Dict[str, Tuple[evdev.InputDevice, bool, bool]] = {}
    for path in sorted(evdev.list_devices()):
        try:
            dev = evdev.InputDevice(path)
            caps = dev.capabilities()
            keys = set(caps.get(E.EV_KEY, []))
            rels = set(caps.get(E.EV_REL, []))
            is_kbd = E.KEY_A in keys and E.KEY_F8 in keys
            is_mouse = E.REL_X in rels and E.BTN_LEFT in keys
            if is_kbd or is_mouse:
                found[path] = (dev, is_kbd, is_mouse)
            else:
                dev.close()
        except Exception:
            pass
    return found


class EvdevKvmBridge:
    """Global kernel-level evdev grab/forward bridge (works on Wayland & X11)."""

    def __init__(
        self,
        sender: TiramisuSender,
        portal: XboxPortalClient,
        sens: float,
        start_active: bool,
    ) -> None:
        self.sender = sender
        self.portal = portal
        self.sens = sens
        self.active = False
        self.guide_mode = False
        self.stop_requested = False
        self.devices: Dict[str, Tuple[evdev.InputDevice, bool, bool]] = {}
        self.grabbed: set[str] = set()
        self.held_mods: set[int] = set()
        self.caps_lock = False
        self.f8_pending = False
        self._last_f8_toggle = 0.0
        self._repair_lock = threading.Lock()
        self._repairing = False
        self.rem_x = 0.0
        self.rem_y = 0.0
        self._last_scan = 0.0
        self._refresh_devices(force=True)
        atexit.register(self.cleanup)
        if start_active:
            self.enter_xbox_mode()

    def _refresh_devices(self, force: bool = False) -> None:
        now = time.monotonic()
        if not force and (now - self._last_scan) < 2.0:
            return
        self._last_scan = now
        live = discover_input_devices()

        for path in list(self.devices.keys()):
            if path not in live:
                dev, _, _ = self.devices.pop(path)
                self.grabbed.discard(path)
                try:
                    dev.close()
                except Exception:
                    pass

        for path, (dev, is_kbd, is_mouse) in live.items():
            if path in self.devices:
                dev.close()
                continue
            self.devices[path] = (dev, is_kbd, is_mouse)
            print(f"[tiramisu-kvm] Watching {path} ({dev.name}, kbd={is_kbd}, mouse={is_mouse})")
            if is_kbd:
                try:
                    if E.LED_CAPSL in dev.leds():
                        self.caps_lock = True
                except Exception:
                    pass
            if self.active:
                self._grab_one(path, dev)

    def _grab_one(self, path: str, dev: evdev.InputDevice) -> None:
        if path in self.grabbed:
            return
        try:
            dev.grab()
            self.grabbed.add(path)
        except Exception as exc:
            print(f"[tiramisu-kvm] Warning: could not grab {path} ({dev.name}): {exc}", file=sys.stderr)

    def _ungrab_all(self) -> None:
        for path in list(self.grabbed):
            entry = self.devices.get(path)
            if entry:
                try:
                    entry[0].ungrab()
                except Exception:
                    pass
        self.grabbed.clear()

    def enter_xbox_mode(self) -> None:
        if self.active:
            return
        self._refresh_devices(force=True)
        self.held_mods.clear()
        self.rem_x = 0.0
        self.rem_y = 0.0
        for path, (dev, _, _) in self.devices.items():
            self._grab_one(path, dev)
        self.active = True
        self.sender.set_active(not self.guide_mode)
        notify_desktop("Xbox Tiramisu LOCKED", "Controlling Xbox — Win: Guide, F8: Return to PC")
        print("[tiramisu-kvm] ACTIVE -> controlling Xbox (PC input locked). Win=Guide, F8=Return to PC.")

    def leave_xbox_mode(self) -> None:
        if not self.active:
            return
        self.active = False
        self.guide_mode = False
        self._ungrab_all()
        self.held_mods.clear()
        self.sender.flush_reset()
        self.portal.clear_all()
        notify_desktop("PC Control Restored", "Press F8 to control Xbox Tiramisu")
        print("[tiramisu-kvm] PAUSED -> controlling PC. Press F8 to control Xbox.")

    def handle_auth_expired(self) -> None:
        """Called on HTTP 401: ungrab PC immediately and prompt via Zenity to re-pair live."""
        self.leave_xbox_mode()
        with self._repair_lock:
            if self._repairing:
                return
            self._repairing = True
        try:
            zen = shutil.which("zenity")
            if not zen:
                notify_desktop("Tiramisu Re-Pair Needed", "Run: tiramisu-kvm --code <TV_CODE>")
                return
            res = subprocess.run(
                [
                    zen,
                    "--entry",
                    "--title=Tiramisu KVM Re-Pair",
                    f"--text=Xbox Tiramisu restarted ({self.sender.host}:{self.sender.port}).\nEnter the 8-digit /controls code shown on the TV:",
                ],
                capture_output=True,
                text=True,
                timeout=120,
            )
            code = res.stdout.strip()
            if res.returncode == 0 and code:
                tok = pair_if_needed(self.sender.host, self.sender.port, code, self.sender.dry_run)
                self.sender.update_token(tok)
                notify_desktop("Tiramisu Re-Paired", "Press F8 to control Xbox")
        except Exception as exc:
            print(f"[tiramisu-kvm] Re-pair failed: {exc}", file=sys.stderr)
            notify_desktop("Tiramisu Re-Pair Failed", str(exc))
        finally:
            with self._repair_lock:
                self._repairing = False

    def cleanup(self) -> None:
        try:
            if self.active:
                self.leave_xbox_mode()
            else:
                self._ungrab_all()
            self.portal.close()
            for dev, _, _ in self.devices.values():
                try:
                    dev.close()
                except Exception:
                    pass
            self.devices.clear()
        except Exception:
            pass

    def _process_event(self, ev: evdev.InputEvent) -> None:
        if ev.type == E.EV_KEY:
            if ev.code == E.KEY_F8:
                if ev.value == 1:
                    self.f8_pending = True
                elif ev.value == 0 and self.f8_pending:
                    self.f8_pending = False
                    now = time.monotonic()
                    if (now - self._last_f8_toggle) >= 0.12:
                        self._last_f8_toggle = now
                        if self.active:
                            self.leave_xbox_mode()
                        else:
                            self.enter_xbox_mode()
                return

            if not self.active:
                return

            if ev.code in (E.KEY_LEFTSHIFT, E.KEY_RIGHTSHIFT, E.KEY_LEFTCTRL, E.KEY_RIGHTCTRL, E.KEY_LEFTALT, E.KEY_RIGHTALT):
                if ev.value != 0:
                    self.held_mods.add(ev.code)
                else:
                    self.held_mods.discard(ev.code)

            shift = (E.KEY_LEFTSHIFT in self.held_mods) or (E.KEY_RIGHTSHIFT in self.held_mods)
            ctrl = (E.KEY_LEFTCTRL in self.held_mods) or (E.KEY_RIGHTCTRL in self.held_mods)
            alt = (E.KEY_LEFTALT in self.held_mods) or (E.KEY_RIGHTALT in self.held_mods)

            # Windows key (Super): press/hold triggers real Xbox Guide / Power menu via Device Portal.
            if ev.code in (E.KEY_LEFTMETA, E.KEY_RIGHTMETA):
                if ev.value in (0, 1):
                    if shift and ev.value == 1:
                        # Shift+Win switches local keyboard routing between Guide/Dashboard and Tiramisu without sending Win.
                        self.guide_mode = not self.guide_mode
                        self.sender.flush_reset()
                        self.portal.clear_all()
                        self.sender.set_active(not self.guide_mode)
                        print(f"[tiramisu-kvm] Input route -> {'Xbox Guide/Dashboard' if self.guide_mode else 'Tiramisu'}")
                        return
                    if not self.portal.enabled:
                        return
                    if ev.value == 1:
                        self.guide_mode = not self.guide_mode
                        self.sender.flush_reset()
                        self.sender.set_active(not self.guide_mode)
                        print(f"[tiramisu-kvm] Win -> Xbox Guide ({'OPEN' if self.guide_mode else 'CLOSED'})")
                    self.portal.send_key(0x5B, ev.value == 1)  # VK_LWIN
                return

            if ev.code in MOUSE_BTN_MAP:
                if ev.value in (0, 1) and not self.guide_mode:
                    self.sender.add_line(f"b {MOUSE_BTN_MAP[ev.code]} {ev.value}")
                return

            down = ev.value != 0
            if ev.code == E.KEY_CAPSLOCK and ev.value == 1:
                self.caps_lock = not self.caps_lock

            if self.guide_mode:
                if ev.value in (0, 1):
                    g_vk = GUIDE_EXTRA_VK.get(ev.code) or EV_KEY_TO_VK.get(ev.code)
                    if g_vk is not None:
                        self.portal.send_key(g_vk, down)
                return

            vk, char_cp = ev_key_to_vk_and_char(ev.code, shift, self.caps_lock, ctrl, alt)
            if vk is None:
                return

            self.sender.add_line(f"k {vk} {1 if down else 0}")
            if down and char_cp is not None and 32 <= char_cp <= 65535:
                self.sender.add_line(f"t {char_cp} 0")

        elif ev.type == E.EV_REL and self.active and not self.guide_mode:
            if ev.code == E.REL_X:
                self.rem_x += ev.value * self.sens
                sx = int(self.rem_x)
                self.rem_x -= sx
                if sx:
                    self.sender.add_motion(sx, 0)
            elif ev.code == E.REL_Y:
                self.rem_y += ev.value * self.sens
                sy = int(self.rem_y)
                self.rem_y -= sy
                if sy:
                    self.sender.add_motion(0, sy)
            elif ev.code == E.REL_WHEEL and ev.value != 0:
                self.sender.add_line(f"w {ev.value * 120} 0")

    def run(self) -> None:
        while not self.stop_requested:
            self._refresh_devices(force=False)
            fds = {dev.fd: (path, dev) for path, (dev, _, _) in self.devices.items()}
            if not fds:
                time.sleep(0.25)
                continue
            rlist, _, _ = select.select(list(fds.keys()), [], [], 0.2)
            for fd in rlist:
                entry = fds.get(fd)
                if not entry:
                    continue
                path, dev = entry
                try:
                    for ev in dev.read():
                        self._process_event(ev)
                except (BlockingIOError, InterruptedError):
                    pass
                except OSError:
                    self.grabbed.discard(path)
                    self._last_scan = 0.0


def main() -> int:
    parser = argparse.ArgumentParser(description="Tiramisu PC-to-Xbox F8 KVM Forwarder (evdev + Xbox Guide)")
    parser.add_argument("--host", default=os.environ.get("TIRAMISU_HOST", DEFAULT_HOST), help="Xbox IP (default 10.0.0.23)")
    parser.add_argument("--port", type=int, default=int(os.environ.get("TIRAMISU_PORT", DEFAULT_PORT)), help="WebXfer port (default 9127)")
    parser.add_argument("--dp-port", type=int, default=int(os.environ.get("DP_PORT", DEFAULT_DP_PORT)), help="Xbox Device Portal port (default 11443)")
    parser.add_argument("--dp-user", default=os.environ.get("DP_USER", DEFAULT_DP_USER), help="Xbox Device Portal username (default dev)")
    parser.add_argument("--dp-pass", default=os.environ.get("DP_PASS", DEFAULT_DP_PASS), help="Xbox Device Portal password (default dev)")
    parser.add_argument("--no-portal", action="store_true", help="Disable Win-key Xbox Guide bridge")
    parser.add_argument("--code", default=None, help="8-digit /controls pairing code on the TV")
    parser.add_argument("--sens", type=float, default=1.0, help="Mouse sensitivity multiplier (default 1.0)")
    parser.add_argument("--start-active", action="store_true", help="Grab PC input immediately on launch")
    parser.add_argument("--dry-run", action="store_true", help="Run locally without sending to Xbox")
    args = parser.parse_args()

    token = pair_if_needed(args.host, args.port, args.code, args.dry_run)
    sender = TiramisuSender(args.host, args.port, token, dry_run=args.dry_run)
    portal = XboxPortalClient(
        args.host,
        args.dp_port,
        args.dp_user,
        args.dp_pass,
        enabled=not args.no_portal,
        dry_run=args.dry_run,
    )
    bridge = EvdevKvmBridge(sender, portal, args.sens, args.start_active)
    sender.on_auth_expired = bridge.handle_auth_expired

    def handle_sig(signum, frame):
        bridge.stop_requested = True

    signal.signal(signal.SIGINT, handle_sig)
    signal.signal(signal.SIGTERM, handle_sig)

    print("[tiramisu-kvm] Running (evdev + Xbox Guide). F8: toggle PC<->Xbox, Win: Xbox Guide (Shift+Win: switch routing).")
    try:
        bridge.run()
    finally:
        bridge.cleanup()
        sender.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
