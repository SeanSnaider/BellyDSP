# Bundled content

Captures and impulse responses that ship inside Amp Sim, so a friend has something to play the moment
the app opens. It's empty for now: the starter pack (free-licensed captures and IRs) is being picked
separately.

## How it works

- Everything in this folder except this README is copied into the app when it's built: into
  `Amp Sim.app/Contents/Resources/content/` on macOS and into `content\` next to `Amp Sim.exe` on
  Windows (the installer installs it there).
- Put captures under `models/` and IRs or cab packs under `irs/`, the same layout as the user library.
- A preset refers to a bundled file as `factory:models/<file>.nam` or `factory:irs/<file>.wav`. The app
  resolves that against its own content folder, wherever it's installed. Saving a preset that uses a
  bundled file writes that form automatically (`presets::makeRef`), and a moved or renamed bundled file is
  still found by its content hash. The preset format stays version 2.
- Every file needs an entry in `manifest.json`, or the build fails. The entry generates the file's
  section of `THIRD_PARTY_NOTICES.txt` (shipped in the app and the DMG, and shown under the brand
  menu's "About / licenses").

## manifest.json

```json
{
  "files": [
    {
      "path": "models/Example Clean.nam",
      "title": "Example Clean (amp only)",
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
