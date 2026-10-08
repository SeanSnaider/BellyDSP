# UI design

**The design source of truth is the UI handoff in `docs/ui/amp-ui-handoff/`**: `UI_HANDOFF.md` (the spec), `ui_reference.html` (the exact CSS and JS values; it wins over the spec), and `screenshots/` (2x renders at 1280 x 760). The earlier proposal that lived in this file (a blue accent, section colours, a two-row chain strip, a scenes bar along the bottom) is retired; ASSUMPTIONS U1 to U22 describe that first GUI, and the H entries (the "Phase 11: UI handoff" table) describe this one.

Everything is still drawn in code with JUCE's Graphics and one LookAndFeel: no artwork or trade dress from other products, and no real product or amp names in the UI (the heads' names, Glass, Ember, Monolith, Forge, Basalt, Comet, Quartz, and Lantern, are invented, and so are their looks).

## Where this build follows the handoff

- **Tokens.** Every colour, the four knob skins, the radii, and the layout sizes are in `src/ui/Theme.h`, named as in the reference's `:root` (`bg`, `surface`, `line` as `line1`, `line-2` as `line2`, `ink`, `ink-dim`, `ink-faint`, `accent`, `accent-dim`).
- **Type.** Geist 300 to 600 and Fraunces SemiBold Italic, bundled (UH10).
- **Window.** The 1280 x 760 canvas, scaled and letterboxed; radius 12 with a 1 px line border; top bar 56, main area padded 18 / 40 / 16, signal chain 72 (UH11).
- **Components.** The knob (one widget, four skins, 64 and 52 px), the toggle, the top bar, the chain blocks with their icons and bypass dots, the amp tabs with their minis, the amp head in nine materials (the slots' three, one for each of the five more built-in amps, and Custom for a user's capture; ASSUMPTIONS UH20 to UH25), the info row, the shared strip, the cab page, and the tuner page, at the reference's sizes. `tests/EditorTests.cpp` renders each designed page at 2x next to its screenshot in `build/proof/compare_<page>.png`.

## Where this build differs, deliberately

| What | How it differs | Entry |
|---|---|---|
| Undesigned pages | Pre FX, EQ, Post FX, Input, and Output keep their old contents, restyled; Pre FX and Post FX are tab rows | UH1, UH2 |
| Amp knobs | Mapped to the slots' trims and tone bands, 0 to 10 with 5.0 at 0 dB; seven on every head (Depth too) | UH3 |
| Amp toggles | None (the engine has none); the jewel alone, centred | UH4 |
| Colours | No warning colours; messages are words in the info row or a line at the bottom-left | UH5 |
| Undo, presets, A/B, scenes, tempo, CPU, CCs | Keys for undo; the name opens the browser; the rest on the Output page | UH6 |
| Shared strip | No Doubler | UH7 |
| Cab page | The real library, real mics (no type selector), packs-only movement, the room mic and alignment under the drop zone | UH8 |
| Tuner | Real readings, string targeting, no strobe | UH9, UH19 |
| Tone response | The real output spectrum, relative to its own average | UH13 |
| Knobs | Hover also shows the value | UH14 |
| Motion | No 150 ms colour fades; no reduced-motion setting to read | UH15 |
| Screenshots' text | The screenshots were rendered with a fallback font, so their text is wider than Geist's | UH10 |
| Brand | Reads "BellyDSP", the product's name, where the handoff has its placeholder "rig" (same dot, 15 px semibold; dot and name take 84 of its 136 px column); a click opens the version, licence, updates, source, and licences menu | DS9, DS38 |

## Implementation map

| Part | Code |
|---|---|
| Tokens, fonts | `src/ui/Theme.h`, `src/ui/Fonts.cpp` |
| Knob, toggle, fields, bypass dot, segmented choice | `src/ui/Controls.*` |
| LookAndFeel (buttons, menus, focus ring) | `src/ui/LookAndFeel.*` |
| Top bar, meters | `src/ui/TopBar.*`, `src/ui/Meters.*` |
| Signal chain | `src/ui/ChainNav.*` |
| Amp page, head art, spectrum | `src/ui/AmpView.*`, `src/ui/AmpHead.*` |
| Cab page | `src/ui/CabView.*` |
| Tuner page | `src/ui/TunerPage.*` |
| Pre FX, Post FX, EQ, Input, Output pages | `src/ui/MainPages.*`, `src/ui/SectionTabs.*`, `src/ui/Pages.*`, `src/ui/EqPage.cpp`, `src/ui/FxPages.cpp` |
| The window, navigation, menus, messages | `src/PluginEditor.*` |
