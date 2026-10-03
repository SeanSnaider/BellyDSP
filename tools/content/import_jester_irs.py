# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Imports the two CC0 cab IR packs by Jester Dyne Productions (Bastian Karschewski) into content/irs/.

    uv run --with numpy --with soundfile python tools/content/import_jester_irs.py \
        --emerald "<unzipped>/Emerald Pack 1.0" --brutal "<unzipped>/Jesters_Brutal_Pack_1.0"

Downloads: https://www.jester-dyne-productions.com/emerald-ir-pack/ and .../brutal-ir-pack/ (free; each
zip has 44.1 and 48 kHz folders and a handbook PDF). Only the 48 kHz files are used, because the app
runs at 48 kHz only. The handbooks are not shipped: they name the gear's brands.

What it does to each file, and nothing else:
  - renames it neutrally (the packs' own names are jokes; the handbook's patch list says which mic and
    speaker each one is, and the names below describe that without brand or model names),
  - cuts anything longer than 1.0 s to 1.0 s with a 50 ms half-cosine fade-out. The app's loader
    (CabIR::maxIRSeconds) plays at most 1 s of any IR anyway, ending it with a 10 ms linear fade; cutting
    it here means what ships is what plays, with a gentler fade, and the app doesn't report "cut to 1 s".
    The samples before the fade are untouched (same 24-bit values).
It then rewrites content/manifest.json's entries under irs/ (keeping every other entry).

