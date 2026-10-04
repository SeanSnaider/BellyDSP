# Bundled content

Captures and impulse responses that ship inside BellyDSP, so anyone has something to play the moment
the app opens.

- `irs/Vintage 4x12/` (6 IRs) and `irs/Modern 4x12/` (15 IRs): the "Emerald" and "Brutal" cab IR packs by
  Bastian Karschewski (Jester Dyne Productions), which he released under CC0 (public domain). Renamed
  without brand names and the modern ones cut to 1 s by `tools/content/import_jester_irs.py`; each file's
  manifest entry cites its original by the pack handbook's patch number. The cab page lists them under "Built in", one entry per IR.
- `models/`: the three built-in amps, one per amp slot and named after its head, each a **gain set**: five
  captures of one amp across its gain knob, and a `gainset.json` listing them (`src/dsp/GainSet.h`), which
  is what the app loads and the presets refer to. `Glass/` (clean to the edge of breakup), `Ember/` (light
  to heavy crunch), `Monolith/` (tight high gain to a saturated lead), each holding `<Amp>, gain 0.nam`,
  `2.5`, `5`, `7.5`, and `10`. The amp head's Gain moves across the steps (BUILD_PLAN "Amp gain"). A fresh
  start loads them, the factory presets use them, and an amp's right-click menu puts one back. For now
  they're stand-ins, trained by `tools/content/make_default_captures.py` from the project's own gray-box
  amp (`prototypes/amp_sim.py`, amp only, each channel at five positions of its gain knob; settings in each
  manifest entry), until Sean's own captures replace them under the same names (`docs/CAPTURING.md`,
  "Replacing a built-in capture"). They replaced the single `Glass.nam`, `Ember.nam`, and `Monolith.nam`
  on 2026-10-04; presets and saved states that name those load the sets. Licensed under **CC BY 4.0** (Creative
  Commons Attribution 4.0 International, `licenses/CC-BY-4.0.txt`, the official legal code from
  creativecommons.org) with the attribution "Sean Snaider": anyone may share and adapt them, including
  commercially, as long as they credit him.

## Licences

The app's code is AGPL-3.0-or-later (`LICENSE` at the repo root). The files in this folder are not code
and each carries its own licence, listed per file in `manifest.json`: CC0 1.0 for the Jester Dyne IRs
(`licenses/CC0-1.0.txt`) and CC BY 4.0 for the captures (`licenses/CC-BY-4.0.txt`).

## How it works

- Everything in this folder except this README is copied into the app when it's built: into
  `BellyDSP.app/Contents/Resources/content/` on macOS and into `content\` next to `BellyDSP.exe` on
  Windows (the installer installs it there).
- Put captures under `models/` and IRs or cab packs under `irs/`, the same layout as the user library.
- A preset refers to a bundled file as `factory:models/<file>.nam` or `factory:irs/<file>.wav`. The app
  resolves that against its own content folder, wherever it's installed. Saving a preset that uses a
  bundled file writes that form automatically (`presets::makeRef`), and a moved or renamed bundled file is
  still found by its content hash. The preset format stays version 2.
- Put a cab's IRs in a folder of their own under `irs/` and start each file name with the folder's name
  ("Vintage 4x12, dynamic, lower.wav" in `irs/Vintage 4x12/`): the cab page lists each folder under its
  own heading, with the prefix left off each entry. A bundled folder is never treated as a cab pack
  unless it has a `cabpack.json`.
- Every file needs an entry in `manifest.json`, or the build fails. The entry generates the file's
  section of `THIRD_PARTY_NOTICES.txt` (shipped in the app and the DMG, and shown under the brand
  menu's "About / licenses"). A gain set's `gainset.json` is the one exception: it only lists its
  captures, and each capture has its own entry (`tools/notices/make_notices.cmake`).

## manifest.json

```json
{
  "credits": [ "Optional: thank-you notes, printed once above the files in the notices" ],
  "files": [
    {
      "path": "models/Example Clean.nam",
      "title": "Example Clean (amp only)",
      "description": "Optional: the line under its name in the app's lists",
      "author": "Who captured it",
      "source": "https://where-it-came-from",
      "license": "CC BY 4.0",
      "license_file": "licenses/CC-BY-4.0.txt",
      "notes": "Optional: what was changed, e.g. renamed"
    }
  ]
}
```

`path` and `license_file` are relative to this folder. Put licence texts under `licenses/` (each text
once; several files can share it). Only bundle files whose licence allows redistribution, and never
anything from Neural DSP or another company's products (CLAUDE.md).
