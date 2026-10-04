# The website (bellydsp.com)

How BellyDSP's website is built, published, and counted, written for you, Sean: what's in it, what to set up once, and what to do when you want to change it.

The short version: plain HTML and CSS in `site/`, published to GitHub Pages from the public repo `SeanSnaider/BellyDSP` by a GitHub Actions workflow, served at **bellydsp.com**. The download buttons and the stats come from GitHub's own release API, read once at deploy time and saved as two small JSON files next to the pages. The site sets no cookies, loads nothing from anywhere else, and has no analytics or page-view counter. The only numbers anyone collects are GitHub's anonymous per-file download counts, which are public anyway.

## What's where

| Path | What it is |
|---|---|
| `site/index.html` | Overview: what BellyDSP does, screenshots, open source, credits |
| `site/download/index.html` | Download: the buttons, requirements, first launch, choosing the input, updates, the changelog, privacy |
| `site/stats/index.html` | Stats: downloads per OS and release, the daily copies-in-use estimate, and how it's counted |
| `site/404.html` | What GitHub Pages shows for a missing page |
| `site/assets/css/site.css` | The whole look: the app's tokens (UI_HANDOFF.md section 3), Geist, hairlines, one emerald accent |
| `site/assets/js/` | `site.js` (shared: loads `data/releases.json`, fills the version line), `download.js` (OS detection, buttons, changelog), `stats.js` (tiles, two SVG charts, tables). Small, no libraries |
| `site/assets/img/` | Screenshots as WebP, two widths each for `srcset` (made by `tools/site/make_site_assets.py`) |
| `site/assets/fonts/` | Geist Regular, Medium, SemiBold as WOFF2, with its licence (SIL OFL 1.1) |
| `site/CNAME`, `site/favicon.svg` | The domain (see "Domain" for why it's informational), the tab icon |
| `site/data/` | Generated at deploy time, not committed (`.gitignore`): `releases.json`, `stats.json` |
| `tools/site/build_site_data.py` | Writes `site/data/` from the API (or a fixture) and the stats history |
| `tools/site/stats_snapshot.py` | Appends today's counts to the stats history (run daily by the stats workflow) |
| `tools/site/site_lib.py` | What each file is, the API calls, and the counting rules, shared by both |
| `tools/site/check_site.py` | Checks the site: links, images, alt text, no outside requests, no em dashes, page weight |
| `tools/site/test_site.py` | Tests for all of the above |
| `tools/site/fixtures/` | A fake but self-consistent API response and 33 days of snapshots, for previews and tests |
| `tools/site/make_site_assets.py` | Rebuilds the screenshots and fonts from `build/proof` and `resources/fonts` |
| `.github/workflows/pages.yml` | Builds `site/data/`, checks the site, and deploys it to Pages |
| `.github/workflows/stats.yml` | Saves every release file's download count once a day on the `stats` branch |

There's no framework and no build step beyond `build_site_data.py`. Every page works without JavaScript: the download buttons then go to the latest release's page on GitHub, which lists every file.

## Previewing it locally

```
uv run --no-project python tools/site/build_site_data.py --fixture tools/site/fixtures/releases.json --stats-csv tools/site/fixtures/downloads.csv
cd site && uv run --no-project python -m http.server 8000
```

Then open http://127.0.0.1:8000. The first command writes `site/data/` from the fixtures (the pages say "Preview data" while it's fixture data); the second serves `site/` the way Pages will. Leave out `--fixture` to read the real API instead (it shows "no release yet" until the first release).

Checks and tests (no network needed):

```
uv run --no-project python tools/site/test_site.py -v
uv run --no-project python tools/site/check_site.py                                # the files
uv run --no-project python tools/site/check_site.py --base http://127.0.0.1:8000   # and over HTTP
```

## How it's deployed

`.github/workflows/pages.yml` ("Website") runs in the public repo when:

- `main` gets a push that touches `site/`, `tools/site/`, `release-notes/`, or the workflow;
- a release is published (`release.sh` publishes with your token, so GitHub fires the event);
- the daily "Download stats" run finishes (fresh counts every day);
- a "Windows build" run for a version tag finishes (it attaches the Windows files with the workflow's own token, which doesn't fire release events, so this is how the site picks them up);
- you run it by hand (Actions > Website > Run workflow).

It checks out `main`, fetches `downloads.csv` from the `stats` branch if there is one, runs `build_site_data.py` (with the workflow's token, only for the API's rate limit), runs `check_site.py`, and deploys `site/` with GitHub's official Pages actions (`configure-pages`, `upload-pages-artifact`, `deploy-pages`). If the API can't be read, or the check fails, nothing deploys and the site stays as it was.

Like the other workflows, it only runs automatically in the public repo (`!github.event.repository.private`); in the private repo only by hand, and there it would fail at the deploy unless Pages were enabled there, which it shouldn't be.

## One-time setup (Sean)

In this order. The repo has to be public first (RELEASING.md, step 1) and `main` pushed to it.

1. **Buy bellydsp.com** at any registrar. Turn on the registrar's privacy protection (WHOIS privacy) if it isn't on by default.
2. **Verify the domain with GitHub** (protects against someone else claiming it for their Pages site): github.com > your profile picture > **Settings > Pages** (the account's settings, not the repo's) > **Add a domain** > `bellydsp.com`. GitHub shows a TXT record: add it at the registrar (host `_github-pages-challenge-SeanSnaider`, the value it shows), wait for it to appear, click **Verify**. Keep that TXT record forever.
3. **DNS records** at the registrar, for the apex (`bellydsp.com`, often written `@` or left blank) and `www`. Delete any parking-page A, AAAA, or CNAME records the registrar added first. These are GitHub's documented Pages addresses (checked on 2026-10-04 at docs.github.com, "Managing a custom domain for your GitHub Pages site"; check again when you do this):

   | Type | Host | Value |
   |---|---|---|
   | A | @ | 185.199.108.153 |
   | A | @ | 185.199.109.153 |
   | A | @ | 185.199.110.153 |
   | A | @ | 185.199.111.153 |
   | AAAA | @ | 2606:50c0:8000::153 |
   | AAAA | @ | 2606:50c0:8001::153 |
   | AAAA | @ | 2606:50c0:8002::153 |
   | AAAA | @ | 2606:50c0:8003::153 |
   | CNAME | www | seansnaider.github.io |
   | TXT | _github-pages-challenge-SeanSnaider | (the value from step 2) |

   Don't add a wildcard (`*`) record. DNS changes can take up to a day to spread. To check from Terminal: `dig bellydsp.com +noall +answer -t A`, `dig bellydsp.com +noall +answer -t AAAA`, `dig www.bellydsp.com +noall +answer`.
4. **Turn Pages on**: SeanSnaider/BellyDSP > **Settings > Pages** > Build and deployment > **Source: GitHub Actions**. (Not "Deploy from a branch": the workflow deploys.)
5. **Run the workflow once**: Actions > Website > Run workflow (on `main`). The first deploy goes to `https://seansnaider.github.io/BellyDSP/` until the domain is set; all the pages' links are relative, so they work there too (only `404.html` assumes the domain).
6. **Set the custom domain**: Settings > Pages > Custom domain: `bellydsp.com` > Save. GitHub checks the DNS. With the `www` CNAME in place, `www.bellydsp.com` redirects to `bellydsp.com`.
7. **Enforce HTTPS**: once GitHub has issued the certificate (minutes to a day after step 6; the checkbox is greyed out until then), tick **Enforce HTTPS** on the same page.
8. **Start the stats**: Actions > Download stats > Run workflow. It creates the `stats` branch with `downloads.csv` and a README. From then on it runs every day at 06:17 UTC.
9. Optional: Settings > Branches > add a rule for `stats` that only allows the workflow to push, if you ever give anyone else write access.

### Domain

`site/CNAME` says `bellydsp.com`, but with a custom Actions workflow GitHub ignores that file (docs.github.com: "If you are publishing from a custom GitHub Actions workflow, no CNAME file is created, and any existing CNAME file is ignored and is not required"). The domain lives in Settings > Pages (step 6). The file is kept so the domain is written down in the repo, and so the site works unchanged if it's ever published from a branch instead.

## How counting works

Everything comes from one public number: GitHub's `download_count` on each release file (the REST API's release assets), which goes up by one every time anyone fetches that file, including through the `.../releases/latest/download/<file>` redirect. No cookies, no trackers, no third-party scripts, no page-view counter. The app itself sends nothing but its update check (Sparkle or WinSparkle fetching the appcast, which carries no ID).

