---
version: 1
slug: "src-plugineditor-cpp"
primary_target: "src/PluginEditor.cpp"
related_targets: ["src/ui"]
---

# BellyDSP app: the redesign

Scope: the whole desktop app (JUCE, every pixel drawn in code), starting with the main playing screen, then every block's controls, the cab, the tuner, the Match tone page, and settings. Visitor mode: Operate.

Audience and job: a guitarist playing through it about 90% of the time and dialing tones about 10%, using the mouse or trackpad and the keyboard. While playing they must see the preset and scene and switch them, see what's on at a glance, and reach a few key knobs, with nothing they need hidden.

Constraints: real-time rules are untouched (the UI never blocks or allocates on the audio thread); parameter IDs are permanent; no other company's product or amp names anywhere; the app's existing amp-head art (nine materials) is reusable gear; Neural DSP's plugins are the internal quality bar only.

Memorable moment: the rig laid out like a real one, the amp lit on its stage and the board of pedals under it, every LED telling the truth.

Unresolved: whether the Input and Output pages become an overlay or settings panel; the website re-skin follows once this lands.

## Direction contract

THESIS: The category standard, built to its best products' craft: one amp per screen on a lit stage, the pedals as a board in chain order, the selected block's controls in an inspector, presets and scenes always in reach. It refuses the current page-per-section navigation that hides the rig behind tabs and pages.

OWN-WORLD: A dark studio ground (#121212), panels #1a1a1a, raised #1f1f1f, hairlines #2c2c2c, ink #e8e8e8 and #9a9a9a, one orange accent (#ff8a00) for on, selected and active, red only for clipping. Rendered gear drawn in code: the nine amp heads and a new family of pedal enclosures, each block its own enclosure colour, LED, footswitch, knobs and silkscreen. The stage is lit from below. Geist for the interface.

STORY: From a step back the player reads the preset and scene and sees which pedals are lit; switches scenes with 1 to 8 or a click and presets with the arrows; turns the amp's Gain and Master or the selected pedal's knobs without leaving the screen; goes deeper in the inspector when dialing; tunes from the footer.

FIRST VIEWPORT: Top bar: the brand, the preset name between previous and next with its scene, A/B, undo, CPU, and the tuner. Left and centre: the stage with the playing amp head at large scale and the amp picker; under it the board, every pre and post block as a pedal in chain order around an amp marker, each with its LED and footswitch, the selected one raised. Right: the inspector, 300 wide, showing the selected block's every control (the amp's seven knobs by default). Bottom: the scenes 1 to 8, the current one lit orange, and the tuner reading. Motion: an LED lights with a short glow, a scene change glides the lit button, picking an amp or the cab crossfades the stage.

FORM: The category standard (canon), chosen by Sean on 2026-10-13 from a roll that assigned The marquee; seed key 68d1a405; quality bar Neural DSP's plugins.

FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and every shipping raster carrying its provenance
