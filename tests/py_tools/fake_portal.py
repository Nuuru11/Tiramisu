"""A Device Portal stand-in that speaks real HTTPS, so the client is tested
over the wire instead of through a monkeypatched transport.

It implements exactly the endpoints `tools/devportal.py` uses, and it is
deliberately strict about the two things that cost real debugging time on a
console:

  * every non-GET needs `X-CSRF-Token`, and the value only appears after the
    first request set the `CSRF-Token` cookie;
  * basic auth is required when a username is configured.

Run it directly for a manual poke:

    python3 tests/py_tools/fake_portal.py --port 11443
"""

import argparse
import base64
import json
import os
import ssl
import subprocess
import tempfile
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PNG = (b"\x89PNG\r\n\x1a\n"
       b"\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x02"
       b"\x00\x00\x00\x90wS\xde\x00\x00\x00\x0cIDATx\x9cc\xf8\xcf\xc0"
       b"\x00\x00\x03\x01\x01\x00\x18\xdd\x8d\xb0\x00\x00\x00\x00IEND"
       b"\xaeB`\x82")

APP_NAME = "Xan.Tiramisu"
APP_VERSION = "1.4.7.0"


def make_cert(directory):
    """Self-signed cert for 127.0.0.1. The client does not verify it; what
    matters is that the handshake is real."""
    key = os.path.join(directory, "portal.key")
    cert = os.path.join(directory, "portal.crt")
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", key, "-out", cert, "-days", "2",
         "-subj", "/CN=127.0.0.1",
         "-addext", "subjectAltName=IP:127.0.0.1"],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return cert, key


def parse_multipart(body, content_type):
    """Return [(field, filename, bytes)] for a multipart/form-data body."""
    marker = "boundary="
    if marker not in content_type:
        return []
    boundary = content_type.split(marker, 1)[1].split(";", 1)[0].strip('" ')
    parts = []
    for chunk in body.split(b"--" + boundary.encode()):
        if b"\r\n\r\n" not in chunk:
            continue
        head, data = chunk.split(b"\r\n\r\n", 1)
        if data.endswith(b"\r\n"):
            data = data[:-2]
        head_text = head.decode("utf-8", "replace")
        field = filename = None
        for line in head_text.splitlines():
            if line.lower().startswith("content-disposition:"):
                for piece in line.split(";")[1:]:
                    key, _, value = piece.strip().partition("=")
                    if key.lower() == "name":
                        field = value.strip('"')
                    elif key.lower() == "filename":
                        filename = value.strip('"')
        parts.append((field, filename, data))
    return parts


