#!/usr/bin/env python3
"""Render a folder of things to listen to, on a real instrument, and say what each file is for.

    python tools/listen.py                       # build/listen/*.wav on the shipped instrument
    python tools/listen.py --instrument some.sfz --out build/listen

Every file is rendered by `boreaz-render`, so the audio is the audio the app would have made, and
nothing here needs an audio device. The four committed fixtures are rendered as they stand, plus two
written here: a velocity ladder that walks across the instrument's own layer split, and one note
held long enough to hear what a loop point sounds like.

Each line printed next to the file is the loudness of what was actually written, so "it sounded
quiet" can be answered with a number instead of a shrug. Peak is full-scale, so 0.15 is about -16 dB
— but it is full-scale *after* the master bus, which runs `tanh(x)` at drive 1.0, so a peak here is a
compressed number and not a level: `tanh(1.0)` is 0.7616, full scale is not something `--gain` can
reach, and the last line of this report says what the same moment looked like before the clipper.
The pack is rendered at `--gain 0` (0 dB) by default, which is 12 dB hotter than the engine's own
-12 dB default. `--gain -12` gives back the level the app comes up with, and `--gain -40` is where a
level stops being compressed at all (+6 dB measured on a `volume=` change measures +6.02 dB there,
+4.71 dB at 0 dB).

What cannot be heard here is said at the end, because a listening pack that does not say so is a
listening pack someone will misunderstand.
"""

import argparse
import array
import math
import re
import subprocess
import sys
import wave
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))

import midifile_fixtures as mf  # noqa: E402  - the MIDI writer this repo already has

STARTER = REPO / "third_party/instruments/upright-piano-KW/UprightPianoKW-20220221.sfz"


def velocity_ladder(key, velocities, beat):
    """One key, struck once per beat, at the given velocities, so the layers and the velocity curve
    arrive as one sound rather than as five renders nobody lines up."""
    events = [(0, mf.tempo(500000)), (0, mf.sequence_name("velocity-ladder"))]
    for index, velocity in enumerate(velocities):
        events.append((index * beat, mf.note_on(0, key, velocity)))
        events.append((index * beat + beat - 80, mf.note_off(0, key)))
    end = (len(velocities) - 1) * beat + beat
    events.append((end, mf.end_of_track()))
    return mf.midi_file(0, [mf.encode_track(mf.deltas(events))])


def hold_one_note(key, velocity, beats, beat):
    """One note, held, with no key off until it is over: what a loop seam sounds like after six
    seconds, which is the thing a two second render cannot show."""
    events = [
        (0, mf.tempo(500000)),
        (0, mf.sequence_name("hold-one-note")),
        (0, mf.note_on(0, key, velocity)),
        (beats * beat, mf.note_off(0, key)),
        (beats * beat, mf.end_of_track()),
    ]
    return mf.midi_file(0, [mf.encode_track(mf.deltas(events))])


def loudness(path):
    """(peak, rms) of a rendered file, full scale, read with the standard library."""
    with wave.open(str(path)) as reader:
        channels, frames = reader.getnchannels(), reader.getnframes()
        data = array.array("h")
        data.frombytes(reader.readframes(frames))
    if not data:
        return 0.0, 0.0
    samples = [value / 32768.0 for value in data]
    peak = max(abs(value) for value in samples)
    rms = (sum(value * value for value in samples) / len(samples)) ** 0.5
    return peak, rms


