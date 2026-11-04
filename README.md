# Boreaz

**An 88-key piano for Windows, three commands from sound.** No DAW, no installer, no account, no
network while it runs. The whole compass of a real piano — **A0 to C8**, all 88 keys — as a
16-voice synthesiser, as a sampled upright you fetch with one command, and with a note waterfall you
can watch.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/boreaz --gui
```

A window opens with 88 playable keys. Click them or type at it. Sound comes out of the system default
output with nothing configured.

## What you get

| | |
|---|---|
| **88 keys, A0 to C8** | the lowest and the highest note a piano has, not a 49-key demo range |
| **Two sound engines** | a subtractive synth (16 voices) that is always there, and any **SFZ** mapping with WAVs beside it |
| **63 parameters** | each one a slider in the window's Mixer and a line in a text file, generated from one table, with a find box that answers `N of 63` |
| **Three views** | **Transport** (a strip that sits still until you play, then rolls), **Waterfall** (88 columns, time running up), **Spectrum** |
| **Your MIDI keyboard** | WinMM, hot-unplug safe: a cable pulled mid-song drops those notes and keeps playing |
| **.mid playback** | play, pause, stop, loop, and a position bar you click to seek; a song also works dragged onto the window |
| **Offline rendering** | the same song to a WAV with no audio device in sight |
| **Effects and modulation** | a distortion/drive blend, a stereo delay, a Schroeder room, and an 8-slot modulation router |
| **A patch browser** | 8 factory patches plus your own saves, each a plain text file you can diff |

Nothing is uploaded and nothing is logged. There is no telemetry to turn off — see
`docs/PRIVACY.md`.

## Playing it with no instrument

The letter rows are a two-octave piano laid out the way most software pianos do it, with the home row
as the sharps in between.

| keys | notes |
|---|---|
| `z s x d c v g b h n j m` | C up to B |
| `q 2 w 3 e r 5 t 6 y 7 u` | the octave above |
| `i 9 o 0 p` | middle C and up |

**Space** is the sustain pedal — held, not toggled, because that is the only thing a foot knows.
Typed notes arrive at a firm velocity rather than a maximum one. Everything else is the mouse, a real
keyboard, or the transpose control (±24 semitones, and it retunes notes that are already held).

## The sampled piano, in one command

```bash
python tools/mkinstrument.py     # fetch and decode the Upright Piano KW (needs ffmpeg)
build/boreaz --gui --instrument third_party/instruments/upright-piano-KW/UprightPianoKW-20220221.sfz
```

66 regions covering **A0 to C8**, two velocity layers that a soft and a hard touch really choose
between, and looped bass. **109 MB resident, 150 ms to load**, all before the audio device opens — and
it is CC0, public domain. It downloads into `third_party/`, so nothing instrument-sized is ever
committed to git. A live 88-key glissando through it finished with **0 late callbacks, 0 xruns, 0
dropped events, 0 stolen voices**.

A key the library never recorded replays the nearest recording it has, shifted, and says which. An SFZ
that will not load names the file, says it is playing the synthesiser instead, and keeps playing: an
instrument that fails must never mean silence. `boreaz-sfz-probe FILE.sfz` prints what a mapping says
before you commit to loading it.

## Shaping the sound

```bash
cp config/params.example.ini mine.ini    # every key, its range, and the default it starts on
build/boreaz --config mine.ini           # start from those defaults
build/boreaz --params patches/piano-mezzo-forte.txt   # or layer a patch on top
build/boreaz --gain -6                   # "it is too quiet", typed at the prompt
```

The Mixer panel is generated from the same table the file reader uses, so a new parameter appears in
the file and on screen by being written down once. Sliders move the sound over about ten milliseconds
instead of snapping to it, which is what lets a cutoff be thrown open mid-phrase without a click.
Eight modulation slots route LFO1, LFO2, a mod envelope, velocity, keytrack or the mod wheel to
cutoff, pitch, an oscillator, the hammer noise or the pan; unwired slots cost nothing measurable. The
FX bus is a distortion blend, a delay and a room, and it is a per-sample cost rather than a per-voice
one, which is why a heavy rack cannot blow the budget at high polyphony.

An unknown key, a value that is not a number, or a number outside its range stops the app with the
line and the reason — a parameter file that quietly did nothing is indistinguishable from one that
worked.

## Levels that behave

The engine opens at **-12 dB on purpose**: sixteen summed voices need headroom, and the shipped
default is a decision rather than an accident. `--gain` is therefore the flag for "too quiet" and "it
is clipping". It is the same knob as the master slider, from the same table, so `--gain 100` answers
`wants -72 to 12` and exits 2, and it outranks `--config`, which is the right precedence for a knob
being turned while something is already playing.

On the shipped instrument 0 dB measures **0.697** peak and +12 dB measures **0.998**: past about 0 dB
you are hearing the limiter, not more piano. At the top of the fader every committed fixture lands on
exactly the same peak, so the ceiling is a ceiling and not a limiter that occasionally taps. MIDI
**CC7** is the other hand on the level — per channel, linear (64 is half the amplitude), and a note
already held hears the move, which is what a volume wheel means.

## MIDI files

```bash
build/boreaz tests/fixtures/midifile/pad-chords.mid                # play it, exit when it ends
build/boreaz --gui tests/fixtures/midifile/pad-chords.mid          # the same file, in the window
build/boreaz-render tests/fixtures/midifile/pad-chords.mid pad.wav # no audio device involved
build/boreaz-render --golden tests/fixtures/midifile/glissando-88.mid   # measurements, not a file
```

In the window: drag a `.mid` onto it, or press **F1** (or `Change`) and type a path — a filter finds
the file in your songs folder or on another drive, and Enter loads it. A path that will not read
leaves the session exactly as it was and prints why. The file reader is a library
(`boreaz-midifile`), so the offline render, the window and the tests all play the same bytes through
the same parser. Notes outside A0–C8 are flagged `EXT` and played, not dropped.

## Numbers rather than promises

* **The CPU is metered by the app itself.** `boreaz.exe` prints the audio callback's own busy time as a
  fraction of one core. 16 sampled voices cost about **1.1 %**, or **1.4 %** with every effect on; the
  synthesiser costs **2.3 %**, which it can afford because the oscillators read a 16 KiB sine table
  built at compile time instead of calling `std::sin` twice per sample per voice.
* **No file I/O in the audio thread is a test, not a promise**: the WAVs are deleted underneath a
  loaded instrument and it keeps playing.
* **`tests/golden/` holds every fixture's rendered output as numbers**, so any change that moves the
  sound moves a golden and the test says by how much. Two files, because GCC and Clang differ by
  0.6 dB on one fixture and each compiler is compared against its own.
* `ctest --test-dir build --output-on-failure` runs several hundred cases and millions of assertions;
  the suite prints its own counts.
* `python tools/sanitize.py` builds ASan and UBSan and runs the suite under them, **and refuses to
  report a pass it did not perform** — which matters, because a Windows GCC without an ASan runtime is
  common enough that a green result there would be a lie.
* Quitting prints callbacks delivered, how many were late, how many times the device restarted,
  events dropped, voices stolen, and how far the audio thread got. With no keyboard it still opens the
  output and prints what it found, which is the usual first move when sound does not appear.
* `python tools/release_check.py build/boreaz.exe` asserts the shipped shape: the DLLs it imports, the
  levels it must not move, and 0 reads and 0 writes in the callback.

## Not built yet

So the gap is not a surprise:

* **SFZ filters.** `fil_type` and `cutoff` are read, ignored, and warned about. Measured on this
  instrument's softened layer, a 2.2 kHz 2-pole lowpass moves it 5 % in brightness and 0.1 % in level,
  so the warning is honest and the hole is small.
* **Waveform selection and band-limited oscillators.** `osc1`/`osc2` are sines and `osc3` is a saw; no
  patch can ask for a square or a triangle, and the saw is not band-limited.
* **The window needs an output device.** With no working output at all the app exits rather than
  drawing a silent panel.
* **Nobody has heard some of it, and nobody has used the mouse.** Every number above is objective and
  none of them is an ear: the velocity layers, the room, the limiter and the modulation are measured,
  not listened to, and no tester who was not the author has clicked Play or dragged a file.
* **No hosted CI.** The gates are the commands in this file, run by hand.

## One folder is the whole product

```text
boreaz.exe         3.97 MB, stripped, static compiler runtime
config/params.ini  2.8 KB of defaults
patches/           36 KB of factory patches
```

About 4 MB, and it imports only system DLLs, so nothing has to be installed on a Windows 10 or 11
machine: no installer, no administrator rights, and nothing written outside `%APPDATA%\Boreaz` — which
holds four remembered names (audio port, MIDI port, sample library, last patch) and no sound. A
missing `config/params.ini` stops the app and prints the absolute path it looked for.
`boreaz --version` and the file's own properties both say which build a machine has.

## Building it

Windows 10 or 11, CMake 3.25 or newer, Ninja, and GCC or Clang. Dependencies are fetched at pinned
versions on first configure, so that first configure wants internet.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
build/boreaz --help     # every flag and every parameter key, printed out of the same table
```

