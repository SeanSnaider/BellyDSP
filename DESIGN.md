---
name: BellyDSP
description: The bellydsp.com website, drawn in the desktop app's current visual world, with the app's amp shelf unrolled into page-scale material bands.
colors:
  ground: "#0b0c0c"
  surface: "#111313"
  hairline: "#1e2221"
  hairline-strong: "#2a2f2d"
  ink: "#e6ebe9"
  ink-dim: "#8a938f"
  ink-faint: "#525a57"
  emerald: "#34d399"
  glass-field: "#cdcac0"
  glass-ink: "#17191a"
  ember-field: "#3f1719"
  ember-gold: "#d9b878"
  monolith-field: "#121313"
  lantern-field: "#c8a467"
  lantern-ink: "#26190d"
  basalt-field: "#2c2f31"
  comet-field: "#141e44"
  comet-ice: "#9ff3ff"
  forge-field: "#101112"
  forge-orange: "#ff6a1f"
  quartz-field: "#dccde3"
  quartz-violet: "#533f6c"
typography:
  display:
    fontFamily: "Geist, system-ui, -apple-system, Segoe UI, sans-serif"
    fontSize: "clamp(56px, 8.5vw, 6rem)"
    fontWeight: 600
    lineHeight: 0.95
    letterSpacing: "0"
  display-script:
    fontFamily: "Fraunces, Georgia, serif"
    fontSize: "clamp(56px, 8.5vw, 6rem)"
    fontWeight: 600
    lineHeight: 0.95
    letterSpacing: "0"
  page-title:
    fontFamily: "Geist, system-ui, sans-serif"
    fontSize: "clamp(40px, 5.6vw, 72px)"
    fontWeight: 600
    lineHeight: 1.15
    letterSpacing: "-0.035em"
  headline:
    fontFamily: "Geist, system-ui, sans-serif"
    fontSize: "clamp(32px, 4vw, 52px)"
    fontWeight: 600
    lineHeight: 1.15
    letterSpacing: "-0.03em"
  statement:
    fontFamily: "Geist, system-ui, sans-serif"
    fontSize: "clamp(24px, 2.1vw, 30px)"
    fontWeight: 600
    lineHeight: 1.2
    letterSpacing: "-0.02em"
  title:
    fontFamily: "Geist, system-ui, sans-serif"
    fontSize: "clamp(24px, 2.4vw, 30px)"
    fontWeight: 600
    lineHeight: 1.15
    letterSpacing: "-0.02em"
  voice:
    fontFamily: "Geist, system-ui, sans-serif"
    fontSize: "clamp(20px, 2vw, 24px)"
    fontWeight: 400
    lineHeight: 1.4
  lead:
    fontFamily: "Geist, system-ui, sans-serif"
    fontSize: "clamp(17px, 1.8vw, 20px)"
    fontWeight: 400
    lineHeight: 1.6
  body:
    fontFamily: "Geist, system-ui, sans-serif"
    fontSize: "16px"
    fontWeight: 400
    lineHeight: 1.6
  label:
    fontFamily: "Geist, system-ui, sans-serif"
    fontSize: "14px"
    fontWeight: 400
    lineHeight: 1.6
  mono:
    fontFamily: "ui-monospace, SF Mono, Menlo, Consolas, monospace"
    fontSize: "0.875em"
    fontWeight: 400
rounded:
  mini: "3px"
  code: "5px"
  sm: "8px"
  md: "10px"
  lg: "12px"
spacing:
  gutter: "16px"
  gutter-tablet: "32px"
  gutter-wide: "56px"
  bar: "56px"
  shelf: "64px"
  shelf-wide: "84px"
  max: "1240px"
