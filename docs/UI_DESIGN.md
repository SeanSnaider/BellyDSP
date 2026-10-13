# UI design

**The source of truth is the redesign's direction contract**, `.impeccable/surfaces/src-plugineditor-cpp.md`, with its sketch `.impeccable/mocks/decision/app/canon.html` (the arrangement, palette, type, and density) and PRODUCT.md's "Operating Context" (how the app is used). Sean chose the direction on 2026-10-13: the category standard (the amp on a dark lit stage, the pedals as a board, a preset bar on top, an inspector on the right, scenes along the bottom), built to the craft of the best products in the category. Those products are an internal reference only: no other company's product or amp names appear anywhere.

The old UI handoff (`docs/ui/amp-ui-handoff/`) is retired as a source of truth; the comparisons against its screenshots are gone from the tests. ASSUMPTIONS U1 to U22 and UH1 to UH25 describe the GUIs before this one; RD1 onward describe this one.

Everything is still drawn in code with JUCE's Graphics and one LookAndFeel: no artwork or trade dress from other products. The amps' and pedals' names (Glass, Ember, Monolith, Lantern, Basalt, Comet, Forge, Quartz; the pedals carry their blocks' own names) are invented, and so are their looks.

## The main screen (phase 1, built)

One screen on the 1280 x 760 canvas, scaled uniformly to the window and letterboxed:

| Part | Where | What |
|---|---|---|
| Top bar | across the top, 52 | The brand (its menu: version, licence, updates, source, Match tone); A and B; undo and redo; the previous arrow, the preset's name over its tag and current scene ("Factory  Scene 2: Lead"), the next arrow, and Save; the CPU; the In and Out meters; Tuner (with the note it hears), Match tone, and Settings, the open one underlined in orange |
| Stage | left 980, 350 high | A dark room lit from below. Its segmented control (top right) picks Amp, Cab, or EQ, crossfading 0.18 s. Amp: the shelf of mini heads (pick the amp), the playing head at about 0.86 of its designed size with its knobs in its own skin, and the info row (the capture's voice, its file, the rate or a message). Cab: the library list, Follow, the drop zone; the speaker with mics A and B; the match curve, the room mic, auto-align. EQ: the post or pre EQ's graph over its live spectrum, with draggable handles |
| Board | under the stage, 280 high | Every pedal (`ui::isPedal`) drawn with `drawPedal` in chain order, at about two thirds of its size, on two rows. Top: PRE and its section switch, the pre section in its saved order, the amp's marker (its mini head and name, its switch), Gate B, the cab's marker. Bottom: POST and its section switch, the post section in its saved order. A click on a footswitch switches the block (one undo step); elsewhere it selects the pedal (drawn raised and ringed). Drag a pedal along its row to reorder its section. A right-click is MIDI learn for its switch. LEDs show each block's own switch, live, and bloom for a moment when one comes on; a section that's off dims its row |
| Inspector | right 300, top bar to footer | The selected block's every control in one scrolling column: a header (its name, where it sits, its switch), then sections under small headings, knobs three to a row, choices on rows of their own with their names, switches flowing, fields under their names. The amp (the default): Gain and Master, the five tone knobs, Gate A's switch with Threshold and Release and its open light, the input and output levels, the output spectrum. The cab: its switch, close mics A and B, the cuts. Every pedal: its page reflowed for the column, and a generic control for any parameter under its prefix the page doesn't cover |
| Footer | across the bottom, 78 | The eight scenes (number and name; the current one lit orange, the light gliding to a new scene in 0.16 s; empty ones a faint outline), Store, and the tuner's reading (the note, the cents, Flat / Sharp / In tune) |

Overlays (the tuner, Match tone, Settings) open over the stage, the board, and the inspector, leaving the top bar and the footer, and close back to the main screen as it was. The tuner has a header with Close; Match tone and Settings bring their own. Settings holds the Input page and the Output page (levels, the limiter, A/B with Copy, the tempo with Tap, the CPU meter and CPU saver, the footswitch's controllers and the MIDI mappings) under a segmented control.

Keys: Left and Right step the presets, 1 to 8 recall the stored scenes, T opens and closes the tuner, Escape closes an overlay, Cmd-Z and Shift-Cmd-Z undo and redo; while Match tone is open, its own keys (Space, 1 to 3) come first. A knob or a segmented control takes Up and Down when it has the focus, and Left and Right only when the keyboard gave it the focus (Tab), so the arrows keep stepping presets after a click on a knob. Tooltips name the keys.

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
| `accent` | #ff8a00 | on, selected, active: the LEDs, the knob arcs, the lit scene, the selected pedal's ring, the segmented control's choice |
| `clip` | #e5484d | a latched clip on the meters, and nothing else |
| `stageLight`, `boardTray`, `letterbox` | #34302b, #161616, #0a0a0a | the stage's light from below, the board, around the canvas |

Type: Geist 300 to 600 and Fraunces SemiBold Italic, bundled. Motion: `stageFadeSeconds` 0.18, `sceneGlideSeconds` 0.16, `ledGlowSeconds` 0.35, all on 60 Hz timers on the message thread.

## Implementation map

| Part | Code |
|---|---|
| Tokens, fonts | `src/ui/Theme.h`, `src/ui/Fonts.cpp` |
| Knob, toggle, fields, bypass dot, segmented control (and its pill style) | `src/ui/Controls.*` |
| LookAndFeel (buttons, menus, focus ring) | `src/ui/LookAndFeel.*` |
| Top bar, meters | `src/ui/TopBar.*`, `src/ui/Meters.*` |
| The stage, the inspector, the overlay frame | `src/ui/MainScreen.*` |
| The board | `src/ui/Board.*` (the pedal art: `src/ui/PedalArt.*`) |
| The amp on the stage and in the inspector, the head art, the spectrum | `src/ui/AmpView.*`, `src/ui/AmpHead.*` |
| The cab on the stage and in the inspector | `src/ui/CabView.*` |
| The pedals' inspectors and their column layout (`InspectorLayout`), the EQ's stage view | `src/ui/Pages.*`, `src/ui/FxPages.cpp`, `src/ui/EqPage.cpp` |
| Scenes and the tuner's reading in the footer | `src/ui/ScenesBar.*` |
| Settings, the Input and Output pages | `src/ui/MainPages.*` |
| The tuner, Match tone | `src/ui/TunerPage.*`, `src/ui/ToneMatchPage.*` |
| The window, selection, overlays, keys, menus, messages | `src/PluginEditor.*` |

Proof: `ampsim_tests --proof-dir build/proof --only "Main screen"` writes `build/proof/redesign/` (the main screen with the amp, a pre and a post pedal, the harmonizer, the Cab and EQ stage modes, each overlay, a small window, a letterboxed one, the board alone).

## What's next

- **Phase 2, every block's controls and the cab:** each block's inspector designed rather than reflowed (the voice tables of the harmonizer and the multivoicer, the gate's and the compressors' meters, the EQ's bands), the cab's stage view redesigned around the speaker, and a finish review of the main screen against the contract (the contract's FINISH line: the review, its verdict, DESIGN.md and its sidecar, every shipping raster's provenance).
- **Phase 3, the rest of the app:** the tuner, Match tone, and Settings redesigned in the new world (they are reused and restyled in phase 1), the About window, the dialogs and menus, and the website re-skinned to follow the app (its images come from these snapshots; `tools/site/make_site_assets.py`'s crops still point at the old layout).
