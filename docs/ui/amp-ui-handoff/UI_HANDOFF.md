# Amp modeler UI: implementation handoff

This folder is the full spec for rebuilding the app's UI. It contains:

- `ui_reference.html`: an interactive reference build. **This is the source of truth.** Every color, size, and interaction below comes from it. Open it in a browser to see behavior; read its CSS and JS for exact values.
- `screenshots/`: 2x renders of every page at the native 1280 x 760 window size.
- `UI_HANDOFF.md`: this file. Structure, tokens, component specs, behavior, and rules.

If this document and the HTML ever disagree, the HTML wins.

---

## 0. Before writing any code

1. Inspect the existing repo. Identify the GUI framework in use (egui, iced, vizia / nih-plug, Slint, Tauri webview, or other) and how the UI currently talks to the audio engine.
2. Report back: the framework, the current parameter plumbing, and a short plan mapping each section of this doc onto that framework. Wait for approval before large changes.
3. Do **not** change DSP, NAM inference, or audio-thread code as part of the UI work, except to expose parameters or meter values the UI needs.

## 1. Non-negotiable engineering rules

- **The UI never blocks the audio thread.** No locks shared with the audio callback, no allocation on the audio thread, no UI-driven work inside the callback.
- Parameters flow UI to audio through lock-free primitives (atomic floats, or a lock-free SPSC ring buffer for events like amp switches and preset loads).
- Meters, tuner pitch, and gate state flow audio to UI through atomics the UI polls at frame rate. The UI never reads audio buffers directly.
- Parameter changes must be smoothed on the audio side (one-pole or linear ramp, roughly 10 to 30 ms) so knob drags never zipper.
- Amp switching (including MIDI program change) must be click-free. Crossfade between amp outputs over roughly 20 to 50 ms rather than hard-swapping models.
- Every piece of UI state that defines the sound (amp, knobs, toggles, cab, mics, bypasses) must be serializable, because presets will save it.

## 2. Window

- Fixed logical size **1280 x 760**. Scales uniformly with the window (keep aspect ratio, letterbox if needed). All px values in this doc are at 1x.
- Window corner radius 12, 1px border `line`.
- Grid: top bar 56px, main area fills, bottom signal chain 72px. Single column.

```
+------------------------------------------------------------------+
| brand        < preset name >  Save           Tuner E   In  Out    |  56
+------------------------------------------------------------------+
|  main page content (amp / cab / tuner / fx pages)                 |
|  padding 18 top, 40 sides, 16 bottom                              |  632
+------------------------------------------------------------------+
|  Input -[Pre FX]-[Amp]-[EQ]-[Cab]-[Post FX]- Output              |  72
+------------------------------------------------------------------+
```

## 3. Design tokens

### Colors (UI chrome)

| Token | Hex | Use |
|---|---|---|
| bg | `#0b0c0c` | Window background |
| surface | `#111313` | Knob bodies in chrome, hover fills |
| line | `#1e2221` | 1px dividers and borders |
| line-2 | `#2a2f2d` | Stronger borders, inactive knob tracks, off states |
| ink | `#e6ebe9` | Primary text |
| ink-dim | `#8a938f` | Secondary text |
| ink-faint | `#525a57` | Tertiary text, axis labels |
| accent | `#34d399` | Emerald. The only accent. Active, on, in-tune, value arcs |
| accent-dim | `rgba(52,211,153,0.14)` | Soft emerald fills (tuner in-tune zone, glows) |

Rule: emerald only means "active, on, or correct." Never use it decoratively.

### Typography

- UI font: **Geist** (weights 300, 400, 500, 600). Fallback Inter, then system sans.
- Ember amp badge only: **Fraunces**, italic, 600.
- Bundle both font files with the app; do not load from the web at runtime.
- Sizes used: 11 (axis labels, small meta), 12 (knob labels, secondary), 13 to 14 (body, nav), 15 (amp tabs, brand), 24 to 52 (amp badges), 180 (tuner note).
- Numbers that change live (knob values, Hz, cents) use tabular figures.

### Shape

- Radii: window 12, amp head 14, chain blocks and cards 10, amp panel and grille 6, small buttons 5 to 6, mini amp icons 3.
- Borders are always 1px. No drop shadows in UI chrome; shadows only on amp heads and knobs.