class ConsoleState:
    """Just enough of a console to be wrong in the same ways the real one is."""

    def __init__(self, app=APP_NAME, user=None, password=None):
        self.app = app
        self.user = user
        self.password = password
        self.packages = []          # list of package dicts
        self.folders = set()        # (package, "\\path")
        self.files = {}             # (package, "\\path", "name") -> bytes
        self.install_polls = 0      # 204s to answer before success
        self.install_ok = True
        self.launches = []          # decoded package full names
        self.csrf = "fake-csrf-token"
        self.send_csrf_cookie = True   # a console with auth off may omit it

    # ---- helpers ---------------------------------------------------------

    def package_full(self):
        return f"{self.app}_{APP_VERSION}_x64__8wekyb3d8bbwe"

    def add_package(self):
        full = self.package_full()
        self.packages = [p for p in self.packages
                         if not p["PackageFullName"].startswith(self.app)]
        self.packages.append({
            "PackageFullName": full,
            "PackageFamilyName": self.app + "_8wekyb3d8bbwe",
            "PackageRelativeId": "App",
            "Version": APP_VERSION,
            "Name": self.app,
        })
        self.folders.add((full, "\\LocalState"))
        return full

    @staticmethod
    def norm(path):
        path = (path or "").replace("/", "\\")
        if path and not path.startswith("\\"):
            path = "\\" + path
        return path

    def listing(self, package, path):
        path = self.norm(path)
        if (package, path) not in self.folders:
            return None
        items = []
        for pkg, folder in self.folders:
            if pkg != package or folder == path:
                continue
            if os.path.dirname(folder) == path:
                items.append({"Name": os.path.basename(folder),
                              "FileSize": 0, "FolderType": "Folder",
                              "Type": 1})
        for (pkg, folder, name), data in self.files.items():
            if pkg == package and folder == path:
                items.append({"Name": name, "FileSize": len(data),
                              "FolderType": "File", "Type": 0})
        return items


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    @property
    def state(self):
        return self.server.state

    def log_message(self, *args):  # keep the test output readable
        pass

    # ---- plumbing --------------------------------------------------------

    def _send(self, status, body=b"", ctype="application/json", headers=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _authorised(self):
        state = self.state
        if not state.user:
            return True
        header = self.headers.get("Authorization", "")
        if not header.startswith("Basic "):
            return False
        raw = base64.b64decode(header[6:]).decode("utf-8", "replace")
        return raw == f"{state.user}:{state.password or ''}"

    def _body(self):
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    def _dispatch(self, method):
        if not self._authorised():
            return self._send(401, {"Error": "auth required"})
        parsed = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        body = self._body() if method in ("POST", "DELETE", "PUT") else b""

        if method != "GET":
            token = self.headers.get("X-CSRF-Token")
            if token != self.state.csrf:
                return self._send(403, {"Error": "CSRF token missing"})

        extra = {}
        if parsed.path == "/api/os/info" and self.state.send_csrf_cookie:
            # The cookie only exists after this response, exactly like a console.
            extra["Set-Cookie"] = f"CSRF-Token={self.state.csrf}; path=/"
        return self._route(method, parsed.path, query, body, extra)

    def do_GET(self):
        self._dispatch("GET")

    def do_POST(self):
        self._dispatch("POST")

    def do_DELETE(self):
        self._dispatch("DELETE")

    # ---- endpoints -------------------------------------------------------

    def _route(self, method, path, query, body, extra):
        state = self.state
        if path == "/api/os/info":
            return self._send(200, {
                "ComputerName": "FakeXbox",
                "OsVersion": "10.0.26100.1234",
                "Platform": {"Name": "Xbox", "Model": "Series S"},
            }, headers=extra)

        if path == "/api/app/packagemanager/packages":
            return self._send(200, {"InstalledPackages": state.packages})

        if path == "/api/app/packagemanager/package":
            parts = parse_multipart(body, self.headers.get("Content-Type", ""))
            names = sorted(name for _, name, data in parts if name and data)
            if not any(n.endswith(".msix") or n.endswith(".appx")
                       for n in names):
                return self._send(400, {"Error": "no package in upload"})
            state.add_package()
            state.install_polls = 2
            state.last_install = names
            return self._send(202, {"Error": "", "Success": True})

        if path == "/api/app/packagemanager/state":
            if state.install_polls > 0:
                state.install_polls -= 1
                return self._send(204)
            if not state.install_ok:
                return self._send(500, {"Success": False,
                                        "Error": "install failed (fake)"})
            return self._send(200, {"Success": True, "Error": ""})

        if path == "/api/taskmanager/app":
            if method == "POST":
                package = base64.b64decode(
                    query.get("package", [""])[0]).decode()
                state.launches.append(package)
                return self._send(200, {"Success": True})
            package = base64.b64decode(query.get("package", [""])[0]).decode()
            state.stopped = package
            return self._send(204)

        if path == "/ext/screenshot":
            return self._send(200, PNG, ctype="image/png")

        if path == "/api/filesystem/apps/files":
            package = query.get("packagefullname", [""])[0]
            items = state.listing(package, query.get("path", [""])[0])
            if items is None:
                return self._send(404, {"Error": "no such folder"})
            return self._send(200, {"Items": items})

        if path == "/api/filesystem/apps/folder":
            package = query.get("packagefullname", [""])[0]
            parent = state.norm(query.get("path", [""])[0])
            name = query.get("newfoldername", [""])[0]
            if (package, parent) not in state.folders:
                return self._send(404, {"Error": "no such parent"})
            state.folders.add((package, state.norm(parent + "\\" + name)))
            return self._send(204)

        if path == "/api/filesystem/apps/file":
            package = query.get("packagefullname", [""])[0]
            folder = state.norm(query.get("path", [""])[0])
            if method == "GET":
                name = query.get("filename", [""])[0]
                data = state.files.get((package, folder, name))
                if data is None:
                    return self._send(404, {"Error": "no such file"})
                return self._send(200, data, ctype="application/octet-stream")
            parts = parse_multipart(body, self.headers.get("Content-Type", ""))
            for _, name, data in parts:
                if name:
                    state.files[(package, folder, name)] = data
            return self._send(204)

        return self._send(404, {"Error": f"unknown endpoint {path}"})


class FakePortal:
    """Context manager: `with FakePortal() as portal: portal.port`."""

    def __init__(self, app=APP_NAME, user=None, password=None):
        self.state = ConsoleState(app=app, user=user, password=password)
        self.cert_dir = tempfile.mkdtemp(prefix="fake-portal-")
        self.server = None
        self.thread = None

    def __enter__(self):
        cert, key = make_cert(self.cert_dir)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.state = self.state
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        self.server.socket = context.wrap_socket(
            self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]
        return self

    def __exit__(self, *exc):
        if self.server:
            self.server.shutdown()
            self.server.server_close()
        if self.thread:
            self.thread.join(timeout=5)
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=11443)
    parser.add_argument("--user", default=None)
    parser.add_argument("--password", default=None)
    args = parser.parse_args()
    portal = FakePortal(user=args.user, password=args.password)
    portal.__enter__()
    print(f"fake Device Portal on https://127.0.0.1:{portal.port} "
          f"(ctrl-c to stop)", flush=True)
    try:
        portal.thread.join()
    except KeyboardInterrupt:
        pass
    finally:
        portal.__exit__()


if __name__ == "__main__":
    main()
