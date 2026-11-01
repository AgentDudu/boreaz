#!/usr/bin/env python3
"""The gate: build, run the tests, then prove the release behaviour with audio.

The last section is the point, and it is the half no amount of test counting gives you. It renders
one MIDI note twice from the same engine and the same params.ini - once from a mapping with a plain
`key=60` region, once with that region plus a `trigger=release` region - and measures three
windows. The note has to sound in both. After the key-up only the build with a release region may
still be making a noise. Then both must be silent, well past the end.

Usage:  python tools/release_check.py [--binary-dir build] [--instrument FILE] [--skip-build]

`--binary-dir` is where the executables actually are: `build` for a single-config Ninja tree,
`build/Release` for a Visual Studio one. `build/` is gitignored, which is why this script lives in
`tools/`: the copy of it that lived in `build/` was replaced from under a session that had just
tested it.

What is deliberately not here:

  * `boreaz.exe`. It has no headless flag - headless is "runs until Ctrl+C", which a script cannot
    end politely, and it opens a real audio device. The instrument path it uses is the same
    `Engine::load_instrument` the render tool drives, so it is gated below; loading it in the app
    is still a thing a human should do at a speaker.
  * benchmarks. PLAN.md quotes `boreaz_bench.exe`, a "3286 assertions" total, an `engine` test
    suite, `--headless --ticks=N`, `--asset-root`, a `boreaz-cc0` preset name and `-ts=release`.
    None of those exist in this repository - no bench source file, no bench target, no suite named
    `engine`, no matching command-line flag, and no commit on any branch that ever had them. They
    belong to a build tree that is not this one. Run the commands below and believe the numbers.
"""
import argparse
import array
import math
import os
import re
import struct
import subprocess
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "release-check"
RATE = 44100
STARTER = ROOT / "third_party" / "instruments" / "upright-piano-KW" / "UprightPianoKW-20220221.sfz"

parser = argparse.ArgumentParser()
parser.add_argument("--binary-dir", default=str(ROOT / "build"))
parser.add_argument("--instrument", default=str(STARTER),
                    help="the starter instrument .sfz; tools/mkinstrument.py builds it")
parser.add_argument("--skip-build", action="store_true")
ARGS = parser.parse_args()


def tool(name: str) -> str:
    exe = name + (".exe" if os.name == "nt" else "")
    for candidate in (Path(ARGS.binary_dir) / exe, Path(ARGS.binary_dir) / "Release" / exe,
                      Path(ARGS.binary_dir) / "Debug" / exe):
        if candidate.exists():
            return str(candidate)
    sys.exit(f"{exe} not found under {ARGS.binary_dir} - build first")


RENDER = tool("boreaz-render")
TESTS = tool("boreaz_tests")
INSTRUMENT = Path(ARGS.instrument)


def run(failures, what, argv, cwd=str(OUT), expect=0, grep=None):
    print("\ngate: " + what)
    print("  " + " ".join(str(a) for a in argv))
    proc = subprocess.run([str(a) for a in argv], cwd=cwd, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, errors="replace")
    ok = proc.returncode == expect
    if ok and grep:
        ok = grep in proc.stdout
    if ok:
        print("  " + (grep if grep else "exit %d" % proc.returncode))
    else:
        print(proc.stdout[-3000:])
    print(("OK: " if ok else "FAIL: ") + what)
    if not ok:
        failures.append(what)


def wav(path, frames, make):
    """Write `frames` 16 bit stereo frames. make(n) is called with a frame index, not a time."""
    buf = array.array("h")
    for n in range(frames):
        left, right = make(n)
        buf.extend((int(max(-1.0, min(1.0, left)) * 32767),
                    int(max(-1.0, min(1.0, right)) * 32767)))
    with wave.open(str(path), "wb") as out:
        out.setnchannels(2), out.setsampwidth(2), out.setframerate(RATE)
        out.writeframes(buf.tobytes())


def note_wav(path, hertz, seconds):
    def make(n):
        t = n / RATE
        # A decaying sine: what a looped recording with no release envelope sounds like.
        v = 0.6 * math.sin(2 * math.pi * hertz * t) * math.exp(-3.0 * t)
        return v, v

    wav(path, int(RATE * seconds), make)