components:
  button-solid:
    backgroundColor: "{colors.ink}"
    textColor: "{colors.ground}"
    typography: "{typography.body}"
    rounded: "{rounded.md}"
    padding: "10px 22px"
    height: "52px"
  button-solid-hover:
    backgroundColor: "#ffffff"
    textColor: "{colors.ground}"
  button-solid-on-glass:
    backgroundColor: "{colors.glass-ink}"
    textColor: "#f2f1ec"
    rounded: "{rounded.md}"
    padding: "10px 22px"
    height: "52px"
  button-solid-on-lantern:
    backgroundColor: "{colors.lantern-ink}"
    textColor: "#f6ead2"
    rounded: "{rounded.md}"
    padding: "10px 22px"
    height: "52px"
  button-ghost:
    backgroundColor: "transparent"
    textColor: "{colors.ink}"
    rounded: "{rounded.md}"
    padding: "10px 22px"
    height: "52px"
  bar-download:
    backgroundColor: "{colors.ink}"
    textColor: "{colors.ground}"
    typography: "{typography.label}"
    rounded: "{rounded.sm}"
    padding: "8px 16px"
  top-bar:
    backgroundColor: "{colors.ground}"
    textColor: "{colors.ink-dim}"
    typography: "{typography.label}"
    height: "{spacing.bar}"
  shelf:
    backgroundColor: "{colors.ground}"
    textColor: "{colors.ink-dim}"
    typography: "{typography.label}"
    height: "{spacing.shelf}"
  chain-cell:
    backgroundColor: "transparent"
    textColor: "{colors.ink}"
    typography: "{typography.label}"
    rounded: "9px"
    padding: "8px 14px"
  shot:
    backgroundColor: "{colors.surface}"
    rounded: "{rounded.lg}"
  notice:
    backgroundColor: "transparent"
    textColor: "{colors.ink-dim}"
    rounded: "{rounded.md}"
    padding: "12px 16px"
  stat-tile:
    backgroundColor: "{colors.ground}"
    textColor: "{colors.ink}"
    padding: "20px 22px"
---

# Design System: BellyDSP

## Overview

**Creative North Star: "The Amp Shelf, Unrolled"**

bellydsp.com is drawn in the desktop app's current visual world, because PRODUCT.md binds "the site looks like the app": a near-black ground, 1px hairlines, Geist for everything, and emerald only for what is on, active, or correct. Onto that calm ground the site lays the app's eight amp heads as page-scale bands. Each band is built from its head's material (silver Glass, oxblood Ember, black Monolith, tweed Lantern, stone Basalt, navy Comet, steel-and-black Forge, lilac Quartz) the way the app builds its textures, with procedural CSS gradients, and the app's own rendered head sits on it as the band's object. Everything that isn't an amp (Match tone, the rest of the rig, how it's built, the download) lives on calm dark bands between them.

The density is low and the scale is generous: one idea per band, a 4/8 column split, the amp's name at display size beside or under its head. The imagery is only real app snapshots and heads rendered by the test suite. There's no hero illustration, no feature-card grid, no logo row.

**This world follows the app's.** The desktop app is about to be redesigned with the whole look open (PRODUCT.md, "The app"). When the app's new world lands, this document and the site get revised to match it. Until then, the tokens below are the app's current look as the site carries it.

**Key Characteristics:**
- Near-black ground (`ground`), separated by 1px hairlines rather than by shadows or fills.
- One accent, emerald, which means on, active, or correct.
- Eight amp materials, used only as full-width bands, inner-page bands, and the shelf's minis.
- Geist throughout. Each amp's name is set in its badge's own lettering.
- The app's real heads and snapshots are the only pictures.
- One authored motion: the head arrives as its band scrolls in. The shelf light follows the current amp.

## Colors

A cool near-black ground with grey-green ink, one emerald signal, and eight amp materials that only ever show up at band scale.

### Primary
- **Signal Emerald** (`emerald`): the shelf light under the current amp, the 2px underline under the current page in the nav, the dot on each active chain cell, the dot in the mark and favicon, the "detected" dot in the download page's OS note, the focus ring, the text selection (at 28% alpha), the caret, and `accent-color`.

### Neutral
- **Amp-Room Black** (`ground`): the page, the chrome, the calm bands, stat tiles. Also `theme-color`.
- **Panel Black** (`surface`): behind screenshots, code, pre blocks, and tooltips.
- **Hairline** (`hairline`): every divider: under the bar and the shelf, the tops of calm bands and doc sections, list and table rules, footer.
- **Strong Hairline** (`hairline-strong`): the borders of interactive or framed things: ghost buttons, chain cells, screenshots, step counters, notices, the scrollbar thumb.
- **Bone Ink** (`ink`): headings, strong text, the solid button's fill on dark.
- **Dim Ink** (`ink-dim`, 6.4:1 on the ground): body copy, leads, nav links at rest, captions.
- **Faint Ink** (`ink-faint`, 2.7:1): hairline-level marks only (the chart crosshair). Never content.

### Amp materials
Each band sets its own `--text` and `--soft` tints from its material, so copy on it reads in that material's ink, and its buttons are re-inked to match.
- **Glass** (`glass-field`, a cream-silver gradient with fine vertical brushing; ink `glass-ink`).
- **Ember** (`ember-field`, an oxblood radial with a woven cross-grid; the name in `ember-gold` script).
- **Monolith** (`monolith-field`, with horizontal slats fading in from the right).
- **Lantern** (`lantern-field`, tweed diagonals; ink `lantern-ink`).
- **Basalt** (`basalt-field`, a stone radial flecked with light and dark grains).
- **Comet** (`comet-field`, a navy radial with a scatter of stars and a `comet-ice` glow).
- **Forge** (`forge-field`, a fine dot grid with a 3px `forge-orange` rule along the bottom edge; the program-change rule turns orange too).
- **Quartz** (`quartz-field`, a lilac gradient with two faint facet sheens; the name in `quartz-violet`).

### Named Rules
**The Emerald Means On Rule.** Emerald marks the current, the active, the focused, or the correct, and nothing else. It never fills a surface, a button, or a heading. The only emerald inside an amp material is a detail the app's own head already has (Monolith's emerald bar), and that belongs to the head.

