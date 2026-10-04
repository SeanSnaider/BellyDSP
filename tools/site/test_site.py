#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Tests for the website's data scripts and checker. Standard library only.

    uv run --no-project python tools/site/test_site.py -v

The counting rules are tested on small hand-made histories whose answers are known; the scripts as a
whole on the fixtures (tools/site/fixtures: a fake but consistent API response and 33 days of
snapshots); the checker on the real site and on a broken copy of it.
"""

import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import site_lib  # noqa: E402
import build_site_data  # noqa: E402

FIXTURES = HERE / "fixtures"


def row(date, tag, asset_id, asset, count):
    os_name, kind = site_lib.classify(asset)
    return {"date": date, "time_utc": date + "T06:00:00Z", "tag": tag, "version": tag[1:], "published_at": "",
            "asset_id": str(asset_id), "asset": asset, "os": os_name, "kind": kind, "size": "1",
            "download_count": str(count)}


class Classify(unittest.TestCase):
    def test_names(self):
        cases = {
            "BellyDSP-0.1.0.dmg": ("mac", "install"),
            "BellyDSP-0.1.0-mac.zip": ("mac", "update"),
            "appcast.xml": ("mac", "appcast"),
            "BellyDSP-12.3.45-windows-setup.exe": ("windows", "install"),
            "BellyDSP-0.1.0-windows-update.exe": ("windows", "update"),
            "appcast-windows.xml": ("windows", "appcast"),
            "Source code (zip)": (None, None),
            "BellyDSP-0.1.0.dmg.sha256": (None, None),
            "Sparkle-2.10.0.tar.xz": (None, None),
        }
        for name, want in cases.items():
            self.assertEqual(site_lib.classify(name), want, name)


class DailyActive(unittest.TestCase):
    def test_steady_growth(self):
        rows = [row("2026-01-01", "v1.0.0", 1, "appcast.xml", 100),
                row("2026-01-02", "v1.0.0", 1, "appcast.xml", 130),
                row("2026-01-03", "v1.0.0", 1, "appcast.xml", 170)]
        self.assertEqual(site_lib.daily_active(rows),
                         [{"date": "2026-01-02", "days": 1, "mac": 30, "windows": 0},
                          {"date": "2026-01-03", "days": 1, "mac": 40, "windows": 0}])

    def test_release_day_sums_old_and_new_feeds(self):
        # 50 copies a day. On 01-02 v1.1.0 comes out: 20 fetch the old feed first, 30 the new one.
        rows = [row("2026-01-01", "v1.0.0", 1, "appcast.xml", 500),
                row("2026-01-02", "v1.0.0", 1, "appcast.xml", 520),
                row("2026-01-02", "v1.1.0", 2, "appcast.xml", 30),
                row("2026-01-03", "v1.0.0", 1, "appcast.xml", 520),
                row("2026-01-03", "v1.1.0", 2, "appcast.xml", 80)]
        self.assertEqual([a["mac"] for a in site_lib.daily_active(rows)], [50, 50])

    def test_os_split_and_other_assets_ignored(self):
        rows = [row("2026-01-01", "v1.0.0", 1, "appcast.xml", 10),
                row("2026-01-01", "v1.0.0", 2, "appcast-windows.xml", 5),
                row("2026-01-01", "v1.0.0", 3, "BellyDSP-1.0.0.dmg", 7),
                row("2026-01-02", "v1.0.0", 1, "appcast.xml", 14),
                row("2026-01-02", "v1.0.0", 2, "appcast-windows.xml", 8),
                row("2026-01-02", "v1.0.0", 3, "BellyDSP-1.0.0.dmg", 70)]
        self.assertEqual(site_lib.daily_active(rows), [{"date": "2026-01-02", "days": 1, "mac": 4, "windows": 3}])

    def test_missed_day_is_averaged(self):
        rows = [row("2026-01-01", "v1.0.0", 1, "appcast.xml", 0),
                row("2026-01-04", "v1.0.0", 1, "appcast.xml", 90)]
        self.assertEqual(site_lib.daily_active(rows), [{"date": "2026-01-04", "days": 3, "mac": 30, "windows": 0}])

    def test_reuploaded_feed_counts_from_zero(self):
        # appcast.xml replaced on 01-02 (new id 9, count restarted): its 12 fetches count; no negative.
        rows = [row("2026-01-01", "v1.0.0", 1, "appcast.xml", 400),
                row("2026-01-02", "v1.0.0", 9, "appcast.xml", 12)]
        self.assertEqual(site_lib.daily_active(rows)[0]["mac"], 12)

    def test_first_snapshot_gives_nothing(self):
        self.assertEqual(site_lib.daily_active([row("2026-01-01", "v1.0.0", 1, "appcast.xml", 400)]), [])
        self.assertEqual(site_lib.daily_active([]), [])


class Snapshots(unittest.TestCase):
    def test_same_day_replaced_and_sorted(self):
        rels = json.loads((FIXTURES / "releases.json").read_text())
        import datetime as dt
        day1 = site_lib.snapshot_rows(rels, dt.datetime(2026, 9, 7, 6, tzinfo=dt.timezone.utc))
        rows = site_lib.merge_snapshot([], day1)
        rows = site_lib.merge_snapshot(rows, day1)
        self.assertEqual(len(rows), len(day1))          # running twice on one day doesn't duplicate
        self.assertEqual(len(day1), 12)                 # 2 releases x 6 BellyDSP assets
        text = site_lib.write_csv(rows)
        self.assertEqual(site_lib.read_csv(text), [{k: str(v) for k, v in r.items()} for r in rows])

    def test_cli_appends(self):
        with tempfile.TemporaryDirectory() as tmp:
            csv_path = Path(tmp) / "downloads.csv"
            for date in ("2026-09-07", "2026-09-08", "2026-09-08"):
                r = subprocess.run([sys.executable, str(HERE / "stats_snapshot.py"), "--csv", str(csv_path),
                                    "--fixture", str(FIXTURES / "releases.json"), "--date", date],
                                   capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)
            rows = site_lib.read_csv(csv_path.read_text())
            self.assertEqual(sorted({r["date"] for r in rows}), ["2026-09-07", "2026-09-08"])
            self.assertEqual(len(rows), 24)


class BuildSiteData(unittest.TestCase):
    def test_from_fixtures(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = subprocess.run([sys.executable, str(HERE / "build_site_data.py"), "--fixture", str(FIXTURES / "releases.json"),
                                "--stats-csv", str(FIXTURES / "downloads.csv"), "--out", tmp], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            print("\n" + "\n".join("      " + line for line in r.stdout.strip().splitlines()), end="")
            rel = json.loads((Path(tmp) / "releases.json").read_text())
            stats = json.loads((Path(tmp) / "stats.json").read_text())
        latest = rel["latest"]
        self.assertEqual(latest["version"], "0.1.1")
        self.assertEqual(latest["downloads"]["mac"]["name"], "BellyDSP-0.1.1.dmg")
        self.assertTrue(latest["downloads"]["mac"]["url"].startswith("https://github.com/SeanSnaider/BellyDSP/releases/download/v0.1.1/"))
        self.assertEqual(latest["downloads"]["windows"]["name"], "BellyDSP-0.1.1-windows-setup.exe")
        self.assertEqual(latest["source_url"], "https://github.com/SeanSnaider/BellyDSP/tree/v0.1.1")
        # Only versions with notes in release-notes/ and a published release appear.
        self.assertEqual([c["version"] for c in rel["changelog"]], ["0.1.0"])
        self.assertNotIn("<h2>", rel["changelog"][0]["notes_html"])
        self.assertNotIn("BellyDSP 0.1.0</h", rel["changelog"][0]["notes_html"])
        api = json.loads((FIXTURES / "releases.json").read_text())
        want_mac = sum(a["download_count"] for r in api for a in r["assets"] if a["name"].endswith(".dmg"))
        self.assertEqual(stats["totals"]["mac"]["install"], want_mac)
        self.assertEqual(stats["downloads_total"], stats["totals"]["mac"]["install"] + stats["totals"]["windows"]["install"])
        self.assertEqual(stats["snapshots"]["count"], 33)
        self.assertEqual(len(stats["active"]), 32)
        self.assertEqual([a["date"] for a in stats["active"] if a["days"] > 1], ["2026-08-26"])
        self.assertEqual([s["version"] for s in stats["releases"]], ["0.1.1", "0.1.0"])

    def test_no_release_yet(self):
        payload = build_site_data.releases_payload("SeanSnaider/BellyDSP", None, [], "now", "api")
        self.assertIsNone(payload["latest"])
        self.assertEqual(payload["latest_url"], "https://github.com/SeanSnaider/BellyDSP/releases/latest")
        stats = site_lib.stats_payload([], [], "now", "SeanSnaider/BellyDSP")
        self.assertEqual(stats["downloads_total"], 0)
        self.assertEqual(stats["active"], [])

    def test_release_without_windows_installer(self):
        rels = json.loads((FIXTURES / "releases.json").read_text())
        latest = dict(rels[0], assets=[a for a in rels[0]["assets"] if "windows" not in a["name"]])
        payload = build_site_data.releases_payload("SeanSnaider/BellyDSP", latest, rels, "now", "fixture")
        self.assertIsNone(payload["latest"]["downloads"]["windows"])
        self.assertIsNotNone(payload["latest"]["downloads"]["mac"])

    def test_latest_from_list_skips_prereleases(self):
        rels = [{"tag_name": "v1.0.0", "published_at": "2026-01-01T00:00:00Z"},
                {"tag_name": "v1.1.0-beta", "published_at": "2026-02-01T00:00:00Z", "prerelease": True},
                {"tag_name": "v0.9.0", "published_at": "2025-12-01T00:00:00Z"}]
        self.assertEqual(site_lib.latest_from_list(rels)["tag_name"], "v1.0.0")


class Checker(unittest.TestCase):
    def run_check(self, site):
        return subprocess.run([sys.executable, str(HERE / "check_site.py"), "--site", str(site)], capture_output=True, text=True)

    def test_real_site_passes(self):
        r = self.run_check(REPO_ROOT / "site")
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_broken_copy_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "site"
            shutil.copytree(REPO_ROOT / "site", copy, ignore=shutil.ignore_patterns("data"))
            index = copy / "index.html"
            text = index.read_text()
            text = re.sub(r'alt="The tuner page[^"]*"', 'alt=""', text, count=1)
            text = text.replace('href="download/"', 'href="downloads/"', 1)
            text = text.replace("</main>", '<img src="https://example.com/x.png" alt="x" width="1" height="1">'
                                "<a href=\"https://example.com/\">x</a> a \u2014 dash</main>", 1)
            index.write_text(text)
            r = self.run_check(copy)
        self.assertEqual(r.returncode, 1)
        for want in ("image without alt text", "broken link downloads/", "loads img from another origin",
                     "link to another site: https://example.com/", "em or en dash"):
            self.assertIn(want, r.stdout)


if __name__ == "__main__":
    unittest.main()
