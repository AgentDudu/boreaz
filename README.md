# Boreaz

A lightweight audio/MIDI synthesizer for Windows: **88 keys, A0 to C8** — the full compass
of a real piano — with an on-screen keyboard, a switchable visualiser (transport strip or
note waterfall), `.mid` playback, and a sampled concert grand.

C++20, no DAW required. Runs standalone, takes a real MIDI keyboard, and can also render a
MIDI file to WAV from the command line.

## Status

Phases 0-5 of 12. **There is sound, and there is a window.** A 16 voice synth engine plays the
full A0–C8 compass through a real WASAPI output and takes a MIDI keyboard; `--gui` opens 88
playable keys with a readout of what the audio thread is doing above them; and a `.mid` file
renders to a `.wav` without any device at all:

```bash
./build/boreaz --gui                            # the window: click keys, or play z s x d c v
./build/boreaz                                  # default output, wait for a keyboard
./build/boreaz --device 1 --period 256          # choose the output and the buffer
./build/boreaz --list-devices                   # this machine's outputs
./build/boreaz --list-midi                      # this machine's MIDI inputs
./build/boreaz-render tests/fixtures/midifile/pad-chords.mid build/pad.wav
./build/boreaz-render --golden tests/fixtures/midifile/sweep-88.mid
```

The file reader is a library (`boreaz-midifile`), so the in-app transport plays the same bytes
through the same merge and the same parser in phase 10. No sampled piano yet (phase 6), and the
window does not load a `.mid` file yet either — playing a file is the in-app sequencer's job.
`PLAN.md` is the contract for what is being built and in which order; phases 0-12 are listed
there with exit criteria.

Quitting prints the counters every claim about the engine is argued from: callbacks delivered,
how many were late, how many times the device restarted, events dropped, voices stolen, and how
far the audio thread has got. Without a keyboard it still opens the output and prints what it
found, which is the usual first move when sound does not appear.

## Requirements

- Windows 10 or 11
- CMake 3.25 or newer, and GCC or Clang (this project is built with `ninja`)
- Internet access on first configure: dependencies are fetched at pinned versions

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/boreaz --help
```

Sound comes out of the default output with no configuration. If the wrong device answers —
or a virtual cable is in the list — `--device` picks by index or by part of the name, and
`--list-devices` shows what is there. Exit status is 0 ok, 1 for anything that stopped the app:
bad usage, no such audio output, MIDI refused.

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
