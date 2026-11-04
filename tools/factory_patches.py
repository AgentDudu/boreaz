#!/usr/bin/env python3
"""Write (or check) the factory patches in patches/.

The one rule about patch files is that they are complete: `adopt_patch` starts from the parameters
that are playing and layers the file on top, so a file with a key missing is a patch whose sound
depends on what happened to be loaded before it. A complete file is a sound.

So the deltas below - which is the musical content, and all anyone should have to read to review a
patch - are expanded against the engine's own table in src/engine/param_table.cpp. That file is the
only place a default lives; this script reads it rather than copying it, so renaming a parameter is
a change in one place.

    python tools/factory_patches.py --write    # write patches/*.txt
    python tools/factory_patches.py --verify   # exit 1 if the files on disk differ

`tests/test_factory_patches.cpp` then checks the files themselves: that they parse through the same
strict reader the app uses, that every row of the table appears exactly once, and that none of them
names an instrument file.
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TABLE = ROOT / "src" / "engine" / "param_table.cpp"
PATCH_DIR = ROOT / "patches"

# `{"key", "Label", " unit", min, max, default, ...}` - a row may wrap, so the table is matched as
# one string. Only the first three numbers matter here.
ROW = re.compile(
    r'\{\s*"([a-z0-9_]+)"\s*,\s*"[^"]*"\s*,\s*"[^"]*"\s*,'
    r"\s*(-?[\d.]+f?)\s*,\s*(-?[\d.]+f?)\s*,\s*(-?[\d.]+f?)\s*,"
)


def table():
    text = TABLE.read_text(encoding="utf-8").replace("\r\n", "\n")
    rows = ROW.findall(text)
    if len(rows) != 63:
        sys.exit(f"expected 63 parameter rows in {TABLE}, found {len(rows)}")
    def number_in_source(text):
        return float(text[:-1] if text.endswith("f") else text)

    return [(key, number_in_source(lo), number_in_source(hi), number_in_source(default))
            for key, lo, hi, default in rows]


def number(value):
    return format(float(value), ".6g")


def render(name, blurb, overrides, rows):
    unknown = sorted(set(overrides) - {key for key, _, _, _ in rows})
    if unknown:
        sys.exit(f"{name}: keys that are not in the table: {', '.join(unknown)}")
    out = [
        "# boreaz patch format 1",
        "#",
        f"# name: {name}",
    ]
    for line in blurb:
        out.append(f"# {line}".rstrip())
    out += ["#", "# Every parameter the engine has, because a patch that leaves one out is a patch",
            "# whose sound depends on what was playing before it was loaded.", ""]
    changed = []
    for key, lo, hi, default in rows:
        value = overrides.get(key, default)
        if not lo <= float(value) <= hi:
            sys.exit(f"{name}: {key} = {value} is outside [{lo}, {hi}]")
        if key in overrides:
            changed.append(f"{key} = {number(value)}")
        out.append(f"{key} = {number(value)}")
    return "\n".join(out) + "\n", changed


# A0 to C8, in every patch: `filter_keytrack` moves the corner with the key, so a patch that is
# written for one register still tracks the keyboard instead of closing up three octaves away. The
# level is the shipped -12 dBFS of the table, so the five sit side by side at one loudness.
PATCHES = [
    (
        "sub-register.txt",
        "Sub Register",
        ["The lowest octave of the instrument: the octave above the fundamental turned up, the saw",
         "at zero, a corner that does not chase the keyboard, and a decay long enough that a low",
         "note is still in the room when the next one arrives."],
        {
            "filter_cutoff_ratio": 1.0,
            "filter_resonance": 0.25,
            "filter_keytrack": 0.5,
            "osc2_level": 0.9,
            "osc3_level": 0.0,
            "osc_detune_cents": 0.0,
            "noise_level": 0.0,
            "amp_attack_s": 0.012,
            "amp_decay_s": 4.0,
            "amp_sustain": 0.5,
            "amp_release_s": 1.5,
            "pan_spread": 0.25,
        },
    ),
    (
        "bass.txt",
        "Bass",
        ["The register that carries the chord. A little of the octave above and a little of the",
         "detuned unison saw give it edges without lifting the corner, and the hammer is a",
         "touch rather than a click."],
        {
            "filter_cutoff_ratio": 2.0,
            "filter_resonance": 0.3,
            "filter_keytrack": 0.7,
            "osc2_level": 0.6,
            "osc3_level": 0.2,
            "osc_detune_cents": 6.0,
            "noise_level": 0.05,
            "noise_decay_s": 0.08,
            "amp_attack_s": 0.008,
            "amp_decay_s": 2.5,
            "amp_sustain": 0.4,
            "amp_release_s": 1.0,
            "pan_spread": 0.4,
        },
    ),
    (
        "low-mid.txt",
        "Low Mid",
        ["The middle of the keyboard, where a note has to be heard as a pitch rather than felt. The",
         "corner tracks the key, the saw is audible, and the decay is where a struck string starts",
         "to leave room for the next note."],
        {
            "filter_cutoff_ratio": 4.0,
            "filter_resonance": 0.35,
            "filter_keytrack": 1.0,
            "osc2_level": 0.4,
            "osc3_level": 0.3,
            "osc_detune_cents": 8.0,
            "noise_level": 0.1,
            "noise_decay_s": 0.06,
            "amp_attack_s": 0.006,
            "amp_decay_s": 2.0,
            "amp_sustain": 0.25,
            "amp_release_s": 0.7,
            "pan_spread": 0.6,
        },
    ),
    (
        "upper.txt",
        "Upper",
        ["Above middle C, where the octave above the fundamental is what makes a melodic line",
         "carry. The hammer is a real transient now and the note is shorter, because that is what",
         "the high strings do."],
        {
            "filter_cutoff_ratio": 8.0,
            "filter_resonance": 0.35,
            "filter_keytrack": 1.0,
            "osc2_level": 0.45,
            "osc3_level": 0.45,
            "osc_detune_cents": 10.0,
            "noise_level": 0.16,
            "noise_decay_s": 0.04,
            "amp_attack_s": 0.004,
            "amp_decay_s": 1.2,
            "amp_sustain": 0.15,
            "amp_release_s": 0.5,
            "pan_spread": 0.7,
        },
    ),
    (
        "brilliance.txt",
        "Brilliance",
        ["The top of the instrument and the sparkle on top of everything else: a wide open corner,",
         "the saw as a partial rather than as body, and an attack fast enough that the first",
         "millisecond is the hit."],
        {
            "filter_cutoff_ratio": 16.0,
            "filter_resonance": 0.5,
            "filter_keytrack": 1.0,
            "osc2_level": 0.5,
            "osc3_level": 0.7,
            "osc_detune_cents": 14.0,
            "noise_level": 0.24,
            "noise_decay_s": 0.03,
            "amp_attack_s": 0.002,
            "amp_decay_s": 0.9,
            "amp_sustain": 0.1,
            "amp_release_s": 0.4,
            "pan_spread": 0.75,
        },
    ),
    # The sampled path honours four of the sixty-three: master level, the stereo spread, the
    # release at key-up, and the effects bus. Everything else - the filter, the oscillators, the
    # modulation - has nothing to act on, so these three are written as dynamics and a room.
    #
    # They name no instrument on purpose. The library's path is a path on this machine, and nothing
    # machine-local or binary belongs in git (PLAN section 5); a patch that names no instrument
    # keeps the library that is playing, which is the same rule the panel's loader already states.
    (
        "piano-pianissimo.txt",
        "Piano Pianissimo",
        ["Played softly: six decibels down, the damper lift slow, and the room the loudest it is",
         "going to get in a quiet note."],
        {
            "master_gain_db": -18.0,
            "amp_release_s": 1.2,
            "pan_spread": 0.6,
            "fx_reverb_mix": 0.35,
            "fx_reverb_seconds": 1.8,
        },
    ),
    (
        "piano-mezzo-forte.txt",
        "Piano Mezzo Forte",
        ["The shipped level, the shipped spread, a damper lift at the length the default plays at,",
         "and a room just behind the note rather than around it."],
        {
            "master_gain_db": -12.0,
            "amp_release_s": 0.6,
            "pan_spread": 0.6,
            "fx_reverb_mix": 0.3,
            "fx_reverb_seconds": 1.8,
        },
    ),
    (
        "piano-fortissimo.txt",
        "Piano Fortissimo",
        ["Played hard: six decibels up, the damper off the string quickly, the field narrower",
         "because a loud note heard wide smears, and the room back so the level reads as the",
         "playing rather than as the hall."],
        {
            "master_gain_db": -6.0,
            "amp_release_s": 0.35,
            "pan_spread": 0.45,
            "fx_reverb_mix": 0.18,
            "fx_reverb_seconds": 1.5,
        },
    ),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--verify", action="store_true")
    args = ap.parse_args()
    rows = table()
    changed_keys = {}
    for filename, name, blurb, overrides in PATCHES:
        text, changed = render(name, blurb, overrides, rows)
        changed_keys[filename] = changed
        path = PATCH_DIR / filename
        if args.write:
            path.parent.mkdir(exist_ok=True)
            path.write_text(text, encoding="utf-8", newline="\n")
        elif path.read_text(encoding="utf-8") != text:
            sys.exit(f"{path} is not what this script writes; run it with --write")
    if not args.write:
        print(f"{len(PATCHES)} patches match their recipes")
        sys.exit(0)
    print(f"wrote {len(PATCHES)} patches to {PATCH_DIR}")
    for filename, changed in changed_keys.items():
        print(f"  {filename}: {len(changed)} of 63 keys set away from the table default")


if __name__ == "__main__":
    main()