def release_wav(path, seconds):
    """A percussive one-shot, which is what a hammer-noise recording is: sound, then floor."""
    import random
    random.seed(1)  # deterministic, or this fails differently on two machines

    def make(n):
        t = n / RATE
        v = 0.5 * random.uniform(-1.0, 1.0) * math.exp(-60.0 * t)  # noqa: S311 - a test signal
        return v, v

    wav(path, int(RATE * seconds), make)


def realtime(failures, instrument):
    """The real-time promise, measured: while the engine renders, it issues no file reads.

    `boreaz-render --counters` reads the process's own page-fault and IO counters around the engine
    call and prints three lines - loading, the first blocks, everything after them. Only the last
    line is asserted, and only its read count is pinned: the page-fault half of the promise was
    never true in the strict sense (a render takes a handful of soft faults as the process first
    touches its own memory) and `reads == 0` is what "no disk in the callback" actually means.
    The fault count is bounded loosely, as a leak detector rather than as a proof.
    """
    argv = [RENDER, "--golden", "--counters"]
    if instrument:
        argv += ["--instrument", instrument]
    proc = subprocess.run(argv + [OUT / "one.mid"], cwd=str(OUT), stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, errors="replace")
    steady = [line for line in proc.stdout.splitlines() if "after warm-up" in line]
    print("\ngate: the engine reads no files while it is rendering")
    print("  " + " ".join(str(a) for a in argv))
    # "after warm-up   0 / 0 / 0   <- zero here is the promise", and the numbers are what matter.
    numbers = re.findall(r"-?\d+", steady[0]) if steady else []
    ok = proc.returncode == 0 and len(numbers) == 3 and int(numbers[0]) < 1000 and \
        int(numbers[1]) == 0 and int(numbers[2]) == 0
    print("  " + (steady[0].strip() if steady else proc.stdout[-300:]))
    print(("OK: " if ok else "FAIL: ") + "no file reads and no page-fault flood after warm-up")
    if not ok:
        failures.append("the engine reads files while it renders")


def midi(path):
    def vlq(value):
        out, stack = bytearray(), [value & 0x7F]
        while value >> 7:
            value >>= 7
            stack.append(0x80 | (value & 0x7F))
        for byte in reversed(stack):
            out.append(byte)
        return bytes(out)

    # One quarter note: 480 ticks at 480 tpq with the default 120 BPM tempo is half a second, and
    # boreaz-render adds one second of release tail, so the WAV is 1.5 s - which is what the three
    # measurement windows below are cut against. The end of track carries no delta of its own: the
    # vlq before it is the delta, and a spare 0x00 there reads as a running-status note off.
    events = b"\x00\x90\x3C\x64" + vlq(480) + b"\x80\x3C\x40" + vlq(0) + b"\xFF\x2F\x00"
    path.write_bytes(b"MThd\x00\x00\x00\x06\x00\x00\x00\x01\x01\xE0" + b"MTrk" +
                     struct.pack(">I", len(events)) + events)


