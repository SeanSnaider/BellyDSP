---
version: 1
slug: "site-index-html"
primary_target: "site/index.html"
related_targets: ["site/download/index.html","site/stats/index.html","site/404.html"]
---

---
version: 1
slug: "site-index-html"
primary_target: "site/index.html"
related_targets: ["site/download/index.html","site/stats/index.html","site/404.html"]
---

# bellydsp.com overview

Scope: the overview (`site/index.html`), with the download, stats, and 404 pages joining the same system. Visitor mode: Persuade.

Audience and job: guitarists who will download and play it, and engineers or recruiters looking at it as Sean's project, weighted equally. The guitarist must see what BellyDSP is, that it runs on their computer, and how to get it, within the first viewport; the engineer must find how it is built and the source without hunting.

Proof on hand: the app's own snapshots (the Amp page with each of the eight amps, the cab, the effects, the tuner, the tone match page), real release notes, measured numbers. No audio demos, video, or testimonials exist; none are invented.

Constraints: the app's look (dark ground, hairlines, Geist, emerald only for on/active/correct, the "B." mark), real snapshots only, the stats page public, no generic SaaS template, the download never hard to find. No other companies' product or amp names, no claims about sound, no em dashes.

Memorable moment: the shelf of eight mini heads as the page's index; pick an amp and the page moves to its band, the head arriving in its own material as the emerald underline follows.

Unresolved: audio demos (when Sean records them, each amp band gets one).

## Adaptations from the contract

- The first viewport's kicker became the page's h1, a statement at about 30 px, left; "Glass" holds the display scale under its head (craft floor: no kicker above a heading).
- "01 / 08" became "Amp N, program change N", which carries information a player uses (craft floor: section numbers only when the sequence informs).
- 2026-10-13, the re-skin to the redesigned app: the app's world changed under this contract, so OWN-WORLD now reads as the app's current tokens (ground #121212, hairlines #2c2c2c, the accent orange #ff8a00 in place of emerald, one radius scale), the heads are the redesigned art with their knobs and lit jewel lamps, and the screenshots are the new main screen, Match tone's six-step page, the cab stage, the tuner and the harmonizer's inspector (ASSUMPTIONS WEB20 to WEB23). The structure is unchanged.
- 2026-10-13, the board's tabs: the app's board became three tabs (Pre FX, Amp & Cab with the amp, Gate B and the cab, Post FX), each with an LED strip, so the rig section's board row became the three tabs shown at once, each its tab capsule with an LED per block, then its blocks as cells, the cable's named ends on the wider layouts; the copy names the LEDs and the `[` and `]` keys. The tab icons are the redrawn app icon, its dot orange on #121212. Review captures in `.impeccable/review/site3/`.
- Phones: the top bar keeps the mark, Source and the Download button; Overview and Stats leave it, because at 320 px there's no room for the mark, four links and the button (measured). The mark is the Overview link and Stats is in every page's footer. Below 960 px the shelf keeps its names and scrolls sideways.

## Direction contract

THESIS: The page is the app's amp shelf unrolled. Each of the eight amps owns a full-width band in its own head material (silver Glass, oxblood Ember, black Monolith, tweed Lantern, stone Basalt, navy Comet, steel-and-black Forge, lilac Quartz), with calm dark bands for Match tone, the rest of the rig, and the engineering between them. It refuses the hero, three feature cards, and a logo row.

OWN-WORLD: The app's near-black ground (#0b0c0c), 1px hairlines (#1e2221), Geist for everything and each amp's badge lettering for its name, emerald (#34d399) only for the active cell, lit lights, and correct states. Amp bands take their head's material as a page-scale field built the way the app builds its textures (procedural CSS gradients), with the app's real head snapshot as the band's object.

STORY: A visitor reads what BellyDSP is and gets the download in the first viewport, sees the amps one by one as they scroll or pick from the shelf, learns that it can match a tone from a song, sees the whole rig, then, if they're an engineer, how it is built and tested and where the source is. It closes on the download for their computer.

FIRST VIEWPORT: The top bar (the mark, the nav, a solid Download button for the detected OS) over the shelf, a full-width row of the eight minis with their names, Glass underlined in emerald. Below, Glass's silver band fills the rest: on the left, a kicker naming BellyDSP as a free, open-source amp sim and multi-effects app for macOS and Windows, then "01 / 08", "Glass" at display scale, its voice line, and the download; on the right, the Glass head from the app at large scale with its knob names. Ember's oxblood band starts at the bottom edge.

FORM: Material bands, structure 2 of 7 on the ranked list, seed key 3a9ad9c8.

FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and every shipping raster carrying its provenance
