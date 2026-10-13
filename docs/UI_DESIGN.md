# UI design

**The source of truth is the redesign's direction contract**, `.impeccable/surfaces/src-plugineditor-cpp.md`, with its sketch `.impeccable/mocks/decision/app/canon.html` (the arrangement, palette, type, and density) and PRODUCT.md's "Operating Context" (how the app is used). Sean chose the direction on 2026-10-13: the category standard (the amp on a dark lit stage, the pedals as a board, a preset bar on top, an inspector on the right, scenes along the bottom), built to the craft of the best products in the category. Those products are an internal reference only: no other company's product or amp names appear anywhere.

The old UI handoff (`docs/ui/amp-ui-handoff/`) is retired as a source of truth; the comparisons against its screenshots are gone from the tests. ASSUMPTIONS U1 to U22 and UH1 to UH25 describe the GUIs before this one; RD1 onward describe this one.

Everything is still drawn in code with JUCE's Graphics and one LookAndFeel: no artwork or trade dress from other products. The amps' and pedals' names (Glass, Ember, Monolith, Lantern, Basalt, Comet, Forge, Quartz; the pedals carry their blocks' own names) are invented, and so are their looks.

## The main screen (phase 1, built)

One screen on the 1280 x 760 canvas, scaled uniformly to the window and letterboxed:

