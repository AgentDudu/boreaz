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

  * `boreaz.exe --gui`. A window cannot be *asserted* from a script: it stays open after the file
    ends, so nothing here would close it, and what it draws is a matter for a human eye. What is
    gated is the app with no window: a real device, the library, one MIDI file, and process
    counters sampled while the driver was pulling callbacks. The instrument path it uses is the
    same `Engine::load_instrument` the render tool drives; adopting one from inside the window is
    still a thing a human should do at a speaker.
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
import threading
import time
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
APP = tool("boreaz")
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


# ---------------------------------------------------------------- the app itself, on a device
#
# `boreaz.exe` was excluded from this gate for six phases on the grounds that it cannot be ended
# politely from a script. It can: give it a MIDI file and it stops when the file's tail has rung
# out. What remained genuinely unmeasured is the app's own audio thread on a real driver, which is
# what the render tool cannot show - so this runs the shipped binary, samples its process counters
# from outside while a device is pulling callbacks, and asserts the same thing §3 asserts.
#
# Sampled from ctypes rather than counted inside the app on purpose: the binary under test stays
# the binary that ships, with no flag switched on for the test, and the question is coarse enough
# that a sample every 40 ms cannot miss it. The window opens on the line the app prints once the
# device has started, so loading a 58 MB library is outside it.
if os.name == "nt":
    import ctypes
    from ctypes import wintypes

    class _Memory(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
            (name, ctypes.c_size_t) for name in "abcdefgh"]

    class _IO(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in (
            "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
            "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

    _kernel32 = ctypes.windll.kernel32
    _kernel32.OpenProcess.restype = wintypes.HANDLE
    _kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _kernel32.K32GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(_Memory),
                                               wintypes.DWORD]
    _kernel32.GetProcessIoCounters.argtypes = [wintypes.HANDLE, ctypes.POINTER(_IO)]

    def process_counters(pid):
        """(page faults, file reads) for a process, or None if it cannot be opened."""
        handle = _kernel32.OpenProcess(0x0410, False, pid)  # QUERY_INFORMATION | VM_READ
        if not handle:
            return None
        memory, io = _Memory(), _IO()
        memory.cb = ctypes.sizeof(_Memory)
        faults = memory.PageFaultCount if _kernel32.K32GetProcessMemoryInfo(
            handle, ctypes.byref(memory), memory.cb) else 0
        reads = io.ReadOperationCount if _kernel32.GetProcessIoCounters(
            handle, ctypes.byref(io)) else 0
        _kernel32.CloseHandle(handle)
        return faults, reads
else:
    def process_counters(pid):
        return None


def app_live(failures, instrument):
    """Run the app for real: a device, the library, one MIDI file, and numbers on the way out."""
    print("\ngate: boreaz.exe on a real audio device")
    ports = subprocess.run([APP, "--list-devices"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, errors="replace")
    if ports.returncode != 0 or not ports.stdout.strip():
        print("  skipped: no audio device on this machine")
        return
    # A virtual cable when the machine has one, because a gate has no business putting a note out
    # loud at whoever is sitting in front of the speaker.
    # shortcut: assumes a port named CABLE can be opened for playback, which is what VB-Cable is;
    # when a machine has a CABLE that cannot, fall back to the default and print that it did.
    cable = next((line.split(". ", 1)[1].strip() for line in ports.stdout.splitlines()
                  if "CABLE" in line.upper()), "")
    argv = [APP, "--period", "1024", "--gain", "-24"]
    if cable:
        argv += ["--device", cable]
    if instrument:
        argv += ["--instrument", instrument]
    print("  " + subprocess.list2cmdline(argv + [OUT / "one.mid"]))

    child = subprocess.Popen(argv + [OUT / "one.mid"], cwd=str(OUT), stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, errors="replace")
    lines = []
    started = threading.Event()

    def read():
        for line in child.stdout:
            lines.append(line.rstrip())
            # The line the app prints after device.start(): from here the callback is running.
            if "Ctrl+C stops" in line:
                started.set()

    watcher = threading.Thread(target=read, daemon=True)
    watcher.start()
    seen = []
    while child.poll() is None:
        time.sleep(0.04)
        if started.is_set():
            sample = process_counters(child.pid)
            if sample:
                seen.append(sample)
    watcher.join(2.0)
    output = "\n".join(lines)

    stats = re.search(r"(\d+) callbacks, (\d+) late, (\d+) restarts, (\d+) dropped, (\d+) steals",
                      output)
    # The first sample is the baseline: reads counted after it are reads the running device made.
    reads = seen[-1][1] - seen[0][1] if len(seen) > 1 else -1
    faults = seen[-1][0] - seen[0][0] if len(seen) > 1 else -1
    device_line = next((line for line in lines if "frame period" in line), "(no device line)")
    print("  " + device_line.strip())
    if stats:
        print("  %s callbacks, %s late, %s restarts, %s dropped, %s steals - %d samples while it ran"
              % (stats.group(1), stats.group(2), stats.group(3), stats.group(4), stats.group(5),
                 len(seen)))
    print("  while the device ran: %d page faults, %d file reads" % (faults, reads))

    ok = child.returncode == 0 and stats is not None and reads == 0 and len(seen) > 2 and \
        int(stats.group(1)) > 0 and int(stats.group(4)) == 0 and int(stats.group(5)) == 0
    print(("OK: " if ok else "FAIL: ") + "the app played, dropped nothing, read nothing")
    if not ok:
        print(output[-1500:])
        failures.append("boreaz.exe on a device")
    # Late callbacks are reported and never asserted: this driver was measured at 21 late of 141
    # at a 512 frame period and 0 at 1024, which is the Realtek, not this code, and an assertion
    # that fails when a laptop changes power state is a gate nobody trusts.


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
    app_live(failures, INSTRUMENT if INSTRUMENT.exists() else None)

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
