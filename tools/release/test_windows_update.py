#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Tests for the separate Windows update file (docs/WEBSITE.md, "How counting works"): first installs
download BellyDSP-<v>-windows-setup.exe and WinSparkle downloads BellyDSP-<v>-windows-update.exe (the
same bytes), so GitHub's download counts tell the two apart. Standard library only; no GitHub, no
Windows, no real keys.

    uv run --no-project python tools/release/test_windows_update.py -v

What it checks:
  - make_appcast.py: a Windows appcast points at the update copy with its length; it refuses the setup
    file, and refuses a URL that names a different file than --file (both platforms);
  - lib.sh windows_update_file: makes a byte-identical copy with the right name, refuses other names;
  - the Windows workflow's signing step (its `run:` block, cut out of .github/workflows/windows.yml and
    run with bash and a throwaway key): writes the copy, an appcast pointing at it, and a signature that
    verifies against the copy;
  - add_windows.sh end to end, against a stand-in for GitHub's release API: uploads the setup, the update
    copy, and appcast-windows.xml, with the copy byte-identical to the setup and the appcast's signature
    valid for it. Skipped if Sparkle's sign_update isn't in build-deps/ (tools/fetch_deps.sh).
"""

import base64
import http.server
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.parse
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import ed25519  # noqa: E402

VERSION = "9.9.9"   # never a real release; dist/9.9.9 is removed afterwards


def make_key(tmp):
    """A throwaway Ed25519 key in Sparkle's file format, and its public key."""
    seed = os.urandom(32)
    path = Path(tmp) / "throwaway.key"
    path.write_text(base64.b64encode(seed).decode() + "\n")
    return path, base64.b64encode(ed25519.public_key(seed)).decode()


def fake_installer(path, size=300_000):
    path.write_bytes(b"MZ" + os.urandom(size))
    return path


def appcast_enclosure(xml):
    m = re.search(r"<enclosure ([^>]*)/>", xml)
    return dict(re.findall(r'([\w:]+)="([^"]*)"', m.group(1)))


class MakeAppcast(unittest.TestCase):
    def run_appcast(self, *args):
        return subprocess.run([sys.executable, str(HERE / "make_appcast.py"), *args], capture_output=True, text=True)

    def test_windows_points_at_update_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            update = fake_installer(Path(tmp) / f"BellyDSP-{VERSION}-windows-update.exe")
            out = Path(tmp) / "appcast-windows.xml"
            r = self.run_appcast("--platform", "windows", "--version", VERSION, "--file", str(update),
                                 "--url", f"https://github.com/O/R/releases/download/v{VERSION}/{update.name}",
                                 "--signature", "c2ln", "--out", str(out))
            self.assertEqual(r.returncode, 0, r.stderr)
            enc = appcast_enclosure(out.read_text())
            print(f"\n    windows appcast enclosure: {enc['url']} ({enc['length']} bytes)", end="")
            self.assertTrue(enc["url"].endswith(f"/v{VERSION}/BellyDSP-{VERSION}-windows-update.exe"))
            self.assertEqual(int(enc["length"]), update.stat().st_size)
            self.assertEqual(enc["sparkle:os"], "windows")

    def test_windows_refuses_setup(self):
        with tempfile.TemporaryDirectory() as tmp:
            setup = fake_installer(Path(tmp) / f"BellyDSP-{VERSION}-windows-setup.exe")
            r = self.run_appcast("--platform", "windows", "--version", VERSION, "--file", str(setup),
                                 "--url", f"https://x/{setup.name}", "--signature", "c2ln", "--out", str(Path(tmp) / "a.xml"))
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("windows-update.exe", r.stderr)
            print(f"\n    refused: {r.stderr.strip()}", end="")

    def test_url_must_name_the_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            z = fake_installer(Path(tmp) / f"BellyDSP-{VERSION}-mac.zip", 1000)
            r = self.run_appcast("--platform", "mac", "--version", VERSION, "--file", str(z),
                                 "--url", "https://x/BellyDSP-1.0.0-mac.zip", "--signature", "c2ln", "--out", str(Path(tmp) / "a.xml"))
            self.assertNotEqual(r.returncode, 0)
            r = self.run_appcast("--platform", "mac", "--version", VERSION, "--file", str(z),
                                 "--url", f"https://x/{z.name}", "--signature", "c2ln", "--out", str(Path(tmp) / "a.xml"))
            self.assertEqual(r.returncode, 0, r.stderr)


class WindowsUpdateFile(unittest.TestCase):
    def call(self, path):
        return subprocess.run(["bash", "-c", 'source "$1"; windows_update_file "$2"', "_", str(HERE / "lib.sh"), str(path)],
                              capture_output=True, text=True)

    def test_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            setup = fake_installer(Path(tmp) / f"BellyDSP-{VERSION}-windows-setup.exe")
            r = self.call(setup)
            self.assertEqual(r.returncode, 0, r.stderr)
            update = Path(r.stdout.strip())
            self.assertEqual(update.name, f"BellyDSP-{VERSION}-windows-update.exe")
            self.assertEqual(update.read_bytes(), setup.read_bytes())

    def test_refuses_other_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            other = fake_installer(Path(tmp) / "BellyDSP.exe", 10)
            self.assertNotEqual(self.call(other).returncode, 0)