## 4. Components

### 4.1 Knob (used everywhere)

Geometry (default size; small variant in brackets):
- Hit area 64 x 64 [52 x 52], plus label below with 6px gap.
- Track arc: radius 32 in a 72px box, sweep 270 degrees from -135 to +135 (0 = straight up), stroke 2, round caps, color = skin `track`.
- Value arc: same radius, from -135 to current angle, stroke 2, emerald, hidden at minimum.
- Body: circle 40 [32] diameter, centered, radial gradient from `knob-a` (highlight at 38% / 30%) to `knob-b`, 1px ring `knob-ring`, small drop shadow.
- Pointer: 3 x 10 rounded bar, 4px from body top edge, color `knob-ptr`. Body rotates with value.
- Label: 12px, weight 500, color `knob-lbl`.

Interaction:
- Vertical drag: 200px of travel = full range. Hold Shift: 800px (fine mode).
- Double-click: reset to default.
- Scroll wheel: 1/50 of range per notch.
- Arrow keys when focused: 1/50 of range.
- While dragging, the label text is replaced by the formatted value (e.g. `6.5`, `+2.0 dB`, `80 ms`). On amp panels the value gets an emerald underline; in chrome the value turns emerald.
- Value formats: 0 to 10 knobs one decimal; dB values signed with one decimal; Hz, ms, inches as shown in the reference.

Skins (CSS variables in the reference):

| Skin | knob-a | knob-b | ring | pointer | label | track |
|---|---|---|---|---|---|---|
| Chrome (default UI) | `#1a1d1c` | `#111313` | `#2a2f2d` | `#e6ebe9` | `#8a938f` | `#2a2f2d` |
| Glass panel | `#fafafa` | `#9ea2a0` | `#6d716f` | `#1d1f1e` | `#2b2e2c` | `rgba(0,0,0,.18)` |
| Ember panel | `#3a3633` | `#0f0e0d` | `#000000` | `#efdfba` | `#d8c49a` | `rgba(239,223,186,.16)` |
| Monolith panel | `#3c3f3e` | `#141515` | `#050505` | `#34d399` | `#9aa4a0` | `rgba(255,255,255,.09)` |

Build the knob once as a reusable widget that takes a skin, range, default, and formatter.

### 4.2 Toggle

28 x 16 pill, 1px border in current text color, 8px dot. Off: dot left, muted. On: border and dot emerald, dot right. Label 12px to the right with 8px gap. Click, Space, or Enter flips it.

### 4.3 Top bar (56px)

- Left: brand, emerald 7px dot + `rig` in 15px / 600 (placeholder name).
- Center: prev arrow, preset name (14px / 500) with `User` tag (12px faint), next arrow, `Save` outlined button.
- Right: `Tuner` button showing the current detected note in emerald. When the tuner page is open, the button gets a 1px emerald underline at the bottom edge. Then In and Out meters: 56 x 2 bars, emerald fill, labels 11px faint.

### 4.4 Bottom signal chain (72px)

- 1px `line` border on top. Horizontal 1px `line-2` connector line running behind the blocks at vertical center.
- Order: Input, Pre FX, Amp, EQ, Cab, Post FX, Output. 16px gaps, centered.
- Each processing block: height 44, padding 0 14, radius 10, 1px `line-2` border, filled with `bg` so it covers the connector. Contents: 20px line icon, label (14px / 500), 7px bypass dot.
- Input and Output are fixed endpoints: no border, no bypass dot, faint color.
- Active page block: emerald border, emerald icon, ink label.
- Bypass dot: emerald when engaged, `line-2` when bypassed. Clicking the dot toggles bypass **without** navigating. Clicking anywhere else on the block opens that page.
- Bypassed block: icon at 35% opacity, label struck through.
- Icons are simple 24-unit line drawings, stroke 1.5, round caps (jack, pedal, amp head, three sliders, speaker, sine wave, exit arrow). Exact paths are in the reference HTML.

### 4.5 Amp page