| Part | Where | What |
|---|---|---|
| Top bar | across the top, 52 | The brand (its menu: version, licence, updates, source, Match tone); A and B; undo and redo; the previous arrow, the preset's name (23 px semibold) over its tag and current scene ("Factory  Scene 2: Lead", the scene 15 px ink; RD4-4) with the preset's notes as its tooltip (RD4-6), the next arrow, and Save; the CPU; the In and Out meters; Tuner (with the note it hears), Match tone, and Settings, the open one underlined in orange |
| Stage | left 980, 350 high | A dark room lit from below. Its segmented control (top right) picks Amp, Cab, or EQ, crossfading 0.18 s. Amp: the shelf of mini heads (pick the amp), the playing head at about 0.86 of its designed size, rendered gear (tolex, woven cloth, skirted knobs with a printed scale and no value arcs, a jewel lamp; RD4-8) standing in a pool of light, and one short line under it ("High gain · Monolith", the capture's file in its tooltip; the rate or a message at the right; RD4-5). Cab: the library list, Follow, the drop zone; the speaker in its baffle with mics A and B on it, large (RD34); the match curve, the room mic, auto-align; its short line ("Modern 4x12 · Dynamic, bright 60 W, var. 3"). EQ: the post or pre EQ's graph over its live spectrum, with draggable handles |
| Board | under the stage, 280 high | Three tabs (Sean's call, 2026-10-13; RD5-1 to RD5-9): Pre FX, Amp & Cab, and Post FX on a strip at the top in the stage's pill grammar, each its name and a row of tiny LEDs, one per block in it, lit while that block is on and at 45% while its section is off, so the closed tabs still show what's on. A click anywhere on a tab opens it. Under the strip, the open tab's one row of gear (`ui::isPedal` pedals drawn with `drawPedal`), scaled to fit the row: Pre FX's pedals in their saved order (1.18 of their size), Amp & Cab's amp marker (a steel plate with the playing amp's head in small, its name, its switch as a jewel), Gate B, and cab marker (the cab's front) as larger tiles centred on the cable, and Post FX's pedals in their saved order (0.83). The open section's switch (a labelled toggle, ON or OFF: RD4-3) sits at the strip's right as "Pre FX section" or "Post FX section". The selection opens its tab wherever it changes; the open tab is remembered (`uiBoardTab`), Pre FX the first time. A click on a footswitch switches the block (one undo step); elsewhere it selects the pedal (drawn raised and ringed). Drag a pedal along its row to reorder its section. A right-click is MIDI learn for its switch. LEDs show each block's own switch, live (1.5 times the first art's: near-black off, haloed on; RD4-2), and bloom for a moment when one comes on; a pedal that's off is dimmed whole (55% brightness, less saturation), and so is every pedal in a section that's off (RD4-1). Gate B isn't in either section, so only its own switch dims it (RD5-8) |
| Inspector | right 300, top bar to footer | The selected block's every control in one scrolling column, designed per block (phase 2, below): the header with the block's gear in small, its meter, its mode as a selector, its main controls as large knobs, then the rest in sections. A generic control still covers any parameter under a pedal's prefix its page doesn't |
| Footer | across the bottom, 78 | The eight scenes (number and name; the current one lit orange, the light gliding to a new scene in 0.16 s; empty ones a faint outline), Store, and the tuner's reading (the note, the cents, Flat / Sharp / In tune) |

Overlays (the tuner, Match tone, Settings) open over the stage, the board, and the inspector, leaving the top bar and the footer, and close back to the main screen as it was (phase 3, below).

Keys: Left and Right step the presets, 1 to 8 recall the stored scenes, [ and ] step the board's tabs (wrapping round; RD5-4), T opens and closes the tuner, Escape closes an overlay, Cmd-Z and Shift-Cmd-Z undo and redo; while Match tone is open, its own keys (Space, and 1 to 3, or 1 to 4 with the raw song) come first. A knob or a segmented control takes Up and Down when it has the focus, and Left and Right only when the keyboard gave it the focus (Tab), so the arrows keep stepping presets after a click on a knob. Tooltips name the keys.

## Every block's controls and the cab (phase 2, built)

Each block's inspector is designed, not reflowed (ASSUMPTIONS RD23 to RD36). One grammar for all of them, top to bottom:

1. **The header:** the block's gear in small on a plinth (its pedal with its LED live; the amp's head in its material; the cab's front), its name in 17 px semibold, where it sits in 12 px faint, its switch at the top right, a hairline.
2. **What it's doing**, when the block has a meter: the gates' (state, level against the thresholds, reduction; drag it to set the threshold), the compressors' (the curve, the reduction now and its last three seconds), the harmonizer's (the note it hears and each voice's).
3. **Its mode** as a selector across the column (`ChoiceBar`: segments, the chosen one lighter with its name in orange).
4. **Its main controls** as large knobs (80 px, the value always shown under the caption), two to a row.
5. **The rest** in sections under small tracked capitals with a hairline (a section's switch at its heading's right), knobs three to a row with their values under their captions in 11 px (RD4-7), selectors and fields on rows of their own, tables for voices and bands.

| Block | First | Large | Then |
|---|---|---|---|
| Gate A, Gate B | the meter; Learn (A) or the link pad (B) | Threshold, Release | release mode (Adaptive, Fixed); Response: attack, hold, range; Detector (its high-pass on the heading): the DI or its own input, hysteresis, the high-pass frequency |
| Compressors | Studio or Pedal; the meter | Threshold, Ratio | Response (auto release): attack, release, knee; Output (auto makeup): makeup, mix; Detector (high-pass): Peak or RMS, frequency |
| Boost | Clean, Tight, Screamer; what the mode does | Level, Tilt | Tight mode: tight, mid push (dimmed unless the mode uses them) |
| Overdrive | its four circuits, two by two; what the circuit is | Drive, Level | Shape (Tight on the heading): tone, mix, tight; Oversampling: 4x or 8x |
| EQs | Graphic or Parametric | | the nine faders side by side, or the five bands as a table (number, type, frequency, gain, Q); the cuts, a row each (switch, frequency, slope) |
| Harmonizer | what it hears | | the key (twelve buttons), the scale and its custom notes; the voices, two lines each; out of key, the lowest note, glide, level |
| Multivoicer | engine, mix mode | Mix, Spread | the shapes and starting points; Ensemble (wet high-pass): voices, its frequency; the eight voices, a row each |
| Bloom | the order chips (each with its effect's LED) | Mix | the three effects in order, each its switch on its heading, its choices as selectors, its knobs |
| Chorus | Classic, Dimension, Tri | Rate (or its note), Depth | sync; Blend: mix, width, shape; Low end (protect): below; Character: analog, noise |
| Delay | Digital, Analog, Tape | Time (or its note and its ms) beside Tap, the tempo, Sync; Feedback, Mix | Stereo: the layout, the right time or offset, ducking; the repeats' cuts; modulation |
| Reverb | Room, Hall, Plate | Mix, Decay | Freeze (a pad); Space (sync pre-delay): size, pre-delay, early/late, diffusion, width; damping; cuts; motion; shimmer and its interval (dimmed on Plate) |
| Amp | | Gain, Master | Tone: the five; input and output; the output spectrum. Only the amp: Gate A is its own pedal (RD4-9) |
| Cab | | | Mic A and Mic B (Mute on each heading): the file, distance, level, pan, delay, channel, phase; the cuts |

Selecting an EQ pedal puts the stage on its graph, and the next other pedal puts it back (RD23). The cab's stage is built around the speaker: the library at the left, the match curve, room mic, and auto-align at the right, and in the middle the speaker in its baffle with the close mics on it, large, dragged to move when they hold a pack (RD34). The board's amp and cab markers are gear: a steel plate with the amp's head in small or the cab's front, the name, and the switch as a pilot jewel (RD35).

## The overlays (phase 3, built)

Each overlay is a page in the 1200 x 598 the frame gives it, under one header (`ui::overlay::paintHeader`: the title, a line about it, its keys as keycaps, Close), made of panels on the ground (the panel colour, a hairline, radius 10) with small uppercase labels. ASSUMPTIONS RD3-1 to RD3-23.

| Overlay | What |
|---|---|
| Tuner | Built to be read from a step back. The display: the note at 200 px (orange within 3 cents) with its sharp and octave, a chevron each side (the left lights when flat, the right when sharp, both orange in tune), the frequency, the cents, and the state, and a 960 px scale of 41 segments (2.5 cents apart; the lit one follows the cents, its neighbours lit in proportion; in tune the middle three light orange with a glow behind the note). Under it, the tuning's six strings as chips (click one to tune to it, again to let go; a string that's been in tune is checked) and the controls (the tunings, A4 with - and +, Mute output). The footer's reading shows the note and a small cents meter while the tuner is open |
| Match tone | The flow as six numbered steps, two rows of three, read left to right then down: 1 The song (the file, the waveform and its section, Play, the song's level, the count-in for Play, separation), 2 Your DI (Record or a file, what's recorded and Save take, the count-in and latency), 3 Match (what you played, Clean up with my take, Match and Cancel, the progress, the stage or the error in full), 4 Result (the closeness and what it measured, the settings it chose, the curve), 5 Compare (Target, Match, Current, Raw, level-matched, the loop, the live mute; Space and 1 to 4), 6 Apply (what it sets, in one undo step; Apply, Discard). Each step's badge says where it stands (waiting, next, running, done); every control stays usable. The main actions (Record, Match, Apply) are primary buttons, filled but not orange |
| Settings | A section list at the left and the chosen section's panels: Input (the level, the calibration, the routing); Output and MIDI (the level and limiter, A/B with Copy, the tempo with Tap, the CPU meter and saver, the footswitch's controllers and the MIDI mappings, the keys); Audio device (48 kHz only as one line of status, with what the device is at; its name, driver, buffer, and latencies in the standalone app, the card folded to one line where there's no device; and the setup a player needs: RD4-10); About (the version, the licences with every notice a click away, updates, the source of this version) |

Menus are rounded raised panels with keycap shortcuts and small uppercase section labels; dialogs (the scene Rename box) are rounded panels with their main button primary.

## The finish review's fixes (built)

A fresh reviewer judged the three phases against the contract and returned seven material fixes (ASSUMPTIONS RD4-1 to RD4-11). The pedal enclosures and the scenes footer are as they were.

| Fix | What changed |
|---|---|
| On and off from 2 m | A pedal that's off is dimmed whole (55% brightness, 45% saturation, from a dimmed copy of its cached art); a section that's off dims its pedals the same way, their LEDs still telling their own switches. The LED is 1.5 times larger: near-black #2a2a2a off with no tint, a 24 pt halo on. PRE and POST are labelled toggles |
| Preset and scene from a step back | The name at 23 px semibold, the scene at 15 px medium ink, in a 48 px box |
| One short line | "High gain · Monolith" under the amp and "Modern 4x12 · Dynamic, bright 60 W, var. 3" under the cab; the capture's file in the line's tooltip, a preset's notes in the preset box's tooltip, never cut off on the stage |
| The head as gear | Tolex grain and rolled edges on every shell, woven cloth on every grille, skirted knobs with a pointer line and a printed scale and no value arcs (the values show on hover and drag), a jewel lamp with the dead faceplate before Gain closed up, one accent (Monolith's and Comet's pointers ink, Forge's the accent), a pool of light on the floor under the head |
| Values under every knob | Every inspector knob shows its value under its caption, in 11 px |
| The amp's inspector is the amp's | Gate A's section and its light are gone from it; Gate A is its own pedal |
| Audio device | A one-line status in place of the label over a giant "48 kHz"; "This device" folds to a line (its button hidden) where there's no device |

Proof: `--only "Main screen"` (main_amp, main_pedal_post_harmonizer with the harmonizer off, stage_cab, board), `--only "Overlays"` (overlay_settings_device, _device_44k), `--only "Editor parts"` (`build/proof/amp_heads_all.png`, every head at 2x as the stage dresses it, and `editor_amp_<material>.png`), `--only "Pedal art"` (`build/proof/pedals/`).

## Tokens

Every colour, type style, size, and radius is in `src/ui/Theme.h`; components hold no hex values of their own (the amp heads' and pedals' materials are their art's, in `AmpHead.cpp` and `PedalArt.cpp`).

| Token | Value | Use |
|---|---|---|
| `bg` | #121212 | the ground, the stage's dark, the overlays |
| `panelBg` | #1a1a1a | the top bar, the inspector, the footer |
| `raised` | #1f1f1f | buttons, fields, knob bodies, the stored scenes, the markers |
| `line1` | #2c2c2c | hairlines |
| `line2` | #3a3a3a | stronger borders, inactive knob tracks, off states |
| `ink`, `inkDim`, `inkFaint` | #e8e8e8, #9a9a9a, #6b6b6b | text |
| `accent` | #ff8a00 | on, selected, active: the LEDs, the inspector's knob arcs, the lit scene, the selected pedal's ring, the segmented control's choice, the lit jewel lamp. The only accent: no knob pointer is teal or emerald (RD4-8) |
| `clip` | #e5484d | a latched clip on the meters, and nothing else |
| `stageLight`, `boardTray`, `letterbox` | #34302b, #161616, #0a0a0a | the stage's light from below, the board, around the canvas |

Type: Geist 300 to 600 and Fraunces SemiBold Italic, bundled. Motion: `stageFadeSeconds` 0.18, `sceneGlideSeconds` 0.16, `ledGlowSeconds` 0.35, all on 60 Hz timers on the message thread.

The app icon is the "B." mark in the same tokens: an `ink` B with an `accent` dot on `bg`, drawn by `tools/content/make_icon.py` into `resources/icon/BellyDSP_icon_1024.png` (the dot was emerald until Sean had it redrawn on 2026-10-13). JUCE makes the .icns and .ico from it when CMake configures, so a new icon needs `cmake -B build` before the build picks it up.

## Implementation map

| Part | Code |
|---|---|
| Tokens, fonts | `src/ui/Theme.h`, `src/ui/Fonts.cpp` |
| Knob, toggle, fields, bypass dot, segmented control (and its pill style) | `src/ui/Controls.*` |
| LookAndFeel (buttons and primary buttons, menus, dialogs, focus ring) | `src/ui/LookAndFeel.*` |
| The overlays' header, panels, step badges, keycaps, notes | `src/ui/Overlay.*` |
| Top bar, meters | `src/ui/TopBar.*`, `src/ui/Meters.*` |
| The stage, the inspector, the overlay frame | `src/ui/MainScreen.*` |
| The board | `src/ui/Board.*` (the pedal art: `src/ui/PedalArt.*`) |
| The amp on the stage and in the inspector, the head art, the spectrum | `src/ui/AmpView.*`, `src/ui/AmpHead.*` |
| The cab on the stage and in the inspector | `src/ui/CabView.*` |
| The pedals' inspectors and their column layout (`InspectorLayout`, `BlockPage::paintHeader`), the EQ's stage view | `src/ui/Pages.*`, `src/ui/FxPages.cpp`, `src/ui/EqPage.cpp` |
| The selector (`ChoiceBar`), the pad switch, the large knob | `src/ui/Controls.*` |
| The meters (gate, compressor), the Tap pad, the harmonizer's display, the cab's front, the inspector's headings | `src/ui/InspectorParts.*` |
| Scenes and the tuner's reading in the footer | `src/ui/ScenesBar.*` |
| Settings and its sections (Input, Output and MIDI, Audio device, About) | `src/ui/MainPages.*` |
| The tuner, Match tone | `src/ui/TunerPage.*`, `src/ui/ToneMatchPage.*` |
| The window, selection, overlays, keys, menus, messages | `src/PluginEditor.*` |

Proof: `ampsim_tests --proof-dir build/proof --only "Main screen"` writes `build/proof/redesign/` (the main screen with the amp, with each of the board's tabs open (`main_board_pre.png`, `main_board_ampcab.png`, `main_board_post.png`), a pre and a post pedal, the harmonizer, the Cab and EQ stage modes, each overlay, a small window, a letterboxed one, the board alone), and with "Main screen inspectors": `inspector_<block>.png` for every block, `stage_cab_pack.png`, `board_markers.png`, `board_markers_cab_off.png`, and the meters (`gate_meter_open.png`, `gate_meter_closed.png`, `gate_meter_linked.png`, `compressor_meter.png`, `harmonizer_hearing.png`). `--only "Overlays"` adds the tuner (`overlay_tuner_<state>.png` and its footer crops), Settings' four sections (`overlay_settings_<section>.png`, and the device at 44.1 kHz), a menu, and the Rename dialog; the tone match tests add every state of Match tone (`overlay_tone_match_<state>.png`).

## What's next

- **Phase 2's finish:** built (above); still owed is the finish review of the main screen against the contract (the contract's FINISH line: the review, its verdict, DESIGN.md and its sidecar, every shipping raster's provenance), and Sean's listening and playing check RD.2 (PROGRESS).
- **Phase 3, the rest of the app:** the tuner, Match tone, Settings, the menus, and the dialogs are built (above). Left: the About window's own look (it keeps the OS's title bar and a plain scrolling text). The website follows the app since 2026-10-13 (its images come from these snapshots and the Editor parts tests' `build/proof/site/` heads; ASSUMPTIONS WEB20 to WEB23).
