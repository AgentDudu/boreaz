# Boreaz

A lightweight audio/MIDI synthesizer for Windows: **88 keys, A0 to C8** — the full compass
of a real piano — with an on-screen keyboard, a switchable visualiser (transport strip or
note waterfall), `.mid` playback, and a sampled concert grand.

C++20, no DAW required. Runs standalone, takes a real MIDI keyboard, and can also render a
MIDI file to WAV from the command line.

## Status

Phases 0-5 and 7 of 12, plus the sampled-instrument machinery out of phase 6. **There is sound, and
there is a window.** A 16 voice synth engine plays the
full A0–C8 compass through a real WASAPI output and takes a MIDI keyboard; `--gui` opens 88
playable keys under a transport strip that sits still until you play and then rolls; and a `.mid` file
renders to a `.wav` without any device at all:

```bash
./build/boreaz --gui                            # the window: click keys, or play z s x d c v
./build/boreaz                                  # default output, wait for a keyboard
./build/boreaz --device 1 --period 256          # choose the output and the buffer
./build/boreaz --list-devices                   # this machine's outputs
./build/boreaz --list-midi                      # this machine's MIDI inputs
./build/boreaz-render tests/fixtures/midifile/pad-chords.mid build/pad.wav
./build/boreaz-render --golden tests/fixtures/midifile/sweep-88.mid
./build/boreaz tests/fixtures/midifile/pad-chords.mid          # play a file, exit when it ends
./build/boreaz --gui tests/fixtures/midifile/pad-chords.mid    # the same file, in the window
# One command, once: fetch the CC0 upright piano and decode it to PCM WAV (needs ffmpeg).
python tools/mkinstrument.py
./build/boreaz --gui --instrument third_party/instruments/upright-piano-KW/UprightPianoKW-20220221.sfz
./build/boreaz-render --instrument third_party/instruments/upright-piano-KW/UprightPianoKW-20220221.sfz \
    tests/fixtures/midifile/pad-chords.mid build/pad.wav       # the same instrument, as a file
./build/boreaz-sfz-probe third_party/instruments/upright-piano-KW/UprightPianoKW-20220221.sfz
```

The file reader is a library (`boreaz-midifile`), so the offline render, this app and the tests play
the same bytes through the same parser. A file plays once through and does nothing else: there is no
seeking, no loop and no open dialog, which are the controls the sequencer (phase 10) is going to
bring.

The sampled instrument is the other half of the same sentence. Give it an SFZ mapping and PCM WAVs —
plain SFZ spellings, both opcode names for everything, `<global>`/`<control>`/`<group>` defaults
inherited the way SFZ says (a group starts again from `<global>`, not from the group above it) — and
88 keys play recordings instead of oscillators: resident before the device opens, one read-head per
voice with pitch shifting, loops, the mapping's own gain and velocity depth, and no file, wave or
window include anywhere in the audio thread. That last part is a test, not a promise: the WAVs are
deleted underneath a loaded instrument and it still plays. A key the library never recorded replays
the nearest recording shifted, and says which. `--no-instrument` declines a library, and one that
will not load names the file, says it is playing the synthesiser instead, and keeps playing.

What `python tools/mkinstrument.py` fetches is the FreePats **Upright Piano KW** (CC0, recorded by
Gonzalo and Roberto, `github.com/freepats/upright-piano-KW`): 66 regions, **A0 to C8**, two velocity
layers that a soft and a hard touch really choose between, the bass looped, **109 MB resident,
150 ms to load**, and nothing written into the repository — the zip's sha256 is pinned, so the piano
that played in a test run is the piano the recipe says it was. A live 88-key glissando through it at
a 1024 frame period finished with **0 late, 0 xruns, 0 dropped, 0 steals**.

What is still missing from a real grand: filters (`fil_type`/`cutoff` are read, ignored, and warned
about, so a layer the recorder softened arrives bright), and `boreaz-mkinst`, which is the tool that
would subset the 394 MB Salamander grand into something playable. `trigger=release` is not one of
them any more: a region that asks to be played when the key goes up is loaded like any other and
voiced from **a pool of eight of its own, outside the sixteen**, because a hammer noise has to survive
the note it follows. It is picked, and played, at the velocity the key went *down* with — a
note-off arrives as velocity zero, so the engine remembers the strike — and the file's own
`amp_veltrack` still decides how much of that curve a hammer noise keeps, because that opcode is the
recorder's opinion about hammers: the same release region measured **0.0022 against 0.0293**
full-scale peak in the 30-100 ms after the key-up at velocity 20 against 110, thirteen times, which is
the note's own ratio. A library that wants its hammer noise at one level says `amp_veltrack=0`. It is one shot whatever the file says (`smpl loop` and
`loop_mode` are dropped on a release, since a key being let go happens once), it is looked up in a
list of its own so a note recording can never be mistaken for one, and the panic button stops it with
everything else. The starter instrument has no release regions at all, so on it a key-up is still the
amp release and nothing else. Nobody has heard any of it yet, either: every number above is objective,
and none of them is an ear.

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

Nothing is hardcoded and nothing is uploaded.

Every parameter the engine has is a line in a file and a slider in the window. `boreaz --gui` puts a
**Mixer** button next to the view switcher: 18 sliders, generated from the same table the file reader
uses, so the panel and `--config` cannot disagree and a new parameter appears in both by being written
down once. Sliders move the sound over about ten milliseconds rather than snapping to it, which is
what lets a cutoff be thrown open mid-phrase without a click, and a transpose retunes the notes that
are already held instead of only the next one. The instrument selector waits for the notes to stop
before it swaps a sample library in, because swapping rebuilds the voice pool.

```bash
cp config/params.example.ini mine.ini   # every key, its range, and the default it starts on
./build/boreaz --config mine.ini        # the defaults the app starts on
./build/boreaz --params solo.ini        # ... or one patch layered on top of them
./build/boreaz --help                   # the list of keys, straight from the table
```

An unknown key, a value that is not a number, and a number outside its range are errors that stop the
app with the line and the reason: a parameter file that quietly did nothing is indistinguishable from
one that worked. `config/params.ini` is the shipped preset and is read when `--config` says nothing.

Machine-specific values come from the environment, optionally backed by a local `.env` file:

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