| File in each release | Who fetches it | Counts as |
|---|---|---|
| `BellyDSP-<v>.dmg` | People installing on a Mac (from the site or GitHub) | macOS downloads |
| `BellyDSP-<v>-windows-setup.exe` | People installing on Windows | Windows downloads |
| `BellyDSP-<v>-mac.zip` | Only Sparkle, when an installed copy updates | macOS updates |
| `BellyDSP-<v>-windows-update.exe` | Only WinSparkle, when an installed copy updates | Windows updates |
| `appcast.xml` | Every running Mac copy, about once a day | macOS update checks |
| `appcast-windows.xml` | Every running Windows copy, about once a day | Windows update checks |

**Downloads** = the `.dmg` plus the `-windows-setup.exe`, summed over every release.

**The Windows update file.** Before this site, WinSparkle's appcast pointed at the same `-windows-setup.exe` that people download, so a Windows update and a first install were the same count. Now every release also has `BellyDSP-<v>-windows-update.exe`: the identical bytes under a name nothing links to except `appcast-windows.xml`, signed with the same Ed25519 key (the signature is made from the update file itself; identical bytes give the identical signature). The three places that write the Windows appcast all do it: `tools/release/add_windows.sh` and `release.sh --windows-dir` (through `windows_update_file` in `lib.sh`) and the Windows workflow's signing step. `make_appcast.py` refuses a Windows appcast that points at anything but a `-windows-update.exe`, and any appcast whose URL names a different file than the one it measured and was given the signature of. `tools/release/test_windows_update.py` tests all of that, including the workflow's step run locally and `add_windows.sh` against a stand-in for GitHub's API. The cost: each release stores the Windows installer twice (GitHub doesn't limit release storage for public repos).

**Copies in use (daily estimate).** Every running copy fetches its appcast from `.../releases/latest/download/` about once a day (Sparkle and WinSparkle check every 24 hours while the app is open, and at launch if a day has passed). `stats.yml` saves every file's count once a day; the estimate for a day is how much the appcast counts grew since the day before, per OS. The rules, in `site_lib.daily_active`:

- The growth is summed over the appcasts of **all** releases. On a release day, copies fetch the old release's appcast until the new one is published and the new one's after; summing keeps the whole day.
- A file that wasn't in the previous snapshot (a new release, or a re-uploaded file with a new id) counts from 0.
- A count never goes down; if it ever seems to, that day's growth for that file is taken as 0.
- If a daily run is missed, the growth over the gap is spread evenly over its days, and the chart marks the stretch.
- The first snapshot has nothing to compare with, so the estimate starts on the second day.

### Its limits (also on the stats page)

- It's "copies opened that day", roughly, not "copies installed": a copy that isn't opened doesn't check, and one opened several times a day can check more than once (Sparkle checks at launch if a day has passed since its last check, so mostly once).
- Offline copies, copies behind a firewall that blocks GitHub, and copies with automatic checks turned off never show up.
- Anything else that fetches the appcast (scripts, bots, crawlers, people clicking it on the releases page) counts. The same goes for downloads: a second download by the same person, one never installed, and bots all count.
- GitHub doesn't document exactly what increments `download_count` (whether a `HEAD` request or a CDN-cached fetch counts). It's the number GitHub shows on the releases page, which is what everyone quotes.
- If a file is replaced on a release (`github_release.sh` deletes and re-uploads a file with the same name when a release command is run again), its count restarts at 0 and the fetches of the old copy since the last snapshot are lost.
- Snapshots start the day `stats.yml` first runs; the totals (from the API) cover all time, the daily estimate doesn't.

### Where the history lives

`downloads.csv` on the `stats` branch of the public repo (`https://github.com/SeanSnaider/BellyDSP/blob/stats/downloads.csv`): one row per BellyDSP file of every release per day, with the columns `date, time_utc, tag, version, published_at, asset_id, asset, os, kind, size, download_count`. The branch has no shared history with `main` (it starts as an orphan), so `main`'s log never sees the daily commits, and pushes made with the workflow's own token don't start other workflows. It grows by about 6 rows per release per day (about 300 KB a year for each release), which is fine for years; if it ever gets big, keep only the newest snapshot per week for old months.

## Updating the site

- **Copy**: edit the HTML in `site/` directly. Keep to what the app does; never say how it sounds. No other companies' product or amp names, no band names, no em dashes (`check_site.py` catches dashes).
- **Screenshots**: run the tests with proof (`build/ampsim_tests_artefacts/Release/ampsim_tests --proof-dir build/proof`), then `uv run tools/site/make_site_assets.py`, which turns the editor snapshots it lists into WebP at two widths. Look at each image for anything that shouldn't be public (a preset or file name, a brand) before committing. To add one, add it to `SHOTS` in the script and a `<figure>` in `index.html` (with `width`, `height`, and real alt text).
- **Demos**: two `TODO(Sean)` HTML comments in `site/index.html` mark where a video and audio clips go. Self-host them in `site/assets/media/` (MP4 for video, Opus in `.webm` or AAC in `.m4a` for audio), use `preload="none"`, and say what's playing. No YouTube or SoundCloud embeds: they'd load third-party scripts and cookies, which the site promises not to. Keep the files small (a few MB each); `check_site.py` reports the page weight.
- **Release notes**: the changelog shows `release-notes/<version>.md` for each version that has a published release, so writing the notes for a release (RELEASING.md) is all it takes. Its first `#` heading is dropped (the page shows the version), the rest is the same Markdown subset as the update dialogs.
- **Fonts**: the site serves Geist from `site/assets/fonts/` (converted from `resources/fonts/Geist` by `make_site_assets.py`, licence alongside). Nothing comes from Google Fonts or any CDN.

## What was verified, and what can't be yet

Verified here (2026-10-04): the data script against the fixtures, against an empty repo (`SeanSnaider/BellyDSP` doesn't exist yet: "no release yet"), and against real public repos' API responses (NeuralAmpModelerCore, 12 releases with no files; Sparkle, 118 releases over two pages, with its real asset objects renamed to BellyDSP's file names); the stats workflow's and the Pages workflow's shell steps against a throwaway local git repo (the `stats` branch created as an orphan, a second day appended, a same-day rerun changing nothing, `main` untouched); every page and file served over HTTP; the pages rendered in headless Chrome (the buttons point at the files, the macOS button is primary on a Mac, the changelog and charts draw) and measured for horizontal overflow at 320, 360, and 390 px wide (none).

Not verifiable until the public repo exists: the workflows on GitHub (the Pages deploy, the daily schedule, the `workflow_run` triggers), real counts, the DNS, the certificate, and how GitHub's counters behave with Sparkle's and WinSparkle's real requests.
