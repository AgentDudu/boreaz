# Boreaz

A lightweight audio/MIDI synthesizer for Windows: **88 keys, A0 to C8** — the full compass
of a real piano — with an on-screen keyboard, a switchable visualiser (transport strip or
note waterfall), `.mid` playback, and a sampled concert grand.

C++20, no DAW required. Runs standalone, takes a real MIDI keyboard, and can also render a
MIDI file to WAV from the command line.

## Status

Phases 0-3 of 12. **There is sound.** A 16 voice synth engine plays the full A0–C8 compass
through a real WASAPI output and takes a MIDI keyboard, with device and buffer selection from
the command line. No window yet (phase 5) and no sampled piano yet (phase 6). `PLAN.md` is the
contract for what is being built and in which order; phases 0-12 are listed there with exit
criteria.

```bash
./build/boreaz --list                   # this machine's outputs and MIDI inputs
./build/boreaz --demo --report          # hear it with no keyboard attached
./build/boreaz                          # default output, wait for a keyboard
./build/boreaz --device 1 --latency 256 # choose the output and the buffer
```

`--report` prints the counters the exit criteria are quoted against: delivered rate and period,
gaps between callbacks sorted by size, and engine CPU. Without a keyboard it still opens the
device and prints what it found, which is the usual first move when sound does not appear.

## Requirements

- Windows 10 or 11
- CMake 3.25 or newer, and GCC or Clang (this project is built with `ninja`)
- Internet access on first configure: dependencies are fetched at pinned versions

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/boreaz --version
```

Sound comes out of the default output with no configuration. If the wrong device answers —
or a virtual cable is in the list — `--device` picks by index or by part of the name, and
`--list` shows what is there. Exit status is 0 ok, 2 bad usage, 3 no such audio output, 4 MIDI
refused.

## Configuration

Nothing is hardcoded and nothing is uploaded. Machine-specific values come from the
environment, optionally backed by a local `.env` file:

```bash
cp .env.example .env     # .env is git-ignored; every key in the example is empty
```

See `docs/PRIVACY.md`.

## License

Boreaz's own source is licensed under the **PolyForm Noncommercial License 1.0.0** — free to
use, study, change and share for non-commercial purposes; selling it or using it commercially
needs a separate agreement with the author. See `LICENSE`.

Bundled third-party code and the sampled piano keep their own terms, listed in `NOTICE`.
The piano (Salamander Grand Piano, CC-BY-3.0) is *not* covered by the non-commercial clause:
it stays freely reusable under CC-BY.
