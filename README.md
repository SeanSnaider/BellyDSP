# BellyDSP download stats

`downloads.csv` has one row per release asset per day: GitHub's download_count for that file
on that date (UTC). Written once a day by `.github/workflows/stats.yml` on `main`; read by the
website's stats page (`docs/WEBSITE.md` on `main` explains the columns and the estimates).
Nothing here is personal: these are GitHub's public, anonymous per-file counters.