Vocabulary (docs/ASSUMPTIONS.md DS22 to DS24):
  dynamic        the classic cardioid dynamic instrument mic both packs use most
  supercardioid  the flat-fronted supercardioid dynamic amp mic
  vocal mic      the handheld cardioid dynamic vocal mic in the modern pack
  upper / lower  which speaker of the 4x12 the mic is on (the handbook gives no position on the cone)
  bright 60 W, dark 60 W, 75 W
                 the modern pack's three speaker models. The two 60 W ones are told apart by measurement:
                 the "bright" one's single-speaker IRs have a spectral centroid (80 Hz to 10 kHz) of
                 2.8 to 3.1 kHz, the "dark" one's 2.2 to 2.5 kHz with 4.5 to 5.6 dB more below 200 Hz.
  var. 1, 2, 3   the handbook lists several IRs with the same mic and speaker; they're different captures
                 (different placements it doesn't describe).
"""

import argparse
import json
import pathlib

import numpy as np
import soundfile as sf

REPO = pathlib.Path(__file__).resolve().parents[2]
CONTENT = REPO / "content"
MAX_SECONDS = 1.0
FADE_SECONDS = 0.050

AUTHOR = "Jester Dyne Productions (Bastian Karschewski)"
LICENCE_QUOTE = (
    'Handbook p. 3: "LICENSED 2022 UNDER: [CC0 Public Domain badge] CC0 (aka CC Zero) is a public '
    "dedication tool, which allows creators to give up their copyright and put their works into the "
    "worldwide public domain. CC0 allows reusers to distribute, remix, adapt, and build upon the material "
    'in any medium or format, with no conditions."'
)

PACKS = {
    "emerald": {
        "title": "Jesters Emerald Pack 1.0",
        "source": "https://www.jester-dyne-productions.com/emerald-ir-pack/",
        "folder": "Vintage 4x12",
        "suffix": "_48",
        "room": "dry room",
        # handbook number: (original stem, new name after "Vintage 4x12, ", list description)
        "files": {
            1: ("1_Nacho_Guacamole", "dynamic, upper, var. 1", "25 W speakers, dry room"),
            2: ("2_Pickle_Punisher", "dynamic, upper, var. 2", "25 W speakers, dry room"),
            3: ("3_Wasabi_Warrior", "dynamic, upper, var. 3", "25 W speakers, dry room"),
            4: ("4_Pesto_Paladin", "supercardioid, upper", "25 W speakers, dry room"),
            5: ("5_Don_Spinacio", "dynamic, lower", "25 W speakers, dry room"),
            6: ("6_Kill_Dill", "supercardioid, lower", "25 W speakers, dry room"),
        },
    },
    "brutal": {
        "title": "Jesters Brutal Pack 1.0",
        "source": "https://www.jester-dyne-productions.com/brutal-ir-pack/",
        "folder": "Modern 4x12",
        "suffix": "",
        "files": {
            1: ("1_Cookie_Monster", "dynamic, bright 60 W, var. 1", "One mic, room ambience"),
            12: ("12_World_Collider", "dynamic, bright 60 W, var. 2", "One mic, room ambience"),
            14: ("14_Cathode_Ray_Fleshburn", "dynamic, bright 60 W, var. 3", "One mic, room ambience"),
            9: ("9_Devils_Cunnilingus", "vocal mic, bright 60 W", "One mic, room ambience"),
            11: ("11_Wumbo", "dynamic, dark 60 W", "One mic, room ambience"),
            3: ("3_Kitten_Slayer", "supercardioid, dark 60 W", "One mic, room ambience"),
            2: ("2_Darth_Genocider", "dynamic, 75 W, var. 1", "One mic, room ambience"),
            10: ("10_October_32th", "dynamic, 75 W, var. 2", "One mic, room ambience"),
            13: ("13_Cannibal_Choir", "dynamic, 75 W, var. 3", "One mic, room ambience"),
            15: ("15_Impaler_Jim", "dynamic, 75 W, var. 4", "One mic, room ambience"),
            4: ("4_Kaiju_Tamer", "blend, dark 60 W + 75 W", "Supercardioid + dynamic, room"),
            5: ("5_Iceburn_Suicide", "blend, dark + bright 60 W, 1", "Supercardioid + vocal mic, room"),
            8: ("8_Big_Bubba", "blend, dark + bright 60 W, 2", "Supercardioid + dynamic, room"),
            6: ("6_Vertical_Lip_Stabber", "blend, 75 W + bright 60 W, 1", "Two dynamics, room"),
            7: ("7_Manslaughter_Joe", "blend, 75 W + bright 60 W, 2", "Vocal mic + dynamic, room"),
        },
    },
}


def find_48k(pack_dir: pathlib.Path, stem: str) -> pathlib.Path:
    matches = [p for p in pack_dir.rglob("*.wav") if p.parent.name == "48kHz" and p.stem == stem]
    if len(matches) != 1:
        raise SystemExit(f"expected exactly one 48 kHz {stem}.wav under {pack_dir}, found {len(matches)}")
    return matches[0]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emerald", type=pathlib.Path, required=True, help="the unzipped Emerald Pack 1.0 folder")
    parser.add_argument("--brutal", type=pathlib.Path, required=True, help="the unzipped Jesters_Brutal_Pack_1.0 folder")
    args = parser.parse_args()

    entries = []
    rows = []
    for key, pack in PACKS.items():
        pack_dir = getattr(args, key)
        out_dir = CONTENT / "irs" / pack["folder"]
        out_dir.mkdir(parents=True, exist_ok=True)
        for number, (stem, name, description) in pack["files"].items():
            source = find_48k(pack_dir, stem + pack["suffix"])
            info = sf.info(str(source))
            if info.samplerate != 48000:
                raise SystemExit(f"{source.name} is {info.samplerate} Hz, not 48000")
            # Read the raw 24-bit integers (as int32, left-justified) so untouched samples stay bit-exact.
            data, rate = sf.read(str(source), dtype="int32", always_2d=True)
            original_len = data.shape[0]
            notes = f"Original file: {source.name} (handbook patch {number}, 48 kHz folder). Renamed."
            if original_len > MAX_SECONDS * rate:
                n = int(MAX_SECONDS * rate)
                fade = int(FADE_SECONDS * rate)
                data = data[:n].copy()
                # Half-cosine (raised-cosine) fade, 1 -> 0 over the last 50 ms: no step at the end, and a
                # smooth start to the fade, unlike a linear ramp's corner.
                ramp = 0.5 * (1.0 + np.cos(np.pi * np.arange(fade) / fade))
                faded = np.round(data[n - fade :].astype(np.float64) * ramp[:, None] / 256.0) * 256.0
                data[n - fade :] = faded.astype(np.int32)
                notes += (
                    f" Cut from {original_len / rate:.3f} s to {n / rate:.3f} s with a {FADE_SECONDS * 1000:.0f} ms"
                    " half-cosine fade-out (the app plays at most 1 s of an IR)."
                )
            notes += " " + LICENCE_QUOTE
            file_name = f"{pack['folder']}, {name}.wav"
            out = out_dir / file_name
            sf.write(str(out), data, rate, subtype="PCM_24")
            peak = np.max(np.abs(data.astype(np.float64))) / 2.0**31
            rows.append((file_name, source.name, original_len / rate, data.shape[0] / rate, 20 * np.log10(peak), out.stat().st_size))
            entries.append(
                {
                    "path": f"irs/{pack['folder']}/{file_name}",
                    "title": f"{pack['title']}, patch {number}",
                    "description": description,
                    "author": AUTHOR,
                    "source": pack["source"],
                    "license": "CC0 1.0",
                    "license_file": "licenses/CC0-1.0.txt",
                    "notes": notes,
                }
            )

    manifest_path = CONTENT / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    kept = [e for e in manifest.get("files", []) if not e["path"].startswith("irs/")]
    manifest["files"] = kept + sorted(entries, key=lambda e: e["path"])
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")

    total = 0
    print(f"{'new name':48s} {'original':32s} {'length':>16s} {'peak':>9s} {'bytes':>8s}")
    for new, old, before, after, peak_db, size in rows:
        total += size
        length = f"{before:.3f} s" if abs(before - after) < 1e-9 else f"{before:.3f}->{after:.3f} s"
        print(f"{new:48s} {old:32s} {length:>16s} {peak_db:6.1f} dB {size:8d}")
    print(f"{len(rows)} files, {total} bytes ({total / 1e6:.2f} MB); manifest: {len(entries)} IR entries")


if __name__ == "__main__":
    main()
