# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Shared code for the website's data scripts (build_site_data.py, stats_snapshot.py). Standard library
only, so it runs on GitHub's runners with plain python3 and here through uv.

Everything the site knows about downloads comes from GitHub's own release API: each release asset has a
download_count, which GitHub increments every time someone fetches that file. No cookies, no trackers,
no page-view counter (docs/WEBSITE.md, "How counting works").

What each asset is (the names come from tools/release):

    BellyDSP-<v>.dmg                   mac      install   first installs (and manual reinstalls)
    BellyDSP-<v>-mac.zip               mac      update    fetched only by Sparkle, by installed copies updating
    appcast.xml                        mac      appcast   fetched about once a day by every running copy
    BellyDSP-<v>-windows-setup.exe     windows  install   first installs (and manual reinstalls)
    BellyDSP-<v>-windows-update.exe    windows  update    the same bytes, fetched only by WinSparkle updating
    appcast-windows.xml                windows  appcast   fetched about once a day by every running copy
"""

import csv
import datetime as dt
import io
import json
import os
import re
import urllib.error
import urllib.request

API_BASE = os.environ.get("AMPSIM_GITHUB_API", "https://api.github.com")

_PATTERNS = [
    (re.compile(r"^BellyDSP-\d+\.\d+\.\d+\.dmg$"), "mac", "install"),
    (re.compile(r"^BellyDSP-\d+\.\d+\.\d+-mac\.zip$"), "mac", "update"),
    (re.compile(r"^appcast\.xml$"), "mac", "appcast"),
    (re.compile(r"^BellyDSP-\d+\.\d+\.\d+-windows-setup\.exe$"), "windows", "install"),
    (re.compile(r"^BellyDSP-\d+\.\d+\.\d+-windows-update\.exe$"), "windows", "update"),
    (re.compile(r"^appcast-windows\.xml$"), "windows", "appcast"),
]

OSES = ("mac", "windows")

CSV_FIELDS = ["date", "time_utc", "tag", "version", "published_at", "asset_id", "asset", "os", "kind",
              "size", "download_count"]


def classify(name):
    """(os, kind) for a release asset's file name, or (None, None) for anything else."""
    for pattern, os_name, kind in _PATTERNS:
        if pattern.match(name):
            return os_name, kind
    return None, None


def version_of(tag):
    return tag[1:] if tag.startswith("v") else tag


def version_key(version):
    """Sort key for major.minor.patch (anything else sorts first)."""
    parts = re.findall(r"\d+", version)
    return tuple(int(p) for p in parts[:3]) if len(parts) >= 3 else (-1,)


# ---- The GitHub REST API ------------------------------------------------------------------------

class NotFound(Exception):
    pass


def api_get(path, token=None):
    """GET {API_BASE}{path} as JSON. A token (GITHUB_TOKEN on Actions) only raises the rate limit: every
    endpoint used here is public."""
    req = urllib.request.Request(API_BASE + path, headers={
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "bellydsp-site",
    })
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            raise NotFound(path) from e
        raise


def fetch_releases(repo, token=None):
    """Every published release (newest first), following pagination. Drafts are never returned to an
    unauthenticated caller and are skipped anyway."""
    out, page = [], 1
    while True:
        try:
            batch = api_get(f"/repos/{repo}/releases?per_page=100&page={page}", token)
        except NotFound:
            return []          # no such repo yet (or no releases): an empty site, not a failure
        out.extend(r for r in batch if not r.get("draft"))
        if len(batch) < 100:
            return out
        page += 1


def fetch_latest(repo, token=None):
    """The release GitHub calls "latest" (the same one .../releases/latest/download/... redirects to, so
    the same one the apps update to), or None if there's none yet."""
    try:
        return api_get(f"/repos/{repo}/releases/latest", token)
    except NotFound:
        return None


def latest_from_list(releases):
    """GitHub's rule for "latest" when only a list is available (fixtures): the newest non-draft,
    non-prerelease release. (A release can be pinned as latest with make_latest; release.sh always marks
    the newest one, so this agrees.)"""
    candidates = [r for r in releases if not r.get("draft") and not r.get("prerelease")]
    if not candidates:
        return None
    return max(candidates, key=lambda r: (r.get("published_at") or "", version_key(version_of(r["tag_name"]))))


# ---- Snapshots (the stats branch's CSV) ---------------------------------------------------------