**The Materials Stay On Their Amps Rule.** An amp material appears as a full band, as an inner page's top band, or as that amp's mini on the shelf. Calm bands, doc sections, and components never borrow an amp's colors as accents.

## Typography

**Display Font:** Geist (with system-ui, -apple-system, Segoe UI, sans-serif), self-hosted at 300, 400, 500 and 600.
**Badge Script:** Fraunces SemiBold Italic (with Georgia, serif), self-hosted, only for the Ember and Lantern names.
**Mono:** ui-monospace, SF Mono, Menlo, Consolas.

**Character:** Geist, the app's own face, does almost everything at 400 and 600. The voice comes from each amp's name taking its badge's lettering: light and wide for Glass, gold script for Ember, spaced capitals for Monolith, Basalt and Quartz, a skewed face with streaks for Comet, condensed capitals for Forge.

### Hierarchy
- **Display** (`display`): an amp's name, beside or under its head. Monolith, Basalt and Quartz step down to `clamp(36px, 4.6vw, 3.6rem)` because of their wide tracking. Names never wrap.
- **Page title** (`page-title`): an inner page's h1 on its material band ("Download BellyDSP", "Stats", "Nothing here.").
- **Headline** (`headline`): a calm band's h2, max 16em, ending with a full stop ("Match a tone from a song.").
- **Statement** (`statement`): the overview's h1, a plain sentence about what BellyDSP is, max 17em, above the download. On purpose, it's smaller than the Glass name beside it.
- **Title** (`title`): a doc section's h2, sticky in its 300px column on wide screens.
- **Voice** (`voice`): an amp's one-line voice under its name, max 22em.
- **Lead** (`lead`): the paragraph under a headline or page title, in dim ink, max 34 to 38em.
- **Body** (`body`, 16px/1.6): copy in dim ink, max 44em in docs. h3 is 17 to 20px at 600.
- **Label** (`label`, 13 to 14px): nav, shelf names, captions, meta lines, the program-change line, tile labels.

### Named Rules
**The Name Is The Display Rule.** Display scale belongs to the amp names. Statements, page titles, and headlines sit below it, and nothing sits above a heading as a label.

**The One Family Rule.** Geist sets everything except the two amps whose badges are script (Ember, Lantern), which get Fraunces. No third face. Comet's slant is a skew because Geist has no italic.

**The Tabular Numbers Rule.** Numbers that get compared (tables, stat values, `.num`) use tabular figures.

## Layout

- **Container:** max width `max` (1240px), centered, with a side gutter of 16px, 32px at 720px and up, and 56px at 1280px and up.
- **The chrome** is sticky at the top: the bar (`bar`, 56px) over the shelf (`shelf`, 64px, or 84px at 960px and up). Anchor jumps use a scroll padding of both together.
- **Amp bands:** full width. Their content is a grid, one column on narrow screens and `4fr / 8fr` (copy / head) from 960px with a 48px gap. Bands alternate with `flip` (`8fr / 4fr`, the copy moved second), so the heads zigzag down the page. Vertical padding is `clamp(72px, 9vw, 128px)`.
- **The opening band** is Glass at a shorter padding (`clamp(40px, 5vw, 72px)`). Its minimum height leaves the top of Ember's band showing at the bottom of the first viewport.
- **Calm bands:** a hairline on top and `clamp(80px, 10vw, 136px)` of vertical padding. A head block (headline and lead) sits `clamp(40px, 5vw, 64px)` above the content.
- **Inner pages:** a single-column material band at the top (`clamp(56px, 7vw, 104px)` vertical padding), then the doc.
- **The doc:** sections divided by hairlines, `clamp(48px, 6vw, 80px)` of padding, and from 960px a `300px / 1fr` grid with the h2 sticky under the chrome.
- **Grid items** always get `min-width: 0`, so long code and wide tables scroll inside their own box.

