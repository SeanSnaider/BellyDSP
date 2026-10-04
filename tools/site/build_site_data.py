#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Writes the website's data files: site/data/releases.json (the download buttons, version, sizes, and
changelog) and site/data/stats.json (the stats page). Run by .github/workflows/pages.yml before every
deploy; standard library only.

    build_site_data.py [--repo OWNER/NAME] [--stats-csv downloads.csv] [--out site/data]
    build_site_data.py --fixture tools/site/fixtures/releases.json --stats-csv tools/site/fixtures/downloads.csv

The repo defaults to RELEASES_REPO in tools/release/release.conf. With --fixture, the release list comes
from a saved API response instead of the network (for local previews and the tests). GITHUB_TOKEN, if
set, is sent with the API calls (only for the rate limit; everything read is public).

The site never calls the GitHub API itself: the pages only read these two files, so a visitor's browser
talks to nothing but bellydsp.com (and github.com when they click a download).

The changelog is release-notes/<version>.md for every version that has a published release, rendered
with the same Markdown subset as the appcasts (tools/release/make_appcast.py).

Exit status: 0 on success, including a repo with no releases yet (the site then shows "no release yet");
non-zero if the API fails, so the Pages workflow stops and the site keeps its last good data.
"""

import argparse
import datetime as dt
import json
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO_ROOT / "tools" / "release"))

import site_lib  # noqa: E402
from make_appcast import markdown_to_html  # noqa: E402


def conf_get(key):
    conf = REPO_ROOT / "tools" / "release" / "release.conf"
    value = ""
    for line in conf.read_text(encoding="utf-8").splitlines():
        if line.startswith(key + "="):
            value = line[len(key) + 1:].strip()
    return value


def notes_html(version):
    """release-notes/<version>.md as HTML for the changelog: its first heading ("# BellyDSP 0.1.0") is
    dropped, since the page shows the version itself, and the rest are moved down to fit under it."""
    path = REPO_ROOT / "release-notes" / f"{version}.md"
    if not path.is_file():
        return None
    lines = path.read_text(encoding="utf-8").splitlines()
    while lines and not lines[0].strip():
        lines.pop(0)
    if lines and re.match(r"^#\s", lines[0]):
        lines.pop(0)
    html = markdown_to_html("\n".join(lines))
    # make_appcast turns "#" into <h2>, "##" into <h3>...; on the page they sit under an <h3> per version.
    for level in (5, 4, 3, 2):
        html = html.replace(f"<h{level}>", f"<h{level + 2}>").replace(f"</h{level}>", f"</h{level + 2}>")
    return html


def asset_info(release, os_name):
    """The installer a visitor on os_name should download from this release, or None."""
    for a in release.get("assets", []):
        o, kind = site_lib.classify(a["name"])
        if o == os_name and kind == "install":
            return {"name": a["name"], "url": a.get("browser_download_url", ""),
                    "size": a.get("size", 0), "download_count": a.get("download_count", 0)}
    return None


def releases_payload(repo, latest, releases, generated_at, source):
    releases_url = f"https://github.com/{repo}/releases"
    source_url = conf_get("SOURCE_URL") or f"https://github.com/{repo}"
    published = sorted(releases, key=lambda r: site_lib.version_key(site_lib.version_of(r["tag_name"])),
                       reverse=True)
    changelog = []
    for rel in published:
        version = site_lib.version_of(rel["tag_name"])
        html = notes_html(version)
        if html is None:
            continue
        changelog.append({"version": version, "published_at": rel.get("published_at") or "",
                          "html_url": rel.get("html_url") or f"{releases_url}/tag/{rel['tag_name']}",
                          "prerelease": bool(rel.get("prerelease")), "notes_html": html})
    payload = {
        "generated_at": generated_at,
        "source": source,
        "repo": repo,
        "releases_url": releases_url,
        "latest_url": f"{releases_url}/latest",
        "source_url": source_url,
        "latest": None,
        "changelog": changelog,
    }
    if latest:
        version = site_lib.version_of(latest["tag_name"])
        payload["latest"] = {
            "version": version,
            "tag": latest["tag_name"],
            "published_at": latest.get("published_at") or "",
            "html_url": latest.get("html_url") or f"{releases_url}/tag/{latest['tag_name']}",
            "source_url": f"{source_url}/tree/{latest['tag_name']}",
            "downloads": {o: asset_info(latest, o) for o in site_lib.OSES},
        }
    return payload


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--repo", help="owner/name (default: RELEASES_REPO in tools/release/release.conf)")
    p.add_argument("--fixture", help="a saved /releases API response (a JSON list) instead of the network")
    p.add_argument("--stats-csv", help="the stats branch's downloads.csv, if there is one")
    p.add_argument("--out", default=str(REPO_ROOT / "site" / "data"))
    args = p.parse_args(argv[1:])

    repo = args.repo or conf_get("RELEASES_REPO")
    if not repo:
        sys.exit("build_site_data.py: no repo (RELEASES_REPO is empty)")
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    generated_at = now.strftime("%Y-%m-%dT%H:%M:%SZ")

    rows = []
    if args.stats_csv and os.path.isfile(args.stats_csv):
        rows = site_lib.read_csv(Path(args.stats_csv).read_text(encoding="utf-8"))
        print(f"stats history: {len(rows)} rows, {len({r['date'] for r in rows})} snapshots")
    elif args.stats_csv:
        print(f"stats history: none at {args.stats_csv} (the stats workflow hasn't run yet)")

    if args.fixture:
        releases = json.loads(Path(args.fixture).read_text(encoding="utf-8"))
        latest = site_lib.latest_from_list(releases)
        source = "fixture"
    else:
        token = os.environ.get("GITHUB_TOKEN") or None
        releases = site_lib.fetch_releases(repo, token)
        latest = site_lib.fetch_latest(repo, token) if releases else None
        source = "api"
    print(f"{repo}: {len(releases)} releases, latest {latest['tag_name'] if latest else 'none'} ({source})")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    rel = releases_payload(repo, latest, releases, generated_at, source)
    (out / "releases.json").write_text(json.dumps(rel, indent=1) + "\n", encoding="utf-8")
    if latest:
        for o, a in rel["latest"]["downloads"].items():
            print(f"  {o:8s} {a['name'] + ' (' + str(a['size']) + ' bytes)' if a else 'no installer in this release'}")
    print(f"  changelog: {', '.join(c['version'] for c in rel['changelog']) or 'empty'}")

    stats = site_lib.stats_payload(releases, rows, generated_at, repo)
    (out / "stats.json").write_text(json.dumps(stats, indent=1) + "\n", encoding="utf-8")
    t = stats["totals"]
    print(f"  downloads: mac {t['mac']['install']}, windows {t['windows']['install']}; updates: mac "
          f"{t['mac']['update']}, windows {t['windows']['update']}; {len(stats['active'])} days of active estimates")
    print(f"wrote {out / 'releases.json'} and {out / 'stats.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