Vertical stack, 16px gaps:
1. **Amp tabs**: text tabs with a 30 x 20 mini amp icon, amp name 15px / 500, and MIDI program number (`PC 1` etc.) in 11px faint. Selected tab: ink text, 1px emerald underline. 1px `line` under the tab row.
2. **Amp head stage**: 290px tall, faint emerald radial glow at the bottom center. Head is 960 x 262 centered.
3. **Info row**: amp voice description on the left (14px dim), `Model <file>.nam` and `48 kHz` on the right (13px faint).
4. **Tone response area**: fills remaining height (see section 6).
5. **Shared strip**: Input (Input), Gate (Threshold, Release, plus 6px gate-open LED), Output (Doubler, Output). Small chrome knobs, group names 12px faint, 1px dividers between groups.

### 4.6 Amp head (one shared component)

Structure, identical for all three amps:
- Handle: 150 x 14 dark rounded tab centered above the head.
- Body: 960 x 262, radius 14, padding 16, 12px gap between sections, soft drop shadow.
- **Control panel on top**: height 118, radius 6, padding 0 22. Left column (96px): pilot light jewel (18px emerald glowing sphere) and the amp's two toggles. Right: the amp's knobs, evenly spaced.
- **Grille on the bottom**: fills remaining height, radius 6, inset shadow, amp name badge centered.
- Two small feet under the bottom edge.

Only the materials change per amp:

| | Glass (clean) | Ember (crunch) | Monolith (high gain) |
|---|---|---|---|
| Body | Cream tolex `#e4e1d9` to `#c9c5ba`, fine dot texture | Oxblood tolex `#55212a` to `#3c1519`, coarse dot texture | Matte black `#1b1c1c` to `#121313`, 1px `#2a2d2c` edge |
| Panel | Brushed silver `#d3d6d4` to `#b4b8b6`, vertical brush lines | Near-black `#211918`, gold piping `#a88a52` double border | `#0d0e0e`, 1px `#262928` border |
| Grille | Dark diagonal weave `#2b2e2c` / `#222423` | Basketweave `#3d322b` / `#2d2520` | Horizontal slot vents `#0a0b0b` / `#1d1f1f` |
| Badge | Geist 300, 46px, wide tracking, light chrome | Fraunces italic 600, 52px, gold `#d9b878` | Geist 600, 24px, tracked, on a dark plate with emerald underline bar |
| Knob skin | Glass | Ember | Monolith |
| Knobs | Gain, Bass, Middle, Treble, Presence, Master | Gain, Bass, Middle, Treble, Presence, Master | Gain, Bass, Middle, Treble, Presence, Depth, Master |
| Toggles | Bright, Fat | Boost, Mid push | Tight, Boost |
| MIDI | PC 1 | PC 2 | PC 3 |

Textures: if the framework can't do these gradients and patterns cheaply, pre-render each body, panel, and grille as tiling PNGs (export at 2x) and draw them as images. Do not redraw procedural textures every frame.

Switching amps:
- Swaps the head materials, knob set, toggles, info row, preset name, and the chain block sublabel.
- Each amp keeps its own knob values. Switching away and back restores them.
- If "Follow amp choice" is on (cab page), the matching cab is selected too.
- MIDI program change 1/2/3 selects the amp exactly like clicking its tab.

### 4.7 Cab page

Three columns (220 / flexible / 240, 40px gaps):
- **Cabinet list**: three options with name and description, 1px left border that turns emerald when selected. Then a "Follow amp choice" toggle (on by default; manually picking a cab turns it off). Then a dashed IR drop zone with an emerald `Browse` link.
- **Speaker**: line-drawn speaker (outer ring, cone rings, dust cap, center crosshair) at 380px. Two draggable mic markers: A in emerald, B in ink, each an 11px ring with a 3px center dot and a letter label. Mic positions are normalized to the speaker radius and clamped inside 95% of it. Below: readout of each mic's zone (Cap under 0.20 of radius, Cap edge under 0.32, Cone under 0.80, else Cone edge).
- **Microphones panel**: per mic, a segmented type selector (Dynamic, Ribbon, Condenser) with emerald underline on the selected one, Distance (0 to 12 in) and Level (-24 to +6 dB) knobs, and a Flip phase toggle. At the bottom: Low cut (20 to 300 Hz) and High cut (3 to 20 kHz).

### 4.8 Tuner page

Opened from the top bar Tuner button. It is **not** a signal chain block; no chain block is active while it's open.

