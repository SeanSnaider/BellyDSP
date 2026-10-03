# UI design

The design system for the real GUI (BUILD_PLAN "GUI", Phase 11). Sean directs the visual design; this first version was written without his input, as a proposal, during the run-to-completion build (ASSUMPTIONS U1 to U8 list every choice). Everything is drawn in code with JUCE's Graphics and a custom LookAndFeel: no artwork, photos, or trade dress from other products, and no product or amp names in the UI.

## Character

A dark, calm instrument panel that gets out of the way of playing: flat surfaces with soft depth, one accent colour for "on" and "selected", and text that's readable from a step back (the laptop sits on a stand while Sean plays). Dense where it must be (the block editors), sparse everywhere else. Motion only where it carries information (meters, the tuner, a reorder).

## Colour

Tokens, in code as named constants in one header (`src/ui/Theme.h`), never hex values scattered in components.

| Token | Value | Use |
|---|---|---|
| `background` | #121417 | Window |
| `surface` | #1B1E23 | Panels, the chain strip |
| `surfaceRaised` | #242830 | Cards, knobs' faces, hovered blocks |
| `outline` | #2F343D | Panel and card borders, separators |
| `text` | #E8EAED | Primary text and values |
| `textDim` | #9AA1AC | Labels, units, captions |
| `accent` | #3FA7D6 | On, selected, knob arcs, focus |
| `accentSoft` | accent at 35% | Fills behind the accent (selected block, active scene) |
| `good` | #4CD98A | In tune, signal present, meters below -12 dBFS |
| `warn` | #FFC04D | Meters -12 to -3 dBFS, warnings, the close threshold |
| `error` | #FF6B5E | Clipping, errors, way out of tune |
| `section` colours | pre #7A8CFF, amp #FF9F43, cab #B57CFF, post #3FD0C9 | A thin bar on each chain block: which section it lives in |

Contrast: `text` on `surface` is 13:1, `textDim` on `surface` 6.4:1 (both above WCAG AA). States are never shown by colour alone: an off block is also dimmed and its switch drawn hollow.

## Typography

The system UI font (San Francisco on macOS) until Sean picks one to embed (U2). Sizes, in points at UI scale 1:

| Style | Size | Weight | Use |
|---|---|---|---|
| `title` | 18 | Semibold | Panel titles, the preset name |
| `body` | 14 | Regular | Buttons, menus, combo boxes |
| `value` | 13 | Medium, tabular figures | Knob values, meters' numbers |
| `label` | 12 | Regular | Knob captions, column headings |
| `caption` | 11 | Regular | Chain blocks' state lines, axis labels, legends, card headings (U10) |
| `huge` | 96 | Bold | The tuner's note |

## Spacing and shape

A 4-point grid: spacing tokens 4, 8, 12, 16, 24, 32. Corner radius 6 for panels and cards, 4 for buttons and fields, round for knobs and switches. Panel padding 16; gaps between controls 8; between groups 16.

## Components

- **Knob.** A 270-degree arc track (`outline`, 3 pt), the value arc in `accent` from the parameter's zero point (the centre for bipolar ranges, the start otherwise), a `surfaceRaised` face with a short pointer, the caption above in `label`, the value below in `value`. Drag up/down (shift for fine, 10x), scroll wheel, double-click to type a value, alt-click to reset to the default, right-click for the MIDI and scenes menu. Sizes: 56 (normal) and 40 (compact).
- **Switch.** A pill toggle (`accent` when on, `outline` hollow when off) with its label to the right. Right-click: the MIDI and scenes menu.
- **Combo box.** A `surfaceRaised` field with a chevron; menus in `surface` with `accent` for the ticked item.
- **Meter.** Vertical bars with a 1.5 s peak hold: `good` up to -12 dBFS, `warn` to -3, `error` above; a clip light that latches until clicked. Input and output meters in the top bar; a gain reduction meter (a bar growing down from the top) on the compressors and gates.
- **Chain block.** A card sharing its row's width, up to 140 x 48 (U9): the section bar on top, the block's name, a power switch, and a tiny state line (the mode, or "off"). Selected: `accentSoft` fill and an `accent` outline. Off: 45% opacity. Drag within its section to reorder; the gap opens where it will land; dropping triggers the same click-free reorder as the order strip.
- **Card.** A group of controls inside a page: `surfaceRaised` at 42% over the panel, the `outline` border, radius 6, and a small uppercase heading inside at the top. Cards in a row share its width in proportion to what they hold.
- **Scene button.** A numbered tile (1 to 8) with the scene's name under it; stored tiles bright, empty ones faint, the active one filled with `accentSoft`.

## Layout

The window opens at 1280 x 820 and resizes from 1100 x 720 up, with a UI scale setting (75, 100, 125, 150%) that scales everything.

- **Top bar (56).** The preset name with its browser (a menu of folders, factory presets, and "Save as..."), Save, A/B with Copy, Undo, Redo, the Tuner button, the tempo field with Tap, input and output meters, and a CPU meter (the audio callback's time against its deadline).
- **Chain strip (156, two rows; U9).** Every block in signal order: input, the pre section (reorderable), the amp, Gate B, and the cab on the first row; a line turns down into the post section (reorderable) and the output on the second. Sections are labelled boxes; the amp and cab are fixed cards. Clicking a block selects it; its switch bypasses it.
- **Editor (the rest).** The selected block's editor, built from the components above: the amp page with three slot cards (capture, status, trims and tone), the cab page with the speaker's face and the close mics on the pack's map (each mic's IR or pack, level, pan, delay; U17), the EQ page with the response curve over a live analyzer (a 4096-point FFT at 30 fps; U11 says what each EQ's analyzer reads) with draggable band handles, and the remaining blocks' controls in cards.
- **Warning line (30).** The sample rate, MIDI learn waiting for a controller, a preset's notes or problems; a hint when there's nothing to say.
- **Scenes bar (48).** Eight scene tiles and Store, at the bottom where a foot-controller's layout would be.
- **Tuner.** A full-window overlay (the existing needle and strobe views), dismissed by its own button or the footswitch.

## Interaction rules

- Every control is attached to its parameter; nothing changes sound outside the parameters, the orders, and file loads.
- Every press starts an undo step (the parameter tree's UndoManager).
- Right-click on any control: MIDI learn, its mappings, and "Held by scenes".
- Repaint only on change: meters and the analyzer at 30 fps while they move, nothing otherwise.
- The audio thread never waits on the GUI: meters and the analyzer read atomics and a lock-free ring.

## Proof

Every page is snapshotted by the test suite (`build/proof/editor_<page>.png`) at UI scale 1, at 1.5 (`_150`), and in the smallest window, 1100 x 720 (`_min`), plus the chain strip alone, the EQ with its analyzer showing a signal, and the tuner; the editor tests keep exercising MIDI learn, scenes, the tuner overlay, and A/B through the new components.
