#!/usr/bin/env python3
"""Render a folder of things to listen to, on a real instrument, and say what each file is for.

    python tools/listen.py                       # build/listen/*.wav on the shipped instrument
    python tools/listen.py --instrument some.sfz --out build/listen

Every file is rendered by `boreaz-render`, so the audio is the audio the app would have made, and
nothing here needs an audio device. The four committed fixtures are rendered as they stand, plus two
written here: a velocity ladder that walks across the instrument's own layer split, and one note
held long enough to hear what a loop point sounds like.

Each line printed next to the file is the loudness of what was actually written, so "it sounded
quiet" can be answered with a number instead of a shrug. Peak is full-scale, so 0.15 is about -16 dB.

What cannot be heard here is said at the end, because a listening pack that does not say so is a
listening pack someone will misunderstand.
"""

import argparse
import array
import subprocess
import sys
import wave
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))

import midifile_fixtures as mf  # noqa: E402  - the MIDI writer this repo already has

STARTER = REPO / "third_party/instruments/upright-piano-KW/UprightPianoKW-20220221.sfz"
# The velocity the shipped instrument splits its two layers on, read from its <group> labels.
LAYER_SPLIT = 81


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
    # The ladder walks over the layer split on purpose: ...78, 80 | 81, 88... is the two recordings
    # of the same key played next to each other, which is the A/B a slider cannot do.
    ladder = args.out / "velocity-ladder.mid"
    ladder.write_bytes(velocity_ladder(mf.C4, [8, 20, 34, 48, 62, 78, 80, 81, 88, 100, 112, 127], beat))
    held = args.out / "hold-one-note.mid"
    held.write_bytes(hold_one_note(mf.C4, 96, 16, beat))
    scores += [(ladder.name, ladder), (held.name, held)]

    print(f"instrument  {args.instrument}\nout         {args.out}\n")
    print(f"{'file':<28}{'seconds':>9}{'peak':>8}{'rms':>8}   what to listen for")
    for name, source in scores:
        wav = args.out / (source.stem + ".wav")
        subprocess.run([str(render), "--instrument", str(args.instrument), str(source), str(wav),
                        "--rate", "48000"], check=True, capture_output=True)
        peak, rms = loudness(wav)
        print(f"{name:<28}{seconds(wav):>9.2f}{peak:>8.4f}{rms:>8.4f}   {WHY.get(name, '')}")

    print("\nnot in this pack:")
    print("  release recordings - the instrument above has none, so the release voice pool cannot")
    print("  be heard here at all; it needs a library recorded with keys being let go.")
    print("  filters - three regions of the instrument ask for lpf_2p@2200 and a sampled voice has")
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
