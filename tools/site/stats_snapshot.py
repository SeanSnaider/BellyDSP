#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Appends today's download counts to the stats history: one row per BellyDSP asset of every release
(its download_count from GitHub's release API), dated today in UTC. Run once a day by
.github/workflows/stats.yml, which keeps the file on the separate `stats` branch. Standard library only.

    stats_snapshot.py --csv downloads.csv [--repo OWNER/NAME] [--fixture releases.json] [--date YYYY-MM-DD]

Running it twice on one day replaces that day's rows (the later counts win). --fixture and --date are
for tests.
"""

import argparse
import datetime as dt
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import site_lib  # noqa: E402
from build_site_data import conf_get  # noqa: E402


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--csv", required=True)
    p.add_argument("--repo")
    p.add_argument("--fixture")
    p.add_argument("--date", help="pretend today is this date (tests)")
    args = p.parse_args(argv[1:])

    repo = args.repo or conf_get("RELEASES_REPO")
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    if args.date:
        now = dt.datetime.combine(dt.date.fromisoformat(args.date), now.time(), dt.timezone.utc)
    if args.fixture:
        releases = json.loads(Path(args.fixture).read_text(encoding="utf-8"))
    else:
        releases = site_lib.fetch_releases(repo, os.environ.get("GITHUB_TOKEN") or None)

    path = Path(args.csv)
    existing = site_lib.read_csv(path.read_text(encoding="utf-8")) if path.is_file() else []
    new = site_lib.snapshot_rows(releases, now)
    rows = site_lib.merge_snapshot(existing, new)
    path.write_text(site_lib.write_csv(rows), encoding="utf-8")
    print(f"{repo}: {len(releases)} releases, {len(new)} asset rows for {now:%Y-%m-%d}; "
          f"{path} now has {len(rows)} rows over {len({r['date'] for r in rows})} days")
    for r in new:
        print(f"  {r['tag']:10s} {r['asset']:40s} {r['os']:8s} {r['kind']:8s} {r['download_count']:>8s}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
