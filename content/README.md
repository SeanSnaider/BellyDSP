# Bundled content

Captures and impulse responses that ship inside Amp Sim, so a friend has something to play the moment
the app opens.

- `irs/Vintage 4x12/` (6 IRs) and `irs/Modern 4x12/` (15 IRs): the "Emerald" and "Brutal" cab IR packs by
  Bastian Karschewski (Jester Dyne Productions), which he released under CC0 (public domain). Renamed
  without brand names and the modern ones cut to 1 s by `tools/content/import_jester_irs.py`; each file's
  original name is in its manifest entry. The cab page lists them under "Built in", one entry per IR.
- `models/`: none yet. The bundled captures will be Sean's own (`docs/CAPTURING.md`).

## How it works

- Everything in this folder except this README is copied into the app when it's built: into
  `Amp Sim.app/Contents/Resources/content/` on macOS and into `content\` next to `Amp Sim.exe` on
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
  menu's "About / licenses").

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
