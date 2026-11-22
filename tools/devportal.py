"""Minimal Xbox Device Portal client for https://<console>:11443 (stdlib only).

Endpoint shapes here are the ones the Dev Mode portal actually serves on a
Series S (verified by the xbox-pc-host / factorio-xbox deployments):

    GET    /api/os/info                                identity + sets CSRF cookie
    GET    /api/app/packagemanager/packages            installed packages
    POST   /api/app/packagemanager/package?package=X   multipart msix + deps + cer
    GET    /api/app/packagemanager/state               install progress
    POST   /api/taskmanager/app?appid=&package=        launch (both base64)
    DELETE /api/taskmanager/app?package=               stop
    GET    /ext/screenshot?download=true               PNG of the TV
    GET    /api/filesystem/apps/files?knownfolderid=LocalAppData
           &packagefullname=&path=                     folder listing (JSON)
    POST   /api/filesystem/apps/folder?...&newfoldername=  create folder
    POST   /api/filesystem/apps/file?...               multipart upload
    GET    /api/filesystem/apps/file?...&filename=     download one file

Notes that cost time to learn the hard way:
  * the portal uses a self-signed certificate on a Dev Mode console, so TLS is
    deliberately unverified here;
  * with "Require authentication" on, basic auth is the Dev Home account;
  * every non-GET needs the `X-CSRF-Token` header, and its value is the
    URL-encoded `CSRF-Token` cookie, which only appears after the first request;
  * a freshly installed package has no LocalState until it runs once - the
    folder API cannot create it.
"""

import base64
import http.client
import json
import os
import ssl
import time
import urllib.parse
import uuid


class DevicePortalError(RuntimeError):
    pass