### Responsive rules
- **Below 960px the shelf keeps its names and scrolls sideways.** The scrollbar is hidden, the minis shrink from 48 x 32 to 36 x 24, and the current amp scrolls itself into view without moving the page.
- **Below 720px the top bar keeps the mark, Source and Download.** Overview, Download and Stats leave the nav (at 320px there's no room for all of them), and the bar button drops its "for macOS" suffix. The mark links to the overview, and Stats is in every page's footer.
- **Below 720px the match result** shows at 640px wide and scrolls sideways so its lines stay readable.
- **Below 960px Comet's streaks** move inside the column instead of hanging into the gutter.

## Elevation & Depth

The page is flat. Depth comes from the hairlines between flat fields and from material bands sitting edge to edge against the black. Only objects cast shadows: the amp heads and the large screenshots. The shadows are soft and drop straight down, never offset.

### Shadow Vocabulary
- **Head on its band** (`filter: drop-shadow(0 30px 34px rgba(0, 0, 0, 0.32))`): every band's head image, following the head's own outline.
- **Featured screenshot** (`box-shadow: 0 24px 48px rgba(0, 0, 0, 0.45)`): a lone large screenshot on a calm band. Screenshots in a group (the Match tone steps, the rig shots) drop it.
- **Mini ring** (`box-shadow: 0 0 0 1px rgba(255, 255, 255, 0.08)`): the shelf's minis, as the app draws them. A ring, not a shadow.

### Named Rules
**The Objects Cast, The Page Doesn't Rule.** Cards, buttons, tiles, notices and bands never get a shadow. When a surface needs separating, use a hairline.

## Shapes

Softly rounded rectangles, small radii, 1px strokes. Buttons and framed boxes (notices, stat tiles, charts) use `md` (10px). Screenshots use `lg` (12px), the bar button, pre blocks and tooltips `sm` (8px), inline code `code` (5px), and minis `mini` (3px, or 4px at full size). Status marks are circles: the 7px mark dot, the 6px chain dot, and the 26px step counters. The focus ring is a 2px emerald outline, offset 3px, at a 4px radius. Dividers are always 1px solid, except for one convention: dashed borders mark the chain's input and output, which are endpoints and not blocks.

## Components

### Top bar
The app's top bar: plain, dark, and dense.
- **Layout:** a 56px row with a hairline under it. The mark sits on the left, then the nav pushed right, then the download button.
- **The mark:** "BellyDSP" in Geist 600 at 15px, after a 7px emerald dot.
- **Nav:** 14px dim-ink links that go to ink on hover. The current page gets ink plus a 2px emerald underline on the bar's bottom edge.
- **Download button** (`bar-download`): solid ink on the ground, 14px at 500, 8px radius, white on hover. On the overview a script adds "for macOS" or "for Windows".

### The shelf
The page's index: the app's shelf of eight minis, each with its name.
- **Minis:** each one is drawn in CSS the way the app draws it: the body, a panel bar across the top, and one detail of its own (Lantern's amber light, Comet's streak, Forge's orange line, Quartz's gem, Monolith's emerald bar).
- **Links:** 14px dim ink, going to ink on hover and when current. Spread across the width from 960px, scrolling sideways with 24px gaps below that.
- **The shelf light:** a 2px emerald bar on the shelf's bottom edge, sized to the current mini and moved under it with `transform` and `width` at `520ms cubic-bezier(0.16, 1, 0.3, 1)`. The current amp is the band crossing a reading line 30% of the way down the space below the chrome. Between amp bands, the last amp passed stays lit. Inner pages show the shelf as plain links to the overview's bands, with no light.

### Amp band
The site's signature: one amp, in its own material, at page scale.
- **Structure:** a full-width section in one of the eight materials (`b-<amp>`), with the material's grain on a `::before` layer behind the content (hidden in forced-colors mode). Inside: the copy column (name, voice, an optional note in soft ink at max 30em, then the program-change line) and the head figure.
- **The program-change line:** "Amp N, program change N" at 13px soft ink, after an 18px rule (orange on Forge). It's information a player uses, not a numbered label.
- **The head:** the app's own rendered head, full column width, with the head shadow.
- **Arrival motion:** where `animation-timeline: view()` is supported and reduced motion isn't requested, the head rises from 44px below at 96.5% scale, with a shallow shadow, to rest with its full shadow, over `entry 5%` to `cover 40%` of its view timeline. The page is complete without it.
- **Buttons on light materials** are re-inked: Glass and Lantern swap the solid button to their ink and the ghost border to their ink at about 30%.

### Calm band
The quiet room between amps.
- **Style:** the ground, a hairline on top, generous padding, a headline and a dim lead, then content. No material, no accent beyond what's active.

### Inner-page material band
The amp band, reused as an inner page's header: a single column, a page title, a lead in soft ink, and the page's own controls (the download buttons at min 250px wide, the version line, the OS note, notices with a 30% text-colored border). Download uses Lantern, Stats uses Basalt, and the 404 uses Monolith, vertically centered at nearly full viewport height.

### Doc layout
Long-form content under an inner-page band: hairline-separated sections, the h2 sticky on the left in 300px from 960px, and the body on the right in dim ink with ink for `strong`. Its parts are a requirements table (a 30% header column with hairline rows), two OS columns from 900px, numbered steps in 26px circled counters, pre blocks that wrap the copyable line, a changelog of hairline-separated versions, and stat tiles in a 1px-gap hairline grid (two columns, four from 720px) with 28 to 38px tabular values.

### Buttons
Big, quiet, and confident. A button can carry a second line.
- **Shape:** gently rounded (`md`, 10px), at least 52px tall, padded 10px 22px, 16px Geist 500. It can stack a 12px meta line at 72% opacity.
- **Solid** (`button-solid`): ink on the ground, white on hover. One per group: the download for this computer.
- **Ghost** (`button-ghost`): transparent, with a strong-hairline border that goes to dim ink on hover. Used for the other OS, the source, the build plan, and first-launch steps.
- **Press:** moves down 1px. Transitions are 160ms on the shared ease-out.
- **Detected:** on the download page, the script marks this computer's button, which turns solid and moves first.

### Match tone steps (tm)
The song and the take side by side (columns in the ratio of the screenshots' real widths, `1042fr / 570fr`), and the match under both at full width, with tops aligned. Each step is an h3 at 17px, a framed screenshot with no shadow, and a dim paragraph at max 40em.

### Rig layouts
- **Chain row:** the signal chain as cells with a 9px radius and a strong-hairline border. Active blocks get a 60% emerald border and a 6px emerald dot. Input and output are dashed and dim.
- **Rig split:** a definition list (terms at 600, a hairline between entries) beside the featured Amp-page screenshot, `4fr / 7fr` from 960px.
- **Rig shots:** a two-column grid of framed screenshots with no shadow. A `wide` shot spans both columns. Captions are 14px dim ink, max 34em.

### Facts list
The engineering's evidence: a definition list, `240px / 1fr` from 840px, with each term at 600 and a hairline across both columns. Definitions are in dim ink, with the measured number in ink at 500 ("**0, 0, 0**", "**6%**"). Credits reuse it.

### Footer
A hairline on top, 14px dim ink: the copyright and licence on the left, then Privacy, Stats and Source.

## Do's and Don'ts

### Do:
- **Do** set every new surface on the ground, with hairlines for structure.
- **Do** give an amp's content its own material band, with its tints for text and its re-inked buttons, and its real head from the app.
- **Do** build materials from CSS gradients, the way the app builds its textures, with the grain on its own layer.
- **Do** set an amp's name in its badge's lettering, at display scale, on one line.
- **Do** keep one solid button per group, for the download that fits this computer.
- **Do** put every motion behind `prefers-reduced-motion: no-preference`, and use the shared ease (`cubic-bezier(0.16, 1, 0.3, 1)`).
- **Do** show measured numbers in ink at 500 inside dim copy, in tabular figures where they're compared.
- **Do** revise this document when the app's redesigned world lands. The site follows the app.

### Don't:
- **Don't** use emerald for decoration, fills, headings, or buttons. It means on, active, or correct.
- **Don't** use an amp material outside its band, its inner-page band, or its mini.
- **Don't** put a label or a number above a heading.
- **Don't** use mockups, stock, or illustrations. Only the app's real snapshots and heads.
- **Don't** shadow surfaces. Only heads and a lone featured screenshot cast a shadow, soft and straight down.
- **Don't** add a third typeface or set body text in Fraunces.
- **Don't** hide the download: the bar keeps its Download button at every width.