def snapshot_rows(releases, now):
    """One CSV row per BellyDSP asset of every release, stamped with today's date (UTC)."""
    rows = []
    for rel in releases:
        for a in rel.get("assets", []):
            os_name, kind = classify(a["name"])
            if os_name is None:
                continue
            rows.append({
                "date": now.strftime("%Y-%m-%d"),
                "time_utc": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
                "tag": rel["tag_name"],
                "version": version_of(rel["tag_name"]),
                "published_at": rel.get("published_at") or "",
                "asset_id": str(a["id"]),
                "asset": a["name"],
                "os": os_name,
                "kind": kind,
                "size": str(a.get("size", "")),
                "download_count": str(a.get("download_count", 0)),
            })
    return rows


def read_csv(text):
    return list(csv.DictReader(io.StringIO(text)))


def merge_snapshot(existing_rows, new_rows):
    """The CSV with today's rows replaced by new_rows (running twice on one day keeps the later counts),
    sorted by date, then tag and asset, so the file diffs cleanly day to day."""
    dates = {r["date"] for r in new_rows}
    kept = [r for r in existing_rows if r["date"] not in dates]
    rows = kept + new_rows
    rows.sort(key=lambda r: (r["date"], version_key(r["version"]), r["asset"], r["asset_id"]))
    return rows


def write_csv(rows):
    buf = io.StringIO()
    w = csv.DictWriter(buf, fieldnames=CSV_FIELDS, lineterminator="\n")
    w.writeheader()
    for r in rows:
        w.writerow({k: r.get(k, "") for k in CSV_FIELDS})
    return buf.getvalue()


# ---- What the stats page shows ------------------------------------------------------------------

def _int(x):
    try:
        return int(x)
    except (TypeError, ValueError):
        return 0


def daily_active(rows):
    """The daily active-install estimate per OS, from the snapshots.

    Every running copy fetches its appcast (appcast.xml or appcast-windows.xml) from
    .../releases/latest/download/, about once a day, and each fetch adds one to that release's appcast
    asset's download_count. So the growth of the appcast counts between two snapshots is roughly the
    number of copies that checked for updates in that time.

    The growth is summed over the appcast assets of ALL releases, not just the newest: on the day a
    release comes out, the old release's appcast was fetched until it was published and the new one's
    after, and both halves belong to that day. An asset that wasn't in the previous snapshot (a new
    release, or a re-uploaded file with a new id) counts from 0. A count can't really go down; if it
    does, it's treated as 0. When snapshots are more than a day apart (a missed run), the growth is
    spread evenly over the days in between and marked with "days" > 1. The first snapshot has nothing
    to compare with, so it gives no estimate.

    Returns [{"date", "days", "mac", "windows"}], oldest first.
    """
    by_date = {}
    for r in rows:
        if r.get("kind") != "appcast" or r.get("os") not in OSES:
            continue
        by_date.setdefault(r["date"], {})[r["asset_id"]] = (r["os"], _int(r["download_count"]))
    dates = sorted(by_date)
    out = []
    for prev_date, date in zip(dates, dates[1:]):
        prev, cur = by_date[prev_date], by_date[date]
        days = max(1, (dt.date.fromisoformat(date) - dt.date.fromisoformat(prev_date)).days)
        growth = {o: 0 for o in OSES}
        for asset_id, (os_name, count) in cur.items():
            before = prev.get(asset_id, (os_name, 0))[1]
            growth[os_name] += max(0, count - before)
        out.append({"date": date, "days": days,
                    **{o: round(growth[o] / days) for o in OSES}})
    return out


def release_summaries(releases):
    """Per release (newest first): downloads (installers), updates (update files), and appcast fetches,
    per OS, from the API's current counts."""
    out = []
    for rel in releases:
        s = {"version": version_of(rel["tag_name"]), "tag": rel["tag_name"],
             "published_at": rel.get("published_at") or "", "html_url": rel.get("html_url", "")}
        for o in OSES:
            for k in ("install", "update", "appcast"):
                s[f"{o}_{k}"] = 0
        for a in rel.get("assets", []):
            os_name, kind = classify(a["name"])
            if os_name:
                s[f"{os_name}_{kind}"] += _int(a.get("download_count"))
        out.append(s)
    out.sort(key=lambda s: version_key(s["version"]), reverse=True)
    return out


def stats_payload(releases, rows, generated_at, repo):
    per_release = release_summaries(releases)
    totals = {o: {k: sum(s[f"{o}_{k}"] for s in per_release) for k in ("install", "update", "appcast")}
              for o in OSES}
    active = daily_active(rows)
    dates = sorted({r["date"] for r in rows})
    return {
        "generated_at": generated_at,
        "repo": repo,
        "totals": totals,
        "downloads_total": sum(totals[o]["install"] for o in OSES),
        "releases": per_release,
        "active": active,
        "snapshots": {"first": dates[0] if dates else None, "last": dates[-1] if dates else None,
                      "count": len(dates)},
    }