def seconds(path):
    with wave.open(str(path)) as reader:
        return reader.getnframes() / reader.getframerate()


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--instrument", type=Path, default=STARTER)
    parser.add_argument("--binary-dir", type=Path, default=REPO / "build")
    parser.add_argument("--out", type=Path, default=REPO / "build" / "listen")
    # Text, not a float: boreaz-render's --gain owns the range and the refusal, and a tool that
    # re-checked the number here would be a second place for the same rule to drift.
    parser.add_argument("--gain", default="0", help="master level in dB for the pack (0 dB is "
                        "about as loud as this instrument renders without clipping)")
    args = parser.parse_args(argv)

    render = args.binary_dir / "boreaz-render.exe"
    if not render.exists():
        print(f"{render} not built - cmake --build {args.binary_dir}", file=sys.stderr)
        return 1
    if not args.instrument.exists():
        print(f"{args.instrument} not found", file=sys.stderr)
        return 1

    args.out.mkdir(parents=True, exist_ok=True)
    beat = mf.TPQ  # one note per quarter note at 120 BPM: half a second a note

    scores = [(path.name, path) for path in sorted((REPO / "tests/fixtures/midifile").glob("*.mid"))]
    # The ladder walks over the layer splits on purpose: 8, 20, 34 | 48, 62 | 78, 80, 81, 88 | 100,
    # 112, 127 crosses every boundary a four-band mapping has (the shipped instrument's two layers
    # split on 80/81, and the Salamander subset splits on 34/64/96), which is the A/B a slider
    # cannot do - a different recording of the same key, next to each other.
    ladder = args.out / "velocity-ladder.mid"
    ladder.write_bytes(velocity_ladder(mf.C4, [8, 20, 34, 48, 62, 78, 80, 81, 88, 100, 112, 127], beat))
    held = args.out / "hold-one-note.mid"
    held.write_bytes(hold_one_note(mf.C4, 96, 16, beat))
    scores += [(ladder.name, ladder), (held.name, held)]

    print(f"instrument  {args.instrument}")
    print(f"gain        {args.gain} dB")
    print(f"out         {args.out}\n")
    print(f"{'file':<28}{'seconds':>9}{'peak':>8}{'rms':>8}   what to listen for")
    clipping = []
    peaks = []
    for name, source in scores:
        wav = args.out / (source.stem + ".wav")
        subprocess.run([str(render), "--instrument", str(args.instrument), "--gain", args.gain,
                        str(source), str(wav), "--rate", "48000"],
                       check=True, capture_output=True)
        peak, rms = loudness(wav)
        peaks.append(peak)
        # 0.7 of output is 0.87 before the clipper, which is where the tanh has taken more than a dB
        # off the transient: dynamics measured above that line are the curve's, not the piano's.
        # It used to be 0.99, which is a level no output reaches — tanh only gets there from 2.65.
        if peak >= 0.7:
            clipping.append(name)
        print(f"{name:<28}{seconds(wav):>9.2f}{peak:>8.4f}{rms:>8.4f}   {WHY.get(name, '')}")
    if clipping:
        print(f"\n  {', '.join(clipping)}: over 0.7 out, so the master-bus tanh is taking real level "
              "off the loudest moment - lower --gain before judging dynamics")

    loudest = max(peak for peak in peaks) if peaks else 0.0
    if 0 < loudest < 0.9:
        # Say what the loudest moment was *before* the clipper, because that is the number that says
        # how much room is left. The line used to promise that `--gain N` spent the room and landed
        # at full scale, which the tanh makes untrue at every N.
        preclip = math.atanh(loudest)
        off = -20.0 * math.log10(math.tanh(preclip) / preclip)
        print(f"\nheadroom  the set's loudest moment is {preclip:.4f} before the clipper, "
              f"{20 * math.log10(1.0 / preclip):.1f} dB below full scale, and the tanh is already "
              f"taking {off:.1f} dB off it")

    print("\nnot in this pack:")
    text = args.instrument.read_text(encoding="utf-8", errors="replace")
    releases = len(re.findall(r"trigger\s*=\s*release", text))
    if releases:
        print(f"  releases   {releases} key-off recordings in this instrument. They fire on "
              f"velocity-ladder\n             (twelve strikes, so twelve key-offs) and lead-simple "
              f"(a gap in every note),\n             and not on hold-one-note, which never lets go.")
    else:
        print("  release recordings - the instrument above has none, so the release voice pool cannot")
        print("  be heard here at all; it needs a library recorded with keys being let go.")
    filtered = len(re.findall(r"fil_type|\bcutoff\b", text))
    if filtered:
        print(f"  filters - {filtered} regions of the instrument ask for a filter and a sampled voice has")
        print("  no filter, which is measured elsewhere as a 5 % shift in brightness: inaudible.")
    return 0


WHY = {
    "bass-running-status.mid": "staccato eighths, low: note off reaching the voice, and the top of "
                               "a file that drops its status bytes",
    "glissando-88.mid": "every key, once, velocities 40 to 120: a wrong sample for a wrong key, and "
                        "the layer split walking past you",
    "hold-one-note.mid": "one note, 8 s, no key off: the loop seam, and whether a held note stays "
                         "alive or dies",
    "lead-simple.mid": "eight ascending notes with a gap in each: attack and the decay of a real "
                       "recording",
    "pad-chords.mid": "twelve notes at once, four tracks: what the voice pool does when it has more "
                      "notes than a piano",
    "velocity-ladder.mid": "one key, twelve strikes, 8 to 127: the velocity curve, and the step "
                           "where the soft recording stops and the loud one starts",
}

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