- Note name at 180px / weight 300, sharp sign and octave number smaller beside it.
- Readout row: frequency in Hz (2 decimals), cents (signed integer), state text (Flat, Sharp, In tune).
- Cents scale 720px wide, -50 to +50, ticks every 5 cents, longer ticks at ±25, longest at 0, labels at -50, -25, 0, +25, +50. A needle with a round head moves along it.
- In tune means within ±3 cents: note, needle, and state text turn emerald and a soft emerald zone lights up behind 0.
- String row: six 54px circles with note names and string numbers. Current string has an ink border; tuned strings turn emerald. Clicking a string targets it.
- Bottom controls: tuning presets (Standard, Drop D, Drop C, FACGCE), A4 reference with -/+ (430 to 450 Hz), Mute output toggle (on by default).
- Pitch detection runs on the audio side or a worker thread, never in the UI. Suggested algorithm: YIN or McLeod Pitch Method on the input signal. The UI just polls the detected frequency and confidence.

### 4.9 Pages not designed yet

Pre FX, EQ, Post FX, Input, and Output pages are not designed. Show a centered faint placeholder ("<Page> page is not mocked up yet") and keep the navigation working. Do not invent designs for them.

## 5. Interaction details that are easy to miss

- All controls are keyboard focusable with a 1px emerald focus outline offset by 3px.
- Respect reduced-motion settings if the framework exposes them (no meter animation; static tuner).
- Hover states: chrome text goes from dim to ink; borders go from `line-2` to `ink-faint`. No hover animations beyond ~150ms color transitions.
- The bypass dot click must not also trigger navigation.

## 6. The tone response curve is a placeholder

In the reference, the curve under the amp is a fake formula driven by the EQ knobs. **Do not ship that formula.** NAM models are not analytic EQs, so a computed curve would misrepresent the sound. Replace that area with one of:

1. **Real-time output spectrum analyzer (preferred)**: FFT on the output (2048 or 4096 points, Hann window), log frequency axis 20 Hz to 20 kHz, smoothing over time, drawn as a single 1.5px emerald line with a faint emerald fill fading to transparent. FFT runs off the UI thread; the UI draws the latest frame.
2. Measured response: run a sweep through the current chain offline (on a worker) when a knob changes and draw the result.

Keep the visual style from the reference: gridlines at 100 Hz, 1 kHz, 10 kHz in `line`, dashed 0 dB line, axis labels 11px faint.

## 7. Framework notes

- **egui**: build the knob, toggle, chain block, and amp head as custom widgets with `ui.allocate_response` plus `Painter`. Arcs need to be drawn as polylines. Load textures as images. Use `ctx.request_repaint` only when meters or tuner change.
- **iced**: use `Canvas` for knobs, the speaker, the tuner scale, and the analyzer.
- **vizia / nih-plug**: vizia supports a CSS-like stylesheet, so most tokens port directly; knobs and the speaker are custom views.
- **Slint**: tokens map to global properties; knobs as custom components with `Path`.
- **Tauri or any webview**: `ui_reference.html` can be ported almost directly, but the audio-to-UI bridge (IPC events for meters and tuner, commands for parameters) must stay off the audio thread.

## 8. Suggested build order

1. Tokens, fonts, window frame, top bar, bottom chain with page switching and bypass.
2. Knob and toggle widgets, wired to real parameters.
3. Amp head component plus the three material sets, amp switching, MIDI PC mapping.
4. Shared strip and meters.
5. Cab page.
6. Tuner page with real pitch detection.
7. Spectrum analyzer replacing the placeholder curve.

## 9. Acceptance checklist

- [ ] Side by side with each screenshot at 1x, layout, spacing, and colors match.
- [ ] Every knob supports drag, Shift fine drag, double-click reset, wheel, and arrow keys, and shows its value while dragging.
- [ ] Switching amps is click-free and restores each amp's own settings.
- [ ] Bypass dots toggle processing without changing the page.
- [ ] Cab follows the amp until a cab is picked manually.
- [ ] Tuner shows real pitch, goes emerald within ±3 cents, and mutes output when enabled.
- [ ] No locks, allocations, or UI work on the audio thread. Audio does not glitch while dragging knobs or switching pages.