class DevicePortal:
    def __init__(self, host, user=None, password=None, port=11443,
                 timeout=120, log=print):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.log = log
        self.ctx = ssl._create_unverified_context()
        self.cookies = {}
        self.auth = None
        if user:
            token = base64.b64encode(f"{user}:{password or ''}".encode()).decode()
            self.auth = "Basic " + token

    # ---- transport -------------------------------------------------------

    def call(self, method, path, body=None, headers=None, timeout=None,
             tries=4):
        """One portal request. Retries transport errors with backoff; a real
        HTTP error is returned to the caller, never swallowed."""
        last = None
        for attempt in range(tries):
            conn = None
            try:
                conn = http.client.HTTPSConnection(
                    self.host, self.port, context=self.ctx,
                    timeout=timeout or self.timeout)
                head = dict(headers or {})
                if self.auth:
                    head["Authorization"] = self.auth
                if self.cookies:
                    head["Cookie"] = "; ".join(
                        f"{k}={v}" for k, v in self.cookies.items())
                if "CSRF-Token" in self.cookies:
                    head["X-CSRF-Token"] = urllib.parse.unquote(
                        self.cookies["CSRF-Token"])
                if method in ("POST", "DELETE") and body is None:
                    head["Content-Length"] = "0"
                conn.request(method, path, body=body, headers=head)
                resp = conn.getresponse()
                data = resp.read()
                for key, value in resp.getheaders():
                    if key.lower() == "set-cookie":
                        name, _, rest = value.partition("=")
                        self.cookies[name.strip()] = rest.split(";", 1)[0]
                return resp.status, data
            except (OSError, http.client.HTTPException) as exc:
                last = exc
                if attempt + 1 == tries:
                    break
                time.sleep(2 ** (attempt + 1))
            finally:
                if conn is not None:
                    conn.close()
        raise DevicePortalError(
            f"{method} {path}: {last} "
            f"(is Device Portal on, at https://{self.host}:{self.port}?)")

    # ---- identity --------------------------------------------------------

    def connect(self):
        """Auth + CSRF sanity check. Returns /api/os/info as a dict."""
        status, data = self.call("GET", "/api/os/info")
        if status == 401:
            raise DevicePortalError(
                "Device Portal rejected the login (HTTP 401): check the "
                "--user/--password pair against the account set in Dev Home"
                + ("" if self.auth else ", or pass --user and --password"))
        if status != 200:
            raise DevicePortalError(
                f"Device Portal answered HTTP {status}: {data[:200]!r}")
        return json.loads(data)

    # ---- packages --------------------------------------------------------

    def packages(self):
        status, data = self.call("GET", "/api/app/packagemanager/packages")
        if status != 200:
            return []
        return json.loads(data).get("InstalledPackages", [])

    def find_package(self, name):
        """Match on PackageFullName prefix: `Xan.Tiramisu_<version>_<arch>__<pub>`."""
        prefix = name + "_"
        for pkg in self.packages():
            if pkg.get("PackageFullName", "").startswith(prefix):
                return pkg
        return None

    @staticmethod
    def _multipart(files):
        """files: [(field, filename, bytes)] -> (body, content-type)."""
        boundary = uuid.uuid4().hex
        chunks = []
        for field, name, data in files:
            chunks.append(
                f"--{boundary}\r\n"
                f'Content-Disposition: form-data; name="{field}"; '
                f'filename="{name}"\r\n'
                f"Content-Type: application/octet-stream\r\n\r\n".encode()
            )
            chunks.append(data)
            chunks.append(b"\r\n")
        chunks.append(f"--{boundary}--\r\n".encode())
        return b"".join(chunks), f"multipart/form-data; boundary={boundary}"

    def install(self, package_path, extras=(), wait_seconds=1200):
        """Upload an .msix with its dependency packages + certificate, then
        poll /api/app/packagemanager/state until it resolves."""
        files = []
        for path in [package_path, *extras]:
            with open(path, "rb") as handle:
                files.append((os.path.basename(path), os.path.basename(path),
                              handle.read()))
        body, ctype = self._multipart(files)
        query = urllib.parse.urlencode({"package": os.path.basename(package_path)})
        status, data = self.call(
            "POST", "/api/app/packagemanager/package?" + query, body=body,
            headers={"Content-Type": ctype}, timeout=1800)
        if status not in (200, 202):
            raise DevicePortalError(
                f"install upload: HTTP {status} {data[:300]!r}")
        deadline = time.time() + wait_seconds
        while time.time() < deadline:
            time.sleep(5)
            status, data = self.call("GET", "/api/app/packagemanager/state")
            if status == 204:  # still working, no body
                continue
            result = json.loads(data) if data else {}
            if status == 200 and result.get("Success", True):
                return result
            raise DevicePortalError(
                f"install failed: HTTP {status} {result or data[:300]!r}")
        raise DevicePortalError(
            f"install did not finish in {wait_seconds // 60} minutes")

    # ---- apps ------------------------------------------------------------

    def launch(self, package):
        query = urllib.parse.urlencode({
            "appid": base64.b64encode(
                package["PackageRelativeId"].encode()).decode(),
            "package": base64.b64encode(
                package["PackageFullName"].encode()).decode(),
        })
        status, data = self.call("POST", "/api/taskmanager/app?" + query)
        if status not in (200, 204):
            raise DevicePortalError(
                f"launch: HTTP {status} {data[:200]!r}")

    def stop(self, package):
        query = urllib.parse.urlencode({
            "package": base64.b64encode(
                package["PackageFullName"].encode()).decode(),
        })
        self.call("DELETE", "/api/taskmanager/app?" + query)

    def screenshot(self):
        status, data = self.call("GET", "/ext/screenshot?download=true")
        return data if status == 200 else None

    # ---- an app's LocalAppData ------------------------------------------

    def _query(self, package_full, remote, **extra):
        params = {
            "knownfolderid": "LocalAppData",
            "packagefullname": package_full,
            "path": remote,
        }
        params.update(extra)
        return urllib.parse.urlencode(params)

    def listing(self, package_full, remote):
        """{name: item} for a folder, or None when it does not exist."""
        status, data = self.call(
            "GET", "/api/filesystem/apps/files?" +
            self._query(package_full, remote))
        if status != 200:
            return None
        try:
            return {item["Name"]: item
                    for item in json.loads(data).get("Items", [])}
        except (ValueError, KeyError):
            return None

    def mkdir(self, package_full, parent, name):
        status, data = self.call(
            "POST", "/api/filesystem/apps/folder?" +
            self._query(package_full, parent, newfoldername=name))
        if status not in (200, 204):
            raise DevicePortalError(
                f"mkdir {parent}\\{name}: HTTP {status} {data[:200]!r}")

    def ensure_dir(self, package_full, remote):
        """Create every missing folder of e.g. \\LocalState\\games\\Factorio."""
        parts = [p for p in remote.split("\\") if p]
        for index in range(1, len(parts)):
            parent = "\\" + "\\".join(parts[:index])
            if parts[index] not in (self.listing(package_full, parent) or {}):
                self.mkdir(package_full, parent, parts[index])

    def put_files(self, package_full, remote, paths):
        files = []
        for path in paths:
            with open(path, "rb") as handle:
                files.append((os.path.basename(path),
                              os.path.basename(path), handle.read()))
        body, ctype = self._multipart(files)
        status, data = self.call(
            "POST", "/api/filesystem/apps/file?" +
            self._query(package_full, remote),
            body=body, headers={"Content-Type": ctype}, timeout=900)
        if status not in (200, 204):
            raise DevicePortalError(
                f"upload to {remote}: HTTP {status} {data[:300]!r}")

    def get_file(self, package_full, remote_dir, name):
        status, data = self.call(
            "GET", "/api/filesystem/apps/file?" +
            self._query(package_full, remote_dir, filename=name))
        return data if status == 200 else None

    def push_tree(self, package_full, local, remote, log=None,
                  batch_files=200, batch_bytes=48 * 2 ** 20):
        """Upload a folder under the app's LocalAppData path. Files already
        present at the same size are skipped, so a rerun resumes."""
        log = log or self.log
        root = os.path.abspath(local)
        started = time.time()
        sent_files = sent_bytes = skipped = 0
        self.ensure_dir(package_full, remote)
        for here, dirs, files in os.walk(root):
            rel = os.path.relpath(here, root)
            dirs.sort()
            target = remote if rel == "." else (
                remote + "\\" + rel.replace(os.sep, "\\"))
            have = self.listing(package_full, target) or {}
            for name in dirs:
                if name not in have:
                    self.mkdir(package_full, target, name)
            todo = []
            for name in sorted(files):
                path = os.path.join(here, name)
                size = os.path.getsize(path)
                if have.get(name, {}).get("FileSize") == size:
                    skipped += 1
                    continue
                todo.append((path, size))
            batch, batch_size = [], 0
            for path, size in todo + [(None, 0)]:
                full = (path is None or
                        (batch and (len(batch) >= batch_files or
                                    batch_size + size > batch_bytes)))
                if full:
                    if batch:
                        self.put_files(package_full, target, batch)
                        sent_files += len(batch)
                        sent_bytes += batch_size
                        elapsed = max(time.time() - started, 1)
                        log(f"  {sent_files} files, "
                            f"{sent_bytes / 2 ** 20:.0f} MB, "
                            f"{sent_bytes / 2 ** 20 / elapsed:.1f} MB/s  "
                            f"{target}")
                    batch, batch_size = [], 0
                if path is not None:
                    batch.append(path)
                    batch_size += size
        return sent_files, sent_bytes, skipped, time.time() - started