class WorkflowSigningStep(unittest.TestCase):
    """Runs the Windows workflow's signing step locally, with bash as Actions runs it (bash --noprofile
    --norc -eo pipefail) and the expressions filled in."""

    def test_ci_step(self):
        wf = (REPO_ROOT / ".github" / "workflows" / "windows.yml").read_text()
        m = re.search(r"- name: Make the update copy.*?\n        run: \|\n(.*?)\n\n", wf, re.S)
        self.assertIsNotNone(m, "signing step not found in windows.yml")
        script = "\n".join(line[10:] for line in m.group(1).splitlines())
        with tempfile.TemporaryDirectory() as tmp:
            key, pub = make_key(tmp)
            script = (script.replace("${{ steps.conf.outputs.version }}", VERSION)
                            .replace("${{ steps.conf.outputs.ed_public_key }}", pub)
                            .replace("${{ steps.conf.outputs.releases_repo }}", "Test/BellyDSP"))
            self.assertNotIn("${{", script)
            work = Path(tmp) / "repo"
            (work / "dist").mkdir(parents=True)
            (work / "tools").symlink_to(REPO_ROOT / "tools")
            setup = fake_installer(work / "dist" / f"BellyDSP-{VERSION}-windows-setup.exe")
            shim = Path(tmp) / "bin"
            shim.mkdir()
            (shim / "python").write_text(f'#!/bin/sh\nexec "{sys.executable}" "$@"\n')
            (shim / "python").chmod(0o755)
            env = dict(os.environ, ED_PRIVATE_KEY=key.read_text().strip(), PATH=f"{shim}:{os.environ['PATH']}")
            r = subprocess.run(["bash", "--noprofile", "--norc", "-eo", "pipefail", "-c", script], cwd=work, env=env, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            update = work / "dist" / f"BellyDSP-{VERSION}-windows-update.exe"
            self.assertEqual(update.read_bytes(), setup.read_bytes())
            enc = appcast_enclosure((work / "dist" / "appcast-windows.xml").read_text())
            self.assertEqual(enc["url"], f"https://github.com/Test/BellyDSP/releases/download/v{VERSION}/{update.name}")
            self.assertTrue(ed25519.verify(base64.b64decode(pub), update.read_bytes(), base64.b64decode(enc["sparkle:edSignature"])))
            print(f"\n    CI step: {r.stdout.strip().splitlines()[-1]}", end="")


class FakeGitHub(http.server.BaseHTTPRequestHandler):
    """Just enough of GitHub's release API for github_release.sh: the release exists (id 42) with no
    assets, and uploads are recorded."""
    uploads = []

    def log_message(self, *a):
        pass

    def reply(self, code, body):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path.endswith(f"/releases/tags/v{VERSION}"):
            self.reply(200, {"id": 42, "tag_name": f"v{VERSION}"})
        elif path.endswith("/releases/42/assets"):
            self.reply(200, [])
        else:
            self.reply(404, {"message": "Not Found"})

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        name = urllib.parse.parse_qs(u.query).get("name", [""])[0]
        FakeGitHub.uploads.append((name, self.headers.get("Content-Type"), body))
        self.reply(201, {"id": len(FakeGitHub.uploads), "name": name})


@unittest.skipUnless((REPO_ROOT / "build-deps" / "Sparkle-2.10.0" / "bin" / "sign_update").exists(),
                     "Sparkle's sign_update isn't in build-deps/ (tools/fetch_deps.sh)")
class AddWindowsEndToEnd(unittest.TestCase):
    def test_add_windows(self):
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeGitHub)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        base = f"http://127.0.0.1:{server.server_address[1]}"
        dist = REPO_ROOT / "dist" / VERSION
        try:
            with tempfile.TemporaryDirectory() as tmp:
                key, pub = make_key(tmp)
                ci = Path(tmp) / "ci-artifact"
                ci.mkdir()
                setup = fake_installer(ci / f"BellyDSP-{VERSION}-windows-setup.exe")
                env = dict(os.environ, AMPSIM_GITHUB_API=base, AMPSIM_GITHUB_UPLOADS=base, GITHUB_TOKEN="fake-token",
                           AMPSIM_ED_KEY_FILE=str(key), AMPSIM_ED_PUBLIC_KEY=pub, AMPSIM_RELEASES_REPO="Test/BellyDSP")
                r = subprocess.run([str(HERE / "add_windows.sh"), VERSION, str(ci)], env=env, capture_output=True, text=True)
                print("\n" + "\n".join("      " + l for l in r.stdout.strip().splitlines()), end="")
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                names = [u[0] for u in FakeGitHub.uploads]
                self.assertEqual(names, [setup.name, f"BellyDSP-{VERSION}-windows-update.exe", "appcast-windows.xml"])
                uploaded = {u[0]: u[2] for u in FakeGitHub.uploads}
                self.assertEqual(uploaded[names[0]], setup.read_bytes())
                self.assertEqual(uploaded[names[1]], setup.read_bytes())
                enc = appcast_enclosure(uploaded["appcast-windows.xml"].decode())
                self.assertEqual(enc["url"], f"https://github.com/Test/BellyDSP/releases/download/v{VERSION}/{names[1]}")
                self.assertEqual(int(enc["length"]), len(uploaded[names[1]]))
                self.assertTrue(ed25519.verify(base64.b64decode(pub), uploaded[names[1]], base64.b64decode(enc["sparkle:edSignature"])))
                print(f"\n    uploaded: {', '.join(f'{n} ({len(uploaded[n])} B)' for n in names)}; appcast -> {enc['url'].rsplit('/', 1)[1]}, signature valid", end="")
        finally:
            server.shutdown()
            shutil.rmtree(dist, ignore_errors=True)
            if (REPO_ROOT / "dist").is_dir() and not any((REPO_ROOT / "dist").iterdir()):
                (REPO_ROOT / "dist").rmdir()


if __name__ == "__main__":
    unittest.main()