Exit status: 0 all good, 1 for anything that stopped the app (no such output, MIDI refused), 2 for bad
usage. Other tools: `listen.py` renders six named WAVs so a change can be heard rather than argued
about; `mkinst.py` cuts a playable subset out of the 394 MB Salamander grand (CC-BY-3.0);
`factory_patches.py` regenerates the shipped patches; `midifile_fixtures.py` regenerates the MIDI
fixtures; `key-in-window.ps1` drives the window's keys from outside, which is how the transport was
tested at all.

## Branches

`main` is the shipped tree: source, tools, tests, `config`, `patches`, `README`, `LICENSE`, `NOTICE`,
`docs/PRIVACY.md`. `develop` is where the work happens, and it additionally carries the working notes
for whoever picks this up next — a plan, handoffs, PR write-ups — which are not promoted. `main` is
built from `develop` by `tools/promote_main.py` and is never a merge of it. Comments that cite
`PLAN.md` point at `develop`.

## License

Boreaz's own source is under the **PolyForm Noncommercial License 1.0.0**: free to use, study, change
and share for non-commercial purposes; selling it or using it commercially needs a separate agreement
with the author. See `LICENSE`.

Bundled third-party code and the sampled pianos keep their own terms, listed in `NOTICE`. Neither is
covered by the non-commercial clause: the Salamander subset `tools/mkinst.py` cuts is CC-BY-3.0 and
stays freely reusable under CC-BY, and the Upright Piano KW that `tools/mkinstrument.py` fetches is
CC0 — public domain, no terms to keep.