def rms(path, from_seconds, to_seconds):
    with wave.open(str(path)) as reader:
        assert reader.getframerate() == RATE and reader.getnchannels() == 2, str(path)
        reader.setpos(int(RATE * from_seconds))
        frames = reader.readframes(int(RATE * (to_seconds - from_seconds)))
    samples = array.array("h")
    samples.frombytes(frames)
    if not samples:
        return 0.0
    # RMS rather than peak: one click would satisfy a peak check, and the question is whether
    # anything is still playing, not whether anything ever moved.
    return math.sqrt(sum(float(s) * float(s) for s in samples) / len(samples)) / 32767.0


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    note_wav(OUT / "note.wav", 440.0, 1.0)
    release_wav(OUT / "hammer.wav", 0.5)
    (OUT / "with.sfz").write_text("<region> sample=note.wav key=60\n"
                                  "<region> sample=hammer.wav key=60 trigger=release\n", newline="\n")
    (OUT / "without.sfz").write_text("<region> sample=note.wav key=60\n", newline="\n")
    # A fast attack, or the note spends the whole "held" window still ramping up (the engine's
    # default amp_attack_s is around half a second, which is a sound-design default, not something
    # a measurement of "is the note playing" wants). A short amp release, because the point of the
    # third window is the release *recording*: with a long amp_release_s the note's own tail is
    # still ringing at 0.6 s and the hammer noise is invisible in a wide-window RMS.
    (OUT / "params.ini").write_text("amp_attack_s = 0.005\namp_release_s = 0.02\nmaster_gain_db = 0\n",
                                     newline="\n")
    midi(OUT / "one.mid")
    failures: list[str] = []

    if not ARGS.skip_build:
        run(failures, "the build is current", ["cmake", "--build", str(ROOT / "build"), "-j", "4"],
            cwd=str(ROOT))

    # The whole suite. There are no test suites to select: `-ts=engine` matches nothing in this
    # tree and doctest calls that success, so an argument-filtered gate can pass over nothing.
    run(failures, "boreaz_tests", [TESTS], cwd=str(ROOT / "build"))

    # The render contract, both ways round. --golden prints measurements and writes no file, and
    # --instrument takes a mapping: the assertion is that a run with the loader switched on still
    # produces measurements, and that the default synthesiser path is untouched by its existence.
    run(failures, "the render contract (synthesiser)",
        [RENDER, "--golden", OUT / "one.mid"], grep="fnv1a64=")
    if INSTRUMENT.exists():
        run(failures, "the render contract (sampled instrument)",
            [RENDER, "--golden", "--instrument", INSTRUMENT, OUT / "one.mid"],
            grep="fnv1a64=")
        run(failures, "a sampled instrument renders to a file",
            [RENDER, "--instrument", INSTRUMENT, OUT / "one.mid", OUT / "instrument.wav",
             OUT / "params.ini"])
        realtime(failures, INSTRUMENT)
    else:
        print(f"\nskipped: no starter instrument at {INSTRUMENT} - tools/mkinstrument.py builds it")
    run(failures, "a synthesiser render still works",
        [RENDER, OUT / "one.mid", OUT / "plain.wav", OUT / "params.ini"])
    run(failures, "bad usage is refused with 2, not 0",
        [RENDER, "--no-such-flag", OUT / "one.mid", OUT / "plain.wav"], expect=2)
    run(failures, "the live parameter bus",
        [TESTS, "-tc=a parameter moves toward its new value instead of arriving there"],
        cwd=str(ROOT / "build"))
    run(failures, "the computer keyboard layout", [TESTS, "-tc=computer keyboard"],
        cwd=str(ROOT / "build"))

    for name in ("with", "without"):
        subprocess.run([RENDER, "--instrument", OUT / (name + ".sfz"), "--rate", str(RATE),
                        OUT / "one.mid", OUT / (name + ".wav"), OUT / "params.ini"],
                       check=True, cwd=str(OUT))

    print("\nwindow            with release      no release")
    for label, a, b in (("note held", 0.10, 0.40), ("30-100 ms after the key-up", 0.53, 0.60),
                        ("1.20-1.45 s, everything gone", 1.20, 1.45)):
        left = rms(OUT / "with.wav", a, b)
        right = rms(OUT / "without.wav", a, b)
        print("%-26s %8.4f %10.6f" % (label, left, right))
        if label.startswith("note held"):
            assert left > 0.05 and right > 0.05, "the note itself must sound in both"
        if label.startswith("30-100"):
            # Measured on this engine: 0.0078 against 0.000000. Small, because amp_release_s above
            # cuts the release recording as well as the note - but the threshold is a quarter of
            # what it measures, and the other file is exactly silent, which is the whole claim.
            assert left > 0.002, "the release recording is not audible after the note died"
            assert right < 1e-5, "something sounded after the key-up with no release region"
        if label.startswith("1.20-1.45"):
            assert left < 1e-5 and right < 1e-5, "a voice outlived its own file"
    print("OK: the release is audible only where a release region exists")

    if failures:
        print("\n%d gate(s) failed: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("\nOK: every gate ran")
    return 0


if __name__ == "__main__":
    sys.exit(main())
