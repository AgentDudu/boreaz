# Boreaz — lightweight C++ audio/MIDI synthesizer

Status: planning approved, nothing implemented.
Decisions below are settled unless a later phase proves them wrong; change them in this file first, not in code.

## 1. What it is

A small desktop instrument for Windows: it opens an audio output device, takes MIDI in
(HW keyboard / DAW / loopback), plays a polyphonic subtractive synth across a **full
88-key piano range (A0–C8)**, plays `.mid` files so it can be heard with no keyboard
attached, and shows a window with knobs. Real-time, low latency, CPU-cheap.

Settled while planning: **16 voices**, latency "feels instant" (~8–12 ms on WASAPI
shared, we print the real number), **mono voices into a stereo FX bus**, **no zones**,
`.mid` **playback is a feature**, GUI is **keyboard along the bottom (all 88 keys, always
full width) → switchable visualiser panel → slim transport strip that never disappears**,
and **three views in that panel: transport, note waterfall, and a log spectrum of the master
output**.
Licence: **PolyForm Noncommercial 1.0.0** (decided, see §7). **Boreaz is also a sample player
for a real recorded grand piano** — that decision and its consequences are in §1 "The piano
(sampled)".

### Non-goals (explicit, so we do not build them by accident)
- No plugin formats (VST3/AU/CLAP). No JUCE.
- No modular patch-cable graph. Fixed signal path with a small modulation router.
- No sample **recording**, and no sampler in the "play your own audio" sense. Samples are
  used for one shipped instrument (§1, The piano), not as a general feature.
- No audio recording. `.mid` **playback** is in scope; sequencing, editing, arranging,
  and multi-track playback are not.
- No Linux/macOS work up front — the stack we chose is portable, so we only *avoid
  Windows-only code*, we do not test elsewhere until the optional phase.
- No presets bank format compatibility with anything (no .fxp/.fxb/.tap).
- No 128-key range. The *instrument* is 88 keys; MIDI notes outside it are handled, not
  hidden (see below).

### Keyboard range: 88 keys, A0 → C8

The instrument range is the standard 88-key piano compass. This is a hard spec, so the
numbers are written down once here and everything else refers to them.

| Key | MIDI note | Frequency | Meaning |
|---|---|---|---|
| A0  | 21  | 27.500 Hz | bottom of the instrument; lowest fundamental we must keep clean |
| C4  | 60  | 261.626 Hz | middle C; reference for key-tracking, pan spread, GUI centre |
| A4  | 69  | 440.000 Hz | tuning reference, configurable 415–466 Hz |
| C8  | 108 | 4186.009 Hz | top of the instrument; worst aliasing case |

- **88 keys = 87 semitone steps = 7 octaves + a minor third** (108 − 21 + 1 = 88).
- Convention: **C4 = MIDI 60**. State it in code comments and print note names with the
  octave number, because the "middle C = C3" vs "C4" vs "C5" argument has wasted real
  hours. One `note_name()` helper is the only place naming happens.
- `note = 69 + 12·log2(f/440)`, rounded, clamped to the 0–127 MIDI range. Never clamped
  to 21–108.
- Engine **accepts the full 0–127 MIDI range**. Notes outside A0–C8 still sound (useful
  for bass drops, effects, and a global transpose control), but the GUI marks them
  `EXT` and shows them in a dimmed part of the keyboard. We do not throw away MIDI data.
- Global `transpose` (±24 semitones) and `octave` (±4) params shift the *played* key,
  so a 49-key or 25-key controller covers all 88 without a settings dialog.
- Tuning is 12-TET with a 128-entry `freq = ref·2^((n−69)/12)` lookup table, computed
  once at init in double precision, stored as `float`.
- The GUI shows **all 88 keys at once, permanently** — no windowing, no scrolling. That is
  what lets visualiser lanes be 1:1 with keys. Cost: a wide-ish window (~19 px per white
  key at 1400 px). Benefit: less code than a scrolling keyboard would need.
- Waveforms must survive the whole compass: a saw at C8 has ~5 harmonics below Nyquist
  at 44.1 kHz, so band-limiting is required for the top octave and is cheap there —
  polyBLEP in phase 8 covers it, no wavetable needed.

### GUI layout

Three bands, top to bottom. Only the middle one switches.

```
┌───────────────── boreaz ─────────────────┐
│ [Waterfall] Transport      ♪ demo.mid ▶■ │  transport strip — always visible
│ ┌──────────────────────────────────────┐ │
│ │      ║        ║                      │ │  switchable visualiser panel
│ │   ║  ║        ║      ║               │ │  (one enum + one table, src/gui/viz/)
│ │   ║  ║   ║    ║      ║   ║           │ │  notes rise out of the keyboard line
│ └──────────────────────────────────────┘ │
│ ▁▂▃▄▅▆▇█ 88-key keyboard  A0 ────── C8  │  all 88 keys, always full width
└──────────────────────────────────────────┘
```

- Lanes in the panel sit **1:1 above the keys**, which is the only reason the keyboard is
  never windowed.
- The spectrum uses that same geometry sideways: its band edges come from `KeyboardLayout`, so a
  band sits above the key that plays it. One coordinate system above the keys, not two that
  disagree.
- A spectrum band is a frequency range, **not a pitch**. The semitones at the bottom of the compass
  are 1.6 Hz apart at A0, which is closer than any window a display can afford to wait for, so the
  lowest octave and a half read as a rising shape rather than as separate notes. The tooltip says
  what a band is instead of the panel pretending.
- The transport strip never hides, because the waterfall has no controls in it; play/stop
  must stay reachable while you watch notes.
- Note: with the chosen source (snapshot + display tap) notes **scroll up from the
  keyboard line**. They do not fall in from the top like Synthesia, because only the `.mid`
  timeline knows the future. Labelled for what it is, not sold as something else.

### The piano (sampled)

Synthesis cannot reach "sounds like a real piano", so the flagship sound is a recording of
one. Chosen instrument, verified against its own pages (not from memory):

**Salamander Grand Piano v3** — Alexander Holm, Yamaha C5, two AKG c414 in AB ~12 cm above
the strings, 48 kHz/24-bit, **16 velocity layers**, sampled **in minor thirds from the
lowest A**, hammer-noise releases chromatic ×1, string-resonance releases in minor thirds
×3. **CC-BY-3.0** → redistribution and commercial use allowed with attribution.
Sources: `freepats.zenvoid.org/Piano/acoustic-grand-piano.html`,
`sfzinstruments.github.io/pianos/salamander/`, `github.com/sfzinstruments/SalamanderGrandPiano`.

**The first instrument that ships is not that one.** It is **FreePats Upright Piano KW** — a
Kawai upright, recorded January 2017 by Gonzalo and Roberto with a Zoom H1, **CC0 1.0,
dedicated by the recorders themselves** (`LICENSE`, `github.com/freepats/upright-piano-KW`,
`freepats.zenvoid.org` — where the release page still says how to reach them): 66 regions, 2 velocity layers, stereo,
bass notes looped. **Reading the release rather than this plan corrected one line here:** the
release has two builds, FLAC (32.4 MB) and OGG (2.2 MB), and **no WAV build at all** - so the
starter asset needs one offline decode to PCM, which is what `tools/mkinstrument.py` is for, and
nothing about that changes the runtime rule that the app opens no decoder. Reading the file rather
than the
brochure, the whole instrument uses **13 opcodes** — `sample lokey hikey pitch_keycenter
loop_start loop_end loop_mode fil_type cutoff lovel hivel key ampeg_release width`, no
`<control>`, no ARIA, no smart variants. Salamander's 16 layers *are* ARIA smart variants, so
playing it means teaching `mkinst` to flatten them into `lovel/hivel` before any runtime reader
can; that is real work and a demo should not wait for it. Two more sets were looked at and
rejected: **Splendid Grand** ("public domain by AKAI" is a third-party claim with no licence
text, and its own README says it only plays correctly in ARIA samplers) and **Salamander's own
repo**, whose `LICENSE` is CC-BY-3.0 while the author's page says "as of 4.3.2022 this is now
public domain" — the conflict is why we still credit it if we ever ship it.

| Item | Decision |
|---|---|
| Upstream build | **Salamander v3, but the size in this row was wrong for a year.** The original 44.1 kHz / 16-bit build is a **488.7 MB** `.tar.bz2` (its WAVs total ~394 MB, which is where the number in this row came from), and the build `mkinst` will actually read is the 48 k/24 FLAC tree of `sfzinstruments/SalamanderGrandPiano`, **ordinary git blobs, not LFS**, fetched one file at a time by `tools/mkinst.py` (748.4 MB for all 641 recordings, 184 MB for the subset it builds by default) so only the subset is ever downloaded. Sizes, digests and both URLs are in §7 |
| Starter asset | **FreePats Upright Piano KW**, FLAC build (32.4 MB) fetched and decoded by `tools/mkinstrument.py` against a pinned sha256: CC0, no attribution required, no 394 MB fetch, plain-SFZ mapping of 13 opcodes. This is what `--instrument` plays first |
| Shipped asset (flagship) | **Built 2026-11-01 by `tools/mkinst.py`, and the size this row asked for is gone: 4 of the 16 velocity layers (v1, v6, v10, v14) plus all 88 chromatic key-off recordings = 208 recordings, 147.3 MB of mono 16-bit WAV, 280 MB resident, 120 playable regions + 88 releases, 0 refused, ~480 ms to load.** ARIA smart variants are not learned, they are dropped: the subset keeps the natural tuning and re-cuts the four bands to cover velocity 0-127. Loops are carried by the mapping, which wins over `smpl` (see §7). Why 280 MB and not ~100 MB is written into §1's piano section and §7's instrument row |
| Format | **SFZ read at runtime** (plain text, both good libraries ship it). No home-grown format, no converter |
| In git | **Nothing binary.** The 748 MB upstream tree, the 147 MB subset and the 280 MB it becomes in RAM never enter the repo; only the `mkinst` recipe (text) is committed — and `third_party/` is gitignored, so a PR can show the tool and never the instrument it built |
| Distribution | Subset published as a release artifact, fetched/built on demand |
| Attribution | `NOTICE.txt` in the zip + credit in the about box: author, title, link, **and a statement that we modified it** (CC-BY-3.0 "indicate changes") |
| CI test asset | A **synthetic SFZ + generated sine samples** fixture, so tests cover the reader without downloading licensed audio |

Two consequences that are easy to get wrong:

- **Never apply stretched tuning or inharmonicity to samples.** A recorded piano is already
  stretched by the tuner; adding our own makes it wrong twice. `Tuning` stays linear 12-TET
  for this instrument. Minor-third spacing is handled per region by `pitch_keyroot`, which
  means up to ±1.5 semitones of resampling — hence the high-quality-interpolation decision.
- **Outside A0–C8 there is no sample.** Those keys pitch-shift the nearest edge sample and
  stay flagged `EXT`; they do not silently fall silent.
- **The same opcode has two spellings, and the two instruments on disk use different ones.**
  `pitch_keycenter` (SFZ 2.0) and `pitch_keyroot` (ARIA alias) are one thing, and
  `loop_start/loop_end/loop_mode` are the 1.0 spellings of `loopstart/loopend/loopmode`. The
  reader accepts both and stores one; a reader that only knows the 2.0 names reads the FreePats
  instrument as 66 regions with no loops and no tuning reference.

**Evaluated and rejected as the instrument:** `github.com/Leethring/piano-sound-samples`
(MIT). Measured: 136 files of **128 kbit/s stereo MP3 rendered by MuseScore** — a soundfont
render, not a recording — **one** velocity layer, no release samples, ~5 s regardless of
pitch (top octave *longer* than the bass, so the decay is a written note value), and a 5.5 dB
per-key level spread. Fine as throwaway test data; it cannot answer "sounds like a piano".

## 2. Stack

| Concern | Choice | Why |
|---|---|---|
| Language | C++20, no exceptions on the audio path | toolchain present: GCC 13.2 / Clang 23 |
| Build | CMake 3.29 + `FetchContent`, pinned tags | no extra tooling for the user |
| Audio I/O | **miniaudio** (WASAPI, WASAPI shared, DSound) | one MIT header pair, device mgmt + resampling done |
| MIDI I/O | **winmm directly**, ~200 lines in `io/midi_input.cpp` | planned as miniaudio `ma_midi`, and **phase 3 found that API does not exist** — the miniaudio header mentions MIDI once, in a comment about macOS Audio MIDI Setup. RtMidi stays the fallback if virtual ports are ever wanted |
| GUI | **Dear ImGui + GLFW + OpenGL3** | immediate mode, zero state, knobs/meters in an afternoon |
| Tests | **doctest** (single header) | fast unit tests + offline render assertions |
| `.mid` parsing | `midicpp` (header-only) — **licence + C++20 support unverified, re-check before use**; fallback `minimidi` | needed by both the offline renderer and in-app playback |
| Instrument | **FreePats Upright Piano KW** (CC0-1.0, plain SFZ) first; **Salamander Grand Piano v3** as the flagship, built by `tools/mkinst.py` as a **4-layer mono subset: 147 MB of WAV, 280 MB resident** (CC-BY-3.0) | a real recorded piano is the only route to "sounds like a piano", and the CC0 one is the one that can be fetched, tested and shipped today |
| Instrument format | **SFZ subset reader**, plus `boreaz-mkinst` to subset upstream → shipped asset | SFZ is plain text; a declared opcode subset beats owning a format *and* a converter |
| Sample decode | **None at runtime.** `mkinst` normalises to PCM WAV once, offline | no decoder in the audio path and no decode at startup |
| Presets | hand-rolled `key = value` text files | params are a flat block; a JSON lib is 25k lines for this |

Fallbacks if a choice breaks: RtAudio + RtMidi instead of miniaudio; `midicpp` or
`minimidi` for `.mid` parsing (verify licence + C++20 support before vendoring).

## 3. Architecture

```
                 ┌──────────── UI thread (GLFW/ImGui) ────────────┐
                 │  knob widgets → Params (plain struct)          │
                 │  meters / CPU% ← atomics read-only             │
                 └───────────────┬────────────────┬───────────────┘
                       param swap │                │ note events
                     (atomic ptr) ▼                ▼  SPSC ring
   .mid sequencer ─┐
   (audio clock)   ├──► EventQueue ──►   AUDIO THREAD (miniaudio callback)
   MIDI in ────────┘      lock-free ┌──────────────────────────────────┐
   (winmm)
                                    │ per block (64 frames):           │
                                    │  drain events → note on/off      │
                                    │  VoiceAllocator (pool of 16)     │
                                    │   └ Voice = Synth | SampledVoice │
                                    │           → mix (synth path)     │
                                    │           → filter (SVF, multi-  │
                                    │             mode, resonant)      │
                                    │           → ADSR amp             │
                                    │  FX bus: distortion, delay,      │
                                    │          reverb, master, limiter │
                                    │  hard clip → output (float32)    │
                                    └──────────────────────────────────┘
```

`Voice` is a closed set of **two** kinds behind one interface, and nothing else:

- **SynthVoice** — osc×3 + noise → mix → SVF → ADSR amp. Everything in phases 1–2.
- **SampledVoice** — read head into a resident `SampleBuffer` + cubic resampler + loop +
  gain envelope + pan. No filter, no modulation matrix. Cheaper per note than a synth voice.

Both are allocated from the same 16-voice pool, so stealing, panic, sustain and the display
tap behave identically whichever is playing. Release samples (`trigger=release`) live
outside the pool, because they must outlive the voice that spawned them.

Three data paths, deliberately separate:
1. **Params** — UI owns a `Params` struct and publishes a copy through a handoff the audio
   thread tries once per block and never waits on; a failed try costs one block of the
   previous parameters, which is one frame of slider lag. An atomic pointer swap was the
   original wording, and it is what the diagrams still show, but a swap has to free the old
   block while the audio thread may be reading it. `engine/param_bus.h` records why the
   copy-based lock-free variants (two slots and a claim, three slots) were rejected: both
   either lose the newest publish or can recycle a slot mid-copy. No locks in the sense this
   rule means — the audio thread is never blocked, and the try is uncontended at 60 fps.
2. **Note events** — one SPSC ring (`Event{tick, type, chan, note, vel}`) fed by MIDI
   callback, the `.mid` sequencer, the offline renderer, and the GUI "test note" buttons.
   Same code path for all four; the sequencer is just another MIDI source, which is what
   keeps playback cheap.
3. **Telemetry** — atomics (voice count, CPU%, clip counter) for scalars, plus one
   **display-only tap**: a fixed ring of note-on/off edges and a 16-slot voice snapshot
   (`note, velocity, gate age`) written *after* processing, read by the UI at frame rate.
   Drop-on-full, allocation-free, and physically unable to delay the audio path.

### Real-time rules (enforced, not suggested)
- No malloc, free, lock, file, or logging inside the audio callback.
- All buffers preallocated at device-open; pool is fixed size.
- Process in blocks of 64 frames; filter coefficients + LFO increments updated once per
  block, never per sample.
- Flush-to-zero + denormals-off on the audio thread.
- Every oscillator is band-limited-ish (polyBLEP) or the demo sounds cheap; naive `saw()`
  is allowed only in the phase where it gets replaced, marked with a `shortcut:` comment.
- Master limiter is the last thing before output; nothing may pass ±1.0.
- Note frequency comes from the `Tuning` LUT, never from `powf`/`pow` in the audio path.
- Filter coefficients are computed in `double` and stored as `float`. At A0 with high
  resonance a `float`-only biquad rounds its own poles and drifts or buzzes; that is a
  bug we will otherwise blame on the compiler.
- Per-sample pitch comes from a 32-bit phase accumulator (resolution ≈ 1e-5 Hz at
  44.1 kHz — far below the 0.5-cent test tolerance even at A0).
- The display tap never blocks and never grows: if the UI lags, frames are **dropped**,
  not queued. A visualiser that costs an audio glitch is a bug, not a tradeoff.
- Sample data is **fully resident before the device starts**: no file handle, no mmap
  page-in, no decode, no allocation in the audio path. Startup may be slow; the callback
  may not.
- **Counted 2026-11-01, so the bullet above is a number and not a hope.** `boreaz-render
  --counters` reads the process's own page-fault and IO counters in a window around the engine
  call — the same `Engine::render_chunk` the device callback calls, on the thread that renders —
  and reports three phases: loading, the first 32 blocks, and every block after them. Loading the
  141 MB flagship costs **104 196 faults and 209 file reads**; rendering an 88-key sweep through it
  afterwards costs **0 file reads and 0 file writes**, which is the half of the promise that was
  ever a promise. The page-fault half was never literally true: across the four committed fixtures
  on both engines the post-warm-up count is **0–14 soft faults per render** (the worst of 27 renders;
  the sweep is 3 058 blocks, so even the worst is 0.5 % of blocks, reads still 0), which is the
  process first touching its own memory and not disk — 2 faults on one run and 14 on the next proves
  it is noise, not a path. So §3
  forbids exactly what is now asserted: **no file I/O in the callback**, and `tools/release_check.py`
  fails if reads or writes ever go nonzero, or if the fault count reaches four digits, which is what
  a real page-in or a leak would look like.
- **And the same counters around a real device, from the gate.** `boreaz.exe` on a MIDI file stops
  when the file's tail has rung out, so a script can run the app itself; `release_check.py` does and
  samples the process's own counters from outside between the line the app prints after the device
  starts and its exit. Measured on this machine (Realtek, VB-Cable, 48 kHz, 1024 frame period, the
  58 MB starter library resident): **72 callbacks, 0 late, 0 dropped, 0 steals, 0 file reads, 151
  page faults**. Those 151 faults are the whole process, driver threads included, and they are soft
  — reads stayed at 0 — which is the honest shape of the claim: a live callback does take page
  faults and is still never late, because none of them are waiting for a disk. Re-measured from the
  app on the flagship, 88-key glissando, the Realtek speakers: **0 late of 833 callbacks at a 1024
  frame period; 256 late of 1665 at 512**, 0 dropped, 0 steals both times — which reproduces the
  phase 6b figures on a build that is not four phases old, and is the driver rather than the engine
  for the reason already given there.
- Preload is capped by an instrument budget, and the flagship now sets its real size: **280 MB
  resident, 208 files, ~480 ms** (measured 2026-11-01, `boreaz-sfz-probe`; the CC0 starter is
  109 MB and 205 ms). Print resident MB and load time at startup, and refuse to start quietly when
  the asset blows the budget. `kMaxResidentBytes` is the refusal, and it is well above 280 MB.
- Per-voice resampling is **cubic at minimum** — the library is minor-third spaced, so most
  keys are pitch-shifted up to ±1.5 semitones. Linear interpolation is audible on a grand.

### Directory layout
```
include/boreaz/        public headers (engine types, Params, Event)
src/dsp/               osc, filter, adsr, math, waveshaper, analyser (window/FFT/log bands)
                       (no I/O, no alloc)
src/engine/            Voice, VoiceAllocator, Engine, Params, EventQueue, Tuning (128-entry LUT)
src/io/                miniaudio device, MIDI input, ring
src/gui/               ImGui app, widget pages
src/gui/viz/           panel switcher + one file per visualiser (transport, waterfall, spectrum)
src/app/               main, CLI flags, wiring
src/offline/           MIDI-file → WAV renderer (test/tool target)
src/midifile/          .mid parser static lib (shared by offline + seq)
src/sfz/               SFZ subset reader: text parse + region inheritance + opcode subset
src/samples/           SampleBuffer store (resident), SampledVoice, cubic resampler, release pool
tools/mkinst.py        offline subsetter: Salamander → 4-layer instrument + NOTICE (built 2026-11-01)
src/seq/               transport, sample-clock, tempo map → event ring
tests/                 doctest units, golden .wav fixtures
third_party/           empty by design — FetchContent downloads elsewhere
.env.example           every BOREAZ_* key, empty values, no machine data (.env stays ignored)
```
Dependency direction: `app → {gui, io, seq, offline} → engine → {dsp, samples, sfz}`;
`midifile`, `samples` and `sfz` are leaves that never include engine types, and `mkinst`
depends on `sfz` alone. `dsp`, `samples` and `engine` never include miniaudio, GLFW, ImGui,
or the parser. That is what keeps the engine testable headless.

## 4. Phases

Each phase ends with a green build, a merged PR into `develop`, and something you can
hear or measure. Exit criteria are the contract; do not start the next phase until they
pass. Phases never appear in commit messages — branch name + PR carry that.

| # | Branch | Deliverable | Exit criteria |
|---|---|---|---|
| 0 | `feature/project-scaffold` | CMake project, FetchContent pins, dir tree, `.clang-format`, `.gitignore`, `.env.example` (empty values only), warnings-as-errors, empty `boreaz` exe linking miniaudio/GLFW/imgui/doctest | clean build with GCC **and** Clang; `boreaz --version` prints; `ctest` runs 1 trivial test; a hygiene gate (`tests/test_hygiene.cpp`, run by `ctest`) fails the build on a machine-absolute path, a committed `BOREAZ_*` value carrying real data, or an embedded credential or token; assistant/model names are a review rule, not a regex |
| 1 | `feature/dsp-primitives` | `Tuning` 128-entry LUT + `note_name()`, `Oscillator` (sine/saw/square/tri, phase accumulator), `SVF` filter (LP/HP/BP, resonance, per-block coeffs, double-precision coeffs), `ADSR` (accurate exponential-ish, retrigger-correct), `Gain`/`SoftClip` | unit tests: freq accuracy < 0.5 cent **at A0, C4, A4 and C8** (not 440 Hz alone); LUT holds 27.5 Hz and 4186.009 Hz and is monotonic over all 88 keys; `note_name(21)=="A0"`, `note_name(60)=="C4"`, `note_name(108)=="C8"`; ADSR segment durations ±1 sample; filter passes DC at 0 resonance and stays stable/bounded at A0 + max resonance; no NaN/denormal after 10⁶ samples |
| 2 | `feature/voice-engine` | `Voice` = 3 osc + noise → mix → SVF → ADSR amp (one class here; phase 6 splits synth/sampled behind it, so keep the boundary clean). `VoiceAllocator`, pool of 16, steal oldest+quietest. Note on/off, velocity, pitch bend, sustain, all-notes-off, transpose/octave. `Engine::process(block)` | render 16-note chord + held pedal: exactly 16 voices, no stuck notes after panic, output peak ≤ 1.0, deterministic (same MIDI + params → byte-identical WAV); **full 88-key chromatic sweep A0→C8 renders with no silence, no NaN, and peak ≤ 1.0 on every key**; notes 0–20 and 109–127 are accepted and reported as extended range |
| 3 | `feature/live-audio-midi` | miniaudio device open/choose, winmm MIDI input (there is no `ma_midi`, see §2), SPSC event ring, real-time safety pass, CLI flags (`--device`, `--midi`, `--list`, `--latency`), auto-restart on device loss | real keyboard plays with no audible glitch at 512 samples period; CPU < 3% of one core; unplug/replug MIDI does not crash or leak voices |
| 4 | `feature/offline-render` | `boreaz-midifile` static lib (parser + tempo/time-sig/event merge, shared with the app), `boreaz-render` tool: `.mid` + params file → `.wav`; golden-file fixtures for engine states | **phase 4 outcome: no parser dependency was taken.** The re-check came back "thin, winmm-shaped, and we would wrap all of it anyway", so `boreaz-midifile` is ours: no licence to audit, no C++20 question, one fewer thing that breaks on someone else's Tuesday. Multi-track (format 1, tempo change living in another track) and tempo-map files render; renders are byte identical across runs and across processes; the four fixtures (mono lead, pad, bass, **88-key chromatic glissando A0→C8**) are committed with a recipe — `python tools/midifile_fixtures.py --verify "boreaz midifile fixtures"` proves what is on disk is what the recipe says — and `tests/golden/midifile.txt` holds their measurements |
| 5 | `feature/gui-shell` | GLFW window + ImGui context inside the audio-coupled loop; device/audio status; CPU% + voice + clip meters; **on-screen keyboard with all 88 keys, A0–C8, always full width**, middle C marked, `EXT` keys dimmed, feeding the *same* event ring; **switchable visualiser panel** = one enum + one table, with the transport view as the first entry. **phase 5 outcome: the window is up and the panel is a table.** `FrameView` is the only thing a visualiser reads, so transport, waterfall and piano roll are three files with no engine include and no view of the audio thread. Keyboard geometry is pure (`KeyboardLayout`) and every one of the 88 keys is asserted clickable at 1280×720 and 1920×1080 at content scales 1.0/1.25/1.5/2.0, with C-labels and octave numbers that never overlap a key boundary. The `.ini` loader the render tool already had is now a library (`boreaz_config`), so the window reads the same file under the same rules instead of growing a second parser. The transport clock takes its zero from the first note or loaded file and not from the device opening, and `Backspace` sets it back to zero - a clock you cannot set back is a stopwatch you cannot use twice - until the transport has controls of its own to put it in. What a headless session cannot prove — 60 fps at the desk, a click that sounds, an ear test — stayed unproven rather than being called done | window at 60 fps while audio runs; all 88 keys individually clickable with no scrolling or paging; adding a visualiser costs one enum value + one file; closing window exits cleanly with device stopped |
| 6 | `feature/sample-playback` → `feature/instrument-asset` | **Split in two:** `feature/sample-playback` = the SFZ subset reader, the resident `SampleBuffer` store, the sampled voice behind `Voice`, and the synthetic fixture that proves them with no audio in git; `feature/instrument-asset` = the CC0 instrument fetched, subset, wired to `--instrument`, and audible. **phase 6a outcome: the machine is built and the instrument is not.** `src/sfz/` parses `<control>`/`<group>`/`<region>` with inheritance into one `Region` struct, accepts both spellings of every opcode name, keeps the whole map, and refuses only what cannot be honoured - `trigger=release` and `loop_mode=sustain` regions, named and counted, never dropped in silence. `SampleStore` decodes PCM WAV (16/24/32-bit int, 32-bit float, mono or stereo, `smpl` chunk read but never trusted over the mapping's own numbers) fully resident before the device opens, and is itself the `engine::Instrument`, so `engine/` never includes `samples/`. A key with no region of its own **replays the nearest recording shifted**, because 88 keys must make a sound and a silent key is a worse bug than an honest shift; a mapping with velocity layers is accepted and **warned about in one counted line**, since playing one layer at every velocity is a missing feature that would otherwise arrive as a bad piano. `Voice` is now the interface phase 2 left room for, and a pool is one kind of voice, chosen once on the setup path. `--sfz` plays the library in both the app and `boreaz-render` - the render tool, which is audio in a file, so the sampled path can be heard with no driver and no download - and `boreaz-sfz-probe` prints the map, the refusals and the warnings. What is left here: `trigger=release` and its second pool, a real velocity choice rather than one recording, `boreaz-mkinst`, and the asset itself. `src/sfz/` reader: `<control>`/`<group>`/`<region>` inheritance over a **declared opcode subset** (`lokey/hikey`, `lovel/hivel`, `pitch_keyroot`, `offset`, `loopstart/loopend/loopmode`, `volume`, `tune`, `amp_veltrack`, `trigger=release`, `group`). `SampleBuffer` store fully resident at open; `SampledVoice` = read head + cubic resampler + loop + gain; `trigger=release` regions served from a separate small pool; velocity crossfade between layers; `boreaz-mkinst` subsets Salamander 44.1/16 → mono, ≥ 4 layers, ~100 MB, emitting `NOTICE.txt` from the pinned recipe; `--instrument file.sfz`, `--no-instrument` | the subset plays **A0→C8 with audible dynamics**; a synthetic SFZ fixture (generated sine samples) covers the reader in unit tests so **CI never downloads licensed audio**; **zero file I/O or page faults in the callback** (no handle open while processing, glitch counter 0 across a full 88-key sweep); held notes sustain **past 5 s** through a loop seam with no click; note-off fires a release sample and the main voice still dies on time; unknown opcodes warn once and never abort a load; resident MB + load time printed and inside budget. **Measured on 6a:** 209 tests, every one of them sine-generated; an 88-key sweep finds a region for every key and no two keys play the same recording; the same `.mid` rendered sampled and synthesised gives different peaks (0.30 against 0.60) and a lead window measuring the file's own pitch where the sawtooth measures its harmonics; a looped region holds a bounded amplitude for 10 s and dies on release; a mapping whose file is missing names the file and exits nonzero from both tools. Not measured: page faults and glitch counts against a real device, and the 100 MB load time - all of which need the asset. **phase 6b outcome: the CC0 instrument is fetched, decoded, wired, and audible, and it found a bug in the reader.** `tools/mkinstrument.py` pins the release zip by sha256, decodes the 66 recordings to 16-bit PCM WAV with ffmpeg (the release ships FLAC/OGG and no WAV, so this one offline step is the whole price of "no decoder at runtime"), rewrites only the `sample=` names, carries `LICENSE` and `README.md` along, refuses a zip whose hash moved, and never writes inside the repository (`third_party/`, ignored). The instrument loads as **66 regions spanning 21-108 - A0 to C8 - 109 MB resident, 150 ms to load, 0 regions refused**, and its two velocity layers now work, which they did not: `<group>` used to inherit from the `<group>` above it, so a file whose four groups say `hivel=80` / `lovel=81` read as lovel 81 above hivel 80 and the entire piano was refused. Groups start from `<global>` again, which is SFZ's own rule and is now a test with the shape of the file that found it. Keys whose layers leave a velocity uncovered are counted and warned about once, and a mapping whose two bands cover the range produces no warning at all - both cases are tested. `--sfz` is `--instrument`, `--no-instrument` declines it, and a path that will not load **names the file, says it is playing the synthesiser instead, and keeps playing**: an instrument that will not load must never mean silence. Measured live against the real device with an 88-key glissando through the recorded instrument: at a 1024 frame period **0 late, 0 xruns, 0 dropped, 0 steals**; at 512, 256 late of 1665 - and the synthesiser on the same file at the same period is **256 late of 1666**, so the recorded voices cost what the oscillators cost; at 256 both engines are 1516 late of 3329 with 0 xruns, which is this Realtek shared-mode driver at a 5 ms period and not the instrument. Still not measured: page faults (PLAN.md 3 parks that counter in the `--diagnostics` build that does not exist yet) and whether it *sounds* like a piano, which needs a human ear. **phase 6c outcome: `trigger=release` is played, from a pool of its own, and the flag that decides what it sounds like is `amp_veltrack`.** A release region is now *loaded* instead of refused: `SampleStore` splits what it decodes into two lists, one to start a note and one to let a key go, and `refused` drops back to what it means - a region with no path to the output. Both lists are served by one function taking a list and a key span, because the rules are the same twice (highest index last, exact key-and-velocity band first, else nearest key center, else clamp to the span) and two copies of precedence arithmetic is how the two kinds start drifting. `Instrument` gains `find_release(key, velocity)`, defaulting to `nullptr`, so the synthesiser and every existing fake are unchanged. The voice is `SampledVoice` answering a different question (`release_samples_`), and what is forced on a release is **one shot**: looping is dropped whatever `smpl` or `loop_mode` say, since a key being let go happens once. The level is not forced: the mapping's own `amp_veltrack` still decides how much of the velocity curve a hammer noise keeps, because that opcode is the recorder's opinion about hammers and not about this engine - the same release region measured **0.0022 against 0.0293** full-scale peak 30-100 ms after the key-up at velocity 20 against 110, thirteen times, which is the note's own ratio (0.0149 against 0.1956), so a library that wants one level says `amp_veltrack=0` and one that does not is played like the piano it recorded. `Engine` holds **8** release voices outside the 16: they are rendered in the same pass at the same offset, so a release starts on the sample its key-up landed on and still passes through the master gain and the clipper; a ninth key-up takes the **oldest**, because the newest key is the one the ear is waiting for; `all_notes_off` and CC 123 fade them with everything else, since a hammer noise that survives the panic button is a stuck note with a nicer name. The velocity a release is chosen by is the one the key went **down** with, remembered per key in a 128 byte table the engine fills on note-on, because a note-off arrives as velocity 0 and a release picked by a 0 would always be the softest recording the library has - and the key-up path runs in whole integer arithmetic with no divide. `offtime` is still ignored and still says so once, now with a reason that is true: a key being let go is timed by the hand and not by the file. Measured: **233 tests, 12 of them new**, the store ones naming what a release is (its own lookup, its own velocity bands, never counted as velocity coverage for a note it is not, a mapping of nothing but releases refused with a reason), the engine ones proving the audible claim from outside - a note that is a constant and a release that alternates, so `peak` and `mean` together say which is sounding without exposing a pool - including that sixteen key-ups in one block neither steal a note voice nor leave anything sounding after their files end. Audio through `boreaz-render` on a two-region SFZ, rendered twice with and without the release region: the held note is identical (**0.0995** against **0.099536**), the window 30-100 ms after the key-up - well past a 20 ms amp release - is **0.0078** against **exactly 0.0000**, and both are silent 700 ms later. That measurement is a gate now, not a script in `/tmp`: `python tools/release_check.py` builds, runs the suite, renders both `--golden` paths and both `--instrument` paths, and asserts those three windows. The numbers were re-measured after the first commit, because the first pair came from a binary that was not built from this tree - see the stale-executable warning in §8. The starter instrument is unchanged where the plan says it must be: `boreaz-sfz-probe` still reports **66 files, 66 regions playable, 0 refused, 109 MB, 199 ms**, now printed as "0 release recordings" because none of its regions asks, and the live 88-key glissando on the real device finished **256 late of 1665 callbacks, 0 xruns, 0 dropped, 0 steals** - the same figures the instrument produced before this change, which is the point: an instrument with no releases must not hear one. A release instrument on the same device ran **0 dropped, 0 steals**. Not re-measured here: the flagship's **529 of 592** regions coming back as releases rather than refusals - that zip is not in this tree, so the figure is the one carried from the machine that has it, and the code has not been run against it. Not built here either: holding a string-resonance release rather than one-shorting it, which wants one flag on `VoiceSample` and not a third pool, filters, and `boreaz-mkinst`, which exists for the 394 MB flagship and its ARIA smart variants |
| 7 | `feature/parameter-ui` | `Params` panel wired by a single descriptor table (name, range, default, unit) → knobs/sliders auto-generated; param smoothing (zipper) so no clicks on jump; range controls: transpose ±24, octave ±4, tuning ref 415–466 Hz, current-key readout; **voice-kind (synth / sampled) + instrument selector, switchable live**. **phase 7 outcome: one table, three consumers, no second list anywhere.** `engine::param_table` holds name, label, unit, range, default and the member pointer for each of the 18 parameters; the `.ini` reader, the Mixer's sliders and the parameter clamps all walk it, so the ranges that used to be written three times are written once and `--help` prints the key list out of the same rows. A jump is followed rather than arrived at: the engine slews toward the published parameters at a fixed rate, which was measured (a cutoff thrown from 0.05 to 24 jumps 0.061 per frame against the sound's own 0.068, where an unwarned jump is 0.098 - and the goldens did not move, because a static parameter file is a no-op for a ramp). Transpose and octave retune the notes already held, clamped so no target frequency can leave the audible range. The instrument selector loads on the UI thread into the sample store that is not playing - the audio thread never opens a file, and a live store cannot be cleared under it - and adopts the new one on the first frame the pool is empty, once only, which is tested by holding a note through the request; what stayed unproven is the part only an ear has: that the ramp and the swap are inaudible rather than merely finite | every engine parameter reachable; transpose/octave changes retune held notes without a click and cannot push a target frequency outside 0–Nyquist; a jump 0→1 on cutoff produces no click (verified by golden diff); one new param costs one table row; switching synth↔sampled with notes held neither clicks nor leaks a voice |
| 8 | `feature/modulation-fx` | Mod router: fixed source list (Mod env, LFO1, LFO2, Velocity, KeyTrack, Wheel) × destination list, 8 slots, bip+unipolar depth. `KeyTrack` is normalised over the 88 keys with **C4 (60) = 0.0**, A0 = −1.0, C8 = +1.0. FX bus: distortion, stereo delay, freeverb-style reverb. polyBLEP oscillator replaces the naive saw. **Sampled voices carry no filter and no modulation matrix — amp and pan only — but they do go through the FX bus**, which is where a dry grand becomes an instrument | per-voice modulation costs < 1 op/sample at default settings; wet/dry bypass A/B is exact for `mix=0`; CPU budget still < 8%; the piano with reverb off vs on is a documented A/B, since a dry sample is not a shippable sound. **phase 8a and 8b outcome: the bus exists and holds two effects, and the A/B is measured.** `dsp::Delay` (8a) and `dsp::Reverb` (8b, Schroeder: 4+4 damped combs sharing one mono excitation, 2 all-passes per ear) sit behind `engine::FxBus`, which is walked by the engine after the voice sum and before the master gain, so the fader and the clipper are downstream of everything. Four rows in the parameter table (`fx_delay_mix/time_ms/feedback`, `fx_reverb_mix/seconds`), all defaulting to silence, so `FxBus::active(params)` is false and **the whole block is skipped**: 247 tests and every golden are unmoved. The delay is asserted arithmetically (an impulse arrives `n` frames late at exactly the send, the tail reaches *exact* zero, and it holds). The room is asserted as a room: the first sample out is 1215 frames after the first sample in (one lap of the shortest comb, so it is an aperture and not a phase effect), energy falls monotonically in 250 ms windows, the two ears are not the same signal from a single source, and RT60 is a measured thing on the tail rather than a coefficient nobody checked. **A full send sits 13.5 dB under a held note** (rms, 1 s in), and asking for 1.8 s measures as 1.6 s because the damping and the leak each take a little off every lap, which the header quotes instead of pretending the label is exact. The room has a **30 Hz leak inside every comb loop**, added because measuring the worst case found a 12 s room accumulating **0.90 of full scale** from a near-DC input at a full send; leaked it is 0.14, and what it costs is 1.4 dB of send level. Rendered on the flagship at the shipping `-12 dB` master, mix 1.0, 1.8 s: total rms **+0.16 dB**, peak **does not rise** (0.0545 → 0.0530, because a decorrelated tail does not stack peaks), the quietest 100 ms of `lead-simple.mid` goes from **digital silence to -77.9 dBFS of room**, and 0.3-0.8 s after the last note there is **-84.0 dBFS** where the dry render is exactly zero. What is left in the phase: the mod router, polyBLEP, and `mix = 0` for a *shipping default* - **decided 2026-11-01: it stays 0 and the room arrives with a preset**, phase 9's factory piano patches are what owes it, see §7. **The first never-measured real-time claim is measured:** `boreaz-render --counters` puts the process's page-fault and IO counters around the engine call and prints load / first 32 blocks / the rest, and `tools/release_check.py` asserts the last line reads 0 reads and 0 writes - see §3 for the numbers. The same gate now runs `boreaz.exe` itself: the app, a device, the library, and its counters sampled from outside while the driver pulled 72 callbacks - 0 late, 0 reads. `--gui` has been *run* by hand since: the window opened, the file played and parked its transport, no `no window:` line, killed after 6 s - which proves the window path initialises and nothing else, least of all that the panel is drawn right. Phase 7's criterion that `--gui` run **without a device** is still not met: `main()` opens the device first and returns 1 when that fails, so the panel cannot be proven on a machine with no driver at all. **The CPU criterion is measured (phase 8c), twice: from the audio thread itself, and offline.** `boreaz.exe` now prints `AudioDevice::cpu_fraction()` - the callback's own busy nanoseconds over the wall time the device ran - in its exit line, and `boreaz-render` times the engine call and nothing else and prints `% of one core` on stderr, so a level or an effect can be metered without a driver in the way. **`pad-chords.mid` is the load, because it is the committed fixture that reaches 16 voices** (`glissando-88` peaks at 7). 16 sampled voices cost **0.43 %** offline and **1.09 %** on the audio thread (48 kHz, a 1024 frame period); with the room at a full send **0.59 %** / 1.39 %; room and delay **0.69 %** / 1.14-1.57 %. 16 synthesiser voices cost **6.81 %** / **8.03 %**, and with the room **7.12 %** / 8.63 %. **the bus is a per-sample cost, not a per-voice one** - the room adds 0.16-0.24 points and the delay 0.02-0.04 at *both* 4 voices (`lead-simple`) and 16, which is what a block-level effect should do and is the reason a heavy FX rack cannot blow the budget at high polyphony. **What ships is the sampled instrument and it sits at 1.1-1.4 % of one core with everything on, 6× inside the 8 % criterion.** The synthesiser does not: 8.0 % dry, 8.6 % with the room, which is the phase-3 number (9.40 %) re-measured with the app's own counter, and phase 8 is not what put it there. The diagnosis in §7's engine-cost row is still the true one, re-read from the code rather than inherited: `osc1_` and `osc2_` are **`Waveform::Sine`**, so every sounding voice evaluates **two `std::sin` per sample**, and `step()` is called on oscillators whose level is 0. polyBLEP is therefore a CPU *saving*, not a cost - it replaces those two evaluations with a table read - and the row says so now, so nobody budgets the opposite way. **8d is the distortion, and it is a blend rather than a send.** `dsp::Distortion` (`tanh(drive·x)/drive`) is the third stage, and the bus order is `drive → delay → room`: the drive is the note's own voice, so it belongs in front of everything that repeats or spaces it, and the room is the furthest thing from the speaker. Its wet line holds `shaped - in` and not `shaped`, which is what makes `dry + mix·wet` a crossfade between the clean note and the driven one; a wet line carrying the shaped signal would make mix 1 at low drive the sum of two nearly identical signals, which is a 6 dB jump dressed up as a knob. The curve was picked for two properties and both are asserted: the small-signal slope is exactly 1 (**colour, not level**), and a peak comes out at `tanh(drive)/drive` (**a drive stage can never raise a peak**), so the master clipper is asked for less help rather than more. `mix = 0` is the same bypass the other two effects use - one line in `FxBus::active` - so 250 tests and every golden are unmoved. Measured on the flagship at the shipping `-12 dB` master, full send: drive 3 takes the peak from -16.86 to **-22.11 dBFS** while the rms goes -37.38 → **-37.75**; drive 8 takes the peak to **-30.07** and the rms to -39.21, so the **crest factor goes 20.5 dB → 9.1 dB** - compression, and precisely the level the master limiter is going to want back. The curve predicts those renders to a twentieth of a decibel: a 0.572 pre-fader peak through `tanh(8x)/8` is 0.102, and the render measures -30.07. It is a drive curve and not a gain, which is a number too: a 0.9 sine's third partial goes **5.6 % → 31.3 %** and its fifth is 16.6 % at drive 8. The cost is the bus's per-sample cost - **+0.09 points** of one core on the engine call, **+0.20** on the callback that pays for it (48 kHz, a 1024 frame period) - so the sampled instrument with all three effects on is still ~1.4 % of a core. Two rows, `fx_distortion_mix` (0-1, default 0) and `fx_distortion_drive` (1-10, default 3), and the panel grows both sliders by itself because the table is the panel. One tool bug fell out: `boreaz-render --golden` refuses a parameter file, though its own `--help` line advertises one, so no effect A/B could be measured by the tool that exists to measure - it takes one now. **8e is the master limiter, and it is the box, not the curve.** See §8.1 above for the two sentences and the ceiling's arithmetic; the measurement is the row. At `--gain 12` on `glissando-88.mid` the chain that used to be the limiter wrote **66303 samples pinned at exactly +-32767 of 1565550** (4.2 % of the file, at a peak 1.66 dB below full scale) and now writes **none**, landing at 0.964020 as int16 - 0.964028 as a float peak - and **every** committed fixture at that gain lands on the same 0.964028, so the top of the fader is a ceiling rather than a limiter that taps occasionally: it buys 1.51x rms for 1.16x peak (0.8279/0.2003 → 0.9640/0.3021, crest 12.33 dB → 10.08 dB), which is what a stage with no makeup gain does. At the shipping `-12 dB` it is asked for nothing - the four fixtures peak at 0.6479 / 0.6047 / 0.3069 / 0.2993, all synthesiser, none of them within 10 dB of an internal ceiling of 2.0 - so **252 tests, every golden and every level claim are unmoved**, and the same-file twice through `Engine::bypass_limiter()` counts the samples that moved: 0 of 96 000. Cost: nothing either meter can resolve - the engine call on the 16-voice flagship load is 0.39 % of one core (median of 5) against a pre-limiter band of 0.39-0.51 %, the synthesiser 6.75 % against 6.81 %, and the audio thread itself 0.87-1.05 % against 1.09-1.39 % (48 kHz, 1024 frames, `--gain -24`), so the only thing that moved is the level at the top of the fader. The argument against is written where it can be read: the release is a moving gain and a moving gain is phase modulation, so a 100 Hz tone at 1.5x the ceiling returns with 14.9 % third-harmonic content, where the `tanh` it relieves reports 23.5 % at the same level and both converge near 30 % by 4x the ceiling. **8f is the modulation router, and the polarity is a slot field.** Eight slots of {source, destination, depth, **unipolar**} over {off, lfo1, lfo2, env2, velocity, keytrack, wheel} × {off, cutoff, pitch, osc2, osc3, noise, pan}: `mod_router.h` for the enums and the contract (shape, clamp, `mod_amount`, `keytrack_value`), the slot loop in `SynthVoice::apply_modulation` for the arithmetic, `prepare_modulation` for the flags that make an unwired voice cost nothing. 38 rows added and 0 removed, **25 → 63 knobs**: four per slot plus the six the cables cannot work without (`env2_*` times, `lfo1_rate_hz`, `lfo2_rate_hz`), with `ModDest::kOff = 0` so an unwired cable is the stored default. The polarity is **per slot and not per source** because five of the six sources are levels that rest at 0 and Env2 is one of them — a per-source rule makes "env2 to pitch, bipolar" an envelope *pitch drop* whose depth depends on how long the key is held, which is not the patch that was named. Units are the destination's own: ±2 semitones, **±3 octaves as `2^x`** (because a cutoff is a multiplier), the whole pan field, ±1 of a level, and only the levels are floored at 0. Keytrack ships as this row specifies (A0 −1.0, C4 0.0, C8 +1.0) with the two halves scaled separately so both ends are exact. Env2 is the voice's own mod envelope, struck at `start()` and released with the key; the LFOs go on running across notes on purpose (a chord whose vibrato all starts at phase zero is one instrument); every source is read once per block and only if somebody routed it, so an unwired cable advances nothing. Measured on the shipping tool: **1.36 %** of one core unwired against **1.38 %** with all eight cables across every destination, medians of 5 inside a 1.33–1.79 % spread (`boreaz-render`'s `% of one core`, `lead-simple.mid`, engine call only) — which is what "nothing measured in the CPU cost until a patch asks for it" had to mean, and 269 tests and every golden unmoved. The router also found a bug that predates it: `ParamSpec` addresses a parameter three ways and two callers branched on `spec.real != nullptr`, so every array row was treated as a whole number — a patch file refused `mod1_depth = 0.4` and the rows it accepted were written through a null member pointer, with the panel doing the same read for 33 of 63 rows. `param_is_integer(spec)` is now the question, asked of the table. **8.4 is the panel, and a cable is chosen rather than numbered.** The 32 cable rows were sliders, so wiring a patch meant remembering that `mod1_source = 2` is LFO 1. The words now live beside the enums in `mod_router.h` — each list sized by `kCount`, so an enumerator added without a word stops compiling — and the table hands them to the panel with `param_choices(array_kind, &count)`, asked by array kind because 32 rows would otherwise be 32 copies of two lists. Four of the six destinations are named for the row they move (**Cutoff, Osc 2, Osc 3, Hammer noise**), which is asserted, as is every name against its enumerator: reordering `ModDest` and leaving the panel calling velocity "Wheel" is a defect no engine test would ever see. `M1 uni` was a 0–1 slider and is a checkbox (any integer row whose range is 0..1 is a switch, so the next boolean gets one for free), the row labels drop three abbreviations, and a find box filters on key or label and prints `N of 63`, because 63 rows in a 340 px column is not a table a hand can search. **The modulation envelope is labelled "Mod env" next to the cable that names it while the amp envelope keeps the bare "Attack" a player reads.** Labels landed in 8.4; **the keys followed on 2026-11-04 and the four rows are `mod_env_attack_s`/`_decay_s`/`_sustain`/`_release_s`**, because a rename that is free now is a compatibility question the moment phase 9 writes a patch file — see §7. Nothing moved in the engine, because this is the panel reading the table, which is what the table is for: 270 tests, flagship golden still `peak=0.306908`, `release_check` green. **8.5, the sine table, and it is the item the plan had backwards: the CPU that phase 8's polyBLEP row was credited with turns out to have been `std::sin` all along.** Micro-benchmark, one binary, `-O3`: one `waveform_at(Sine, phase)` costs **48.0 ns**; a 4096-entry table read with linear interpolation costs **2.9 ns**, and nearest-neighbour costs 2.7 ns, so the interpolation is 0.2 ns and it stays. Engine-level, same build directory, medians of 5: `pad-chords.mid` at 16 voices goes **6.07 % -> 1.14 %** of one core, `lead-simple.mid` at 4 voices goes **2.28 % -> 0.34 %**. The number the criterion is actually measured on — `boreaz.exe --no-instrument`, 48 kHz / 1024 frame period, the device that has failed it every phase since 3 — goes **10.9 % -> 2.32 %**, so the synthesiser is inside the < 8 % budget for the first time, with 3.4x margin, and today's baseline measured 10.9 % against the 8.03 % the plan quotes, which is machine variance pointing the same way. The table is `.rodata`, 16 KiB, built at compile time by a constexpr 13-term Taylor (`detail::sine_exact`, which stops when a term becomes zero), and the read is a `std::fma` between two `float` entries at namespace scope — not a function-local `static`, because that puts a guard on every call. The index is masked, not `%`, because `phase * 4096.0f` is 4096.0 at phase 1.0 and `4096 & 4095` is 0, which is the wrap the phase counter already does; `static_assert(1 << 12 == kSineSize)` is why a resize cannot silently break that. The accuracy is asserted, not assumed: the worst absolute error over a cycle, sampled half a step off the table's own nodes, is **4.76837e-7** inside a 1e-6 bound, and the `float` reference `std::sin` carries 4.023e-7 of its own, so the interpolation sits at the floor of what a `float` can ask for. The goldens are the proof of what did not change: **every peak identical** (0.299295 / 0.647880 / 0.306908 / 0.604739), **every steal count identical** (0 0 0 8), and the flagship compared sample by sample: **573 146 of 573 300 samples bit-identical, 154 off by exactly one 16-bit unit**, which is -96 dBFS. `GOLDEN_SYNTH` in `tools/release_check.py` moved for that reason and its comment says so; `GOLDEN_SAMPLED` did not move, because recorded voices never ask the oscillator for anything. What this is **not** is polyBLEP: no waveform changed, the aliasing item is still open, and its real scope is narrower than this row ever said — **no parameter selects a waveform.** `osc1_waveform`/`osc2_waveform` are `Sine` and `osc3_waveform` is `Saw` in `synth_voice.h:37-39`, so `Square` and `Triangle` have never been reachable from a patch and the aliased saw reaches the output only as osc3 at `osc3_level` 0.12. 270 tests, `release_check` green. What is left in the phase: polyBLEP. |
| 9 | `feature/presets` | save/load patch files (`key = value`, versioned header), factory patches (one per register: sub/bass/low-mid/upper/brilliance, each written for the A0–C8 compass, plus piano presets at several dynamics, **and any factory piano patch with `fx_reverb_mix = 0` leaves the §6 row about a dry grand open**), patch browser, config file for audio/MIDI device prefs + last patch + instrument path, autosave on exit | patch round-trips byte-for-byte, including transpose/octave/tuning-ref/voice-kind/instrument; a missing or unreadable instrument **falls back to the synth with a visible warning**, never silence; a corrupt or unknown-key file is **refused whole**: the reason appears in the panel's own line and the patch that was playing keeps playing (**decided 2026-11-04**, reversing this row's earlier ask for a partial load with a warning — the parser has always taken a file as a unit or not at all, and a half-loaded preset is some of one sound and some of another, which is a sound nobody wrote and nobody can name; the app's job was to say why rather than to exit) ; **`voice_kind` is not a parameter, and the phase-9 row's list of names is met without one — decided 2026-11-04. The kind is *which library is playing*, so it travels as a name and not a number: a patch names its library in the header (`save_params_file`'s instrument field, read back by `read_patch_meta`), and adopting the patch asks for that library — one that names nothing keeps what is already playing, and one whose library has gone away keeps its parameters and says why in the panel's own line, which is this row's fallback rather than a contradiction of it. The app's setup file carries the same name across a restart. A stored kind would be a value that goes stale the moment its library does: `voice_kind = sampled` over a missing library is a lie about the sound the player is about to hear, and this row has refused that shape of wrong since it was written** (the DoD two clauses above demands a fallback to the synthesiser rather than silence, which is what a stored kind would contradict). The five names round trip: three as rows of the patch file, two as names, asserted in `tests/test_live_params.cpp`, `tests/test_params_panel.cpp` and `tests/test_app_settings.cpp`. | **9.1, the file format and the writer, is done:** `save_params_file` writes every parameter in the table's own order, each as the shortest text that reads back as the same number (`std::to_chars` with no precision): `amp_sustain = 0.7` rather than `0.699999999`, and `octave_shift = 2` rather than `2.0` — the reader refuses a fraction on a whole-number row, so a writer that printed a decimal point would write files it cannot read. Above them sits `# boreaz patch format 1` and two comment fields, `# name:` and `# instrument:`, which the existing reader ignores **on purpose**: a patch's name and the sampled instrument it wants are not engine parameters and the reader refuses unknown keys as a rule, so the header is the one place in the file they can live without any new loading code. **A patch file is therefore a parameter file** — `--params mypatch.txt` already works on it. Proof of the row's round-trip claim: every one of the 63 rows written at its minimum, its maximum and its default comes back bit-exact, and save-then-load-then-save gives identical bytes. What is left in the phase: a way to trigger a save and a load from the app, factory patches, the browser, the device config, autosave on exit. | **9.2, the panel's side, is in:** a `Patch` block in the mixer — a name field and Save into `patches/<name>.txt`, and the folder listed as one button per patch, labelled with the name from its own header, hover showing the file and the instrument it wants. A load takes the parameters *and* the instrument in one action, through the engine's ramp so it cannot click; a patch that names no instrument keeps the library that is playing, and a patch whose library is missing says so in the panel rather than going quiet. `patches_dir()` finds the folder the way `defaults_path` finds `config/params.ini`, because a build tree sits below the repository. What is left: the factory patches (written as files, checked by render), the device config file, autosave on exit.  **9.3, the factory patches, is in:** eight files in `patches/` - five registers (Sub Register, Bass, Low Mid, Upper, Brilliance) and three piano dynamics (Pianissimo, Mezzo Forte, Fortissimo) - each one written by `tools/factory_patches.py` from the handful of keys it actually changes, expanded against `param_table.cpp` so a default keeps living in exactly one place, and `--verify` says when a hand edit and the recipe disagree. **Every one carries all 63 keys**, which is what makes it a sound rather than an opinion about one parameter of somebody else's. `tests/test_factory_patches.cpp` loads them through the app's own strict reader and asserts the things that can rot: one assignment per row, a name in the header, no instrument named, no factory patch in the folder the app writes to, and at least three patches carrying a room. **Measured by render, four fixtures, best of 3, and nothing leans on the ceiling:** the loudest patch on the heaviest fixture is `piano-fortissimo` at **peak 0.511** with 16 voices, where the shipped `config/params.ini` on the same fixture is 0.869, and the dynamics presets measure the master gain they are named for - **0.144 / 0.282 / 0.511** for pp / mf / ff, which is -6 / 0 / +6 dB. CPU on that 16-voice fixture: **1.01-1.08 % of one core for the registers, 0.54-0.55 % for the piano presets**, against 1.03 % dry synthesiser and 0.36 % dry sampled; the piano side is higher only because of the room, and the A/B on one patch says so (0.36 % with `fx_reverb_mix = 0` against 0.57 % with it running on sixteen voices, and 0.19 % against 0.29 % on four: **+0.10 to +0.21 points**). **The §6 and §7 rows about a dry grand are closed by these files**: the presets send 0.35 / 0.30 / 0.18, the registers send nothing, and that is phase 8's "the room arrives with a preset" decision paid. **The overwrite the factory patches made possible is closed in the same change:** saves go to `%APPDATA%/Boreaz/patches` (`user_patches_dir()`), the browser lists shipped voices first and the player's own underneath, and the shipped folder is never written to. Two parameter comments in the config files were found telling the truth about an earlier design: `osc2_level` is not "the saw an octave below" and `osc3_level` is not "the fifth above" - `synth_voice.cpp` puts a sine an octave above and a saw in unison, and the `# The three parabolas` line in `config/params.ini` is the same mistake. Fixed in the comment, not in the engine: the goldens are the evidence of what the engine actually does. 277 tests. **9.4, the app's own setup, is in:** four names in one file - `%APPDATA%\Boreaz\settings.txt` (`settings_path()`, beside the folder saves go to): the audio port, the MIDI port, the sample library, and the patch that was open. Read at every start, written at the end of a session that had a window; a run with no window writes nothing, because playing one file from the prompt is a command, not a preference. **Deliberately not a patch:** no parameter lives in it and nothing rewrites a parameter file on the way out - closing the app is not a thing that edits a sound, and a setting like `audio_device` would be refused by the parameter reader anyway. Its reader is line based and forgiving, the opposite of the parameter reader on purpose: an unknown key is skipped and named with the number of the line it was on, a line that is not an assignment is reported and the keys around it still work, no file at all is a first run that says nothing, and a file written by a newer build starts on an older one. **Precedence is the newest thing said,** which is the rule `--gain` already follows: a flag at the prompt, then this file, then the program's default, and `--no-instrument` outranks a remembered library. **A remembered audio port that is not there any more warns and opens the system default** rather than refusing to start - the interface was asleep, the laptop is off its dock - while a port named at the prompt still fails, because a person asked for that port and deserves to be told. A remembered patch that cannot be read says so and starts from the defaults: a path this app wrote is not a promise the player made today. A remembered library that cannot be read keeps this phase's fallback to the synthesiser with a visible warning, and because the exit writes what is actually playing, one session clears a stale path. What is written is the name that was used, not the word that asked for it: the port the driver answered with, so a remembered index does not move when the order of ports changes. **Measured:** a start with a remembered library and patch loads both (66 regions, 109 MB resident, 165 ms); a session at the mixer rewrote the file with the port it used, the library it loaded and the patch it opened with; a remembered port that no longer exists was named, then the default was opened, and the run still played. `tests/test_app_settings.cpp` (4 cases) holds the round trip byte-stable, the unknown key skipped and named, the first run quiet, the broken line reported, and the separation from the parameter file: none of the four names is an engine parameter, and the strict parameter reader refuses this file. Nothing is left in the phase: the criterion's five names all round trip, three as rows of the patch file and two as names. The rows are asserted with values away from their defaults (`transpose_semitones -3`, `octave_shift +2`, `tuning_reference_hz 432` written, read back, and written as the same bytes again). The names are the patch header's instrument, which the panel turns into a library request the moment the patch is adopted, and the setup file's four names, which survive a save and a restart byte for byte. **9.5, a remembered path resolves from wherever the next start begins:** the same file holds paths, and a path written relative to where today's process happened to begin is a path that worked exactly once. Read back from `build/`, the value 9.4 wrote (`patch = patches/piano-fortissimo.txt`) printed `cannot open the parameter file 'patches/piano-fortissimo.txt'` and came up on the shipped defaults instead of the voice the player had left open. **Fixed at the write, not by a search at the read:** `config::resolved_path()` absolutizes and collapses `.` and `..` lexically (`absolute()` + `lexically_normal()`, because `weakly_canonical` in this standard library leaves a relative path relative when the file does not exist yet, which failed two assertions before it was replaced), `patches_dir()` answers absolute so the browser and the first save find the factory folder from any working directory, and the exit writes `patch` and `instrument` as the paths that resolved. Searching at the read was the other option and is worse: a bare `patches/x.txt` would then be ambiguous between the shipped folder and the player's own, with the app picking one silently. `resolved_path("")` stays empty on purpose - an empty library is the synthesiser, and resolved it becomes the working directory, which is a CHECK and not a comment. **A patch file keeps the name its author typed:** that file is written to travel between machines, this one is one machine's note about itself. Measured in order on the device: relative memory from `build/` = 1 warning and the defaults; one session started with `--params ../patches/piano-fortissimo.txt` rewrote the line absolute; starting again from `build/` and from the repository root with no flags = 0 warnings both (0.71 % and 1.01 % audio-thread CPU, 0 restarts, 0 dropped, 0 steals). What it does not do: an old file keeps its relative line until one session at the mixer rewrites it, and `defaults_path()` keeps its relative walk - neither is remembered state. 283 cases, `release_check` green, goldens unmoved. |
| 10 | `feature/seq-playback` | in-app `.mid` playback using `boreaz-midifile`: transport (play/stop/pause, position, loop), tempo from the file + user override, sequencer clocked by the **audio thread sample counter** and pushing into the same event ring, keyboard + playhead follow, `boreaz -- song.mid` (the file is named positionally; **the part that plays it is already in**, on `feature/file-playback`: the file is loaded once, fed into the same event ring ahead of the audio thread's position, and the transport starts with it and parks at its length — what is left here is the controls, i.e. pause, loop, seek and an open dialog - 10.3 built every one of those but the dialog, and 10.4 settled what that dialog should be: not a dialog, see the end of this cell. **10.1, the machinery, on `feature/transport-core`: `boreaz::midifile::Transport` (play, pause, stop, seek, whole-song loop) over a `Song`, and it keeps no counter of its own - one number, the offset between `Engine::sample_position()` and the position inside the song, so a position is a subtraction. The engine gained `request_transport_flush()`: throwing away notes already queued for a timeline being left has to happen on the audio thread, because `EventQueue::clear` is consumer-side only, so the request is an atomic flag and the drop is in `process()`, which also panics. The block after any state change feeds nothing - 10.7 ms at the front of a resume, and the price of a pause being a pause rather than a wobble. **10.2 on `feature/transport-routing`: the app's own pump now goes through the transport instead of feeding a cursor itself, so a command-line file and a clicked song are the same player; the transport gained `set_lookahead` because the mixer's pump needs the half second of runway it always had (the default is one block, and a pump that sleeps longer than its lookahead feeds late)** **10.3, the controls, on `feature/transport-controls`: the row lives in the window shell, under the view switcher and above the panel, so it never depends on which panel is open and phase 11 and 12 cannot hide it.** Play/Pause is one button and what one press means is `Transport::toggle`'s answer, not the button's; then Stop, a Loop checkbox, and a position bar reading `m:ss / m:ss` that **seeks where it is clicked** - a click and not a drag, because a bar that follows the playhead and is also the thing you drag fights the player every frame. The window owns no song and no clock: `Options::Transport` is nine calls that read the app's state and report what a hand pressed, all on the thread that pumps the window, and `Transport` grew `toggle`, `seek_fraction` and `end_sample` so the arithmetic of "what does this control do" exists once. `Enter` and numpad `Enter` call the same thing the button calls; not Space, which is the sustain pedal, and not a letter, because `m` is A. Backspace stays what phase 5 made it - the strip's own clock, not the song's playhead, and the two clocks are different things. **Two bugs fell out of wiring it:** typing a patch name into a text field played a chord, because every letter in `piano` is a note - the guard is `WantTextInput`, and *not* `WantCaptureKeyboard*, which goes true in this app for reasons that have nothing to do with a field being open and would have taken the piano away; and the loop state had two accessors (`looped`, `looping`) where one was enough. **Measured, on a live window, live device and real file**, using the one thing that makes an invisible window observable - the app prints `finished` when the file runs out: one `Enter` after a 2.958 s file had ended sent it back to the top and it ended again (one `finished` in the log before the key, two after); and one `Enter` at 1.2 s of that same file left it unfinished at 5.7 s, which cannot happen unless the pause took, and it finished after a second `Enter`. The key arrives by `PostMessage(WM_KEYDOWN)` because the desktop refuses `SetForegroundWindow` from another process, which is how the first three attempts at this test silently proved nothing. **Not proven: the mouse.** Nobody has clicked Play, the Loop box or the bar. And the row is drawn only when a song is loaded, so "the transport strip never hides" is now qualified honestly: it never hides *from a loaded song*, and it does not care which panel is open. 295 cases, 8 861 666 assertions, `release_check` green, goldens unmoved. **10.4, the last item, on `feature/open-a-song`: a song chosen while the app runs, and the open dialog is deliberately not one.** `GetOpenFileName` was not used because its modal loop runs on the thread that pumps this window, and that thread is the one feeding the event ring: a picker left open past the lookahead lets the device run past the samples it was given and stutters the song, which is the app's one real-time promise. What replaces it costs no thread. Drop the file on the window (GLFW's drop callback copies the name into one slot, because its C strings die with the call, and the drawing frame acts on it), or type a path: F1 or the `Change` button opens a field that takes the keyboard in the frame it opens, and Enter loads it. Reading stays the app's job: `Options::Songs::load(path, reason)` runs on the window's thread, parses into a temporary, and only then stops the transport, moves the `Song` and reloads - so **a path that will not read leaves the session exactly as it was**, the loader's own words going to the log and to the panel's line. `song_path` becomes the one name for which file is loaded, so the startup print, the runtime print and the `finished` line cannot disagree, and the window learns the name from `set_song_path`, in the same family as `set_external_playback`. Measured on a live window and a live device: an app started with **no file at all**, F1, the path typed one `WM_CHAR` at a time, Enter - `"...pad-chords.mid": 48 events, 6.5 seconds`, then `finished`. And an app started with `lead-simple.mid` handed `C:/songs/does-not-exist.mid`: `cannot open 'C:/songs/does-not-exist.mid' for reading. Is it a file, and is it there?`, the original song's `finished` already printed, the run continuing with nothing swapped. F1 rather than Ctrl+O because **a modifier cannot be injected into a window that is not the foreground one**: posting left-Ctrl then `O` delivers the `O` stroke with `mods = 0`, GLFW taking its modifiers from the operating system rather than from the queue. That is why every shortcut in this app - Enter, Backspace, F1 - is a bare key, and it is written into `tools/key-in-window.md`, which gained `-text` (a `WM_CHAR` per character into an ImGui field) and lost the `-ctrl` that cannot work. Not measured: the **drop** itself, for the same reason the mouse is not measured - `glfwSetDropCallback` is wired and the frame answers it through the same `ask_for_song` the field uses, but an OLE drag out of Explorer cannot be scripted.) | a `.mid` plays in time with no drift against the audio clock over 3 minutes (measured at the transport: 16 740 blocks, 8 570 368 samples on a looped song, and the final position equals start + elapsed mod the song length exactly, wraps included); stop and panic leave zero stuck notes (measured; and what `panic` is matters here - it releases voices the way dampers do, so the claim is no voice sounding that is not released, not silence); seeking/looping retrigger correctly (measured: a note passed over does not sound, the next one does, a seek into a held note does not start it again, and the loop wrap keeps its overshoot so the repeat stays in time); a file with >16 simultaneous notes plays as specified stealing, no crash (measured: twenty note-ons into a pool of eight, active voices capped, steal count up); CPU stays inside the phase-8 budget (measured against the code it replaced: interleaved A/B, 10 runs each, headless with a song - 278-281 callbacks, 0 restarts, 0 dropped, 0 steals on both sides, and audio-thread CPU 0.82 %-1.25 % against 0.94 %-1.29 % for the old path, i.e. nothing measurable. The rule this taught: absolute CPU readings on this box drift by tens of percent between hours - the same old binary reads 1.20 % today where the phase-8 band was 0.71 %-1.01 % - so the 1.2 % budget in phase 12 is judged by an A/B on the same machine at the same hour, never against a stored number); notes outside A0–C8 flagged `EXT`, not dropped (measured: keys 10 and 120 sound, pitch from the tuning table, and are flagged) (on the piano these are pitch-shifted edge samples, which must sound wrong enough to be honest) |


| 11 | `feature/note-waterfall` | **Note waterfall** in the switchable panel: 88 lanes 1:1 with the keyboard, notes leaving the keyboard line and scrolling up, length = held duration, brightness = velocity, colour per MIDI channel, scroll-speed + lane-height controls, toggle kept in the panel switcher. Source is the display tap + voice snapshot — **held and recent notes, no look-ahead**, so nothing falls in from the top | lanes line up with the keys below them to the pixel (geometry assertion test); a 5 ms staccato note still appears (proves the tap, not frame polling); 40 concurrent notes over a glissando file drops zero notes at 60 fps; panel costs < 2 ms/frame and introduces **zero** audio glitches (glitch counter stays 0); switching to transport or back does not stop, reset or retrigger playback |
| 12 | `feature/spectrum-panel` | **Spectrum** in the switchable panel: log-spaced bands, one per key range from `KeyboardLayout` so a band sits over the key that plays it, with a falling peak-hold line over the top. Source is the **master output post-FX** — what the speakers get, reverb and noise included — taken by a new **audio tap**: a fixed SPSC ring the callback memcpy's into and overwrites when nobody drains, so it can never wait. `Analyser` lives in `boreaz_dsp` (Hann window, radix-2 FFT, energy summed into bands, dB, peak decay) so the panel stays one file that includes no FFT, and **the frame loop does the maths while the audio thread does the copy**. **Two window lengths**, because one window cannot do both jobs: 8192 samples (~171 ms, ~5.9 Hz per bin) where the bins have to be narrow, 2048 samples (~43 ms, ~23 Hz, about a semitone at C8) where a transient has to be lit now. Peak-decay rate is the one knob, in the `.ini` | on synthetic buffers, no window and no device: a lone −20 dBFS sine lands in the correct band and within 1 band of it for A0, C2, A2, C4, A4, C6, A6, C8, reading within 1.5 dB; sines a fifth apart (220 + 330 Hz) show two bands with a valley ≥ 6 dB deep; silence reads below −100 dB and DC does not light the lowest band; a 20 ms burst lights the top octave within two frames, which is what the short window is for. Real-time: glitch counter **0** across an 88-key sweep with the panel open; a frame loop that stops draining for 10 000 blocks neither blocks nor grows the ring, and the panel says how much it dropped; the analyser prints its own µs/frame inside the < 2 ms/frame panel budget; switching to transport or back does not stop, reset or retrigger playback (inherited from the waterfall) |


### 8.1 What the limiter is for (phase 8e)

Nothing in the chain above it can promise a level, so something at the end has to. That is the whole
justification, and the numbers are in the phase-8 row above; the parts worth being findable here are
that the **ceiling is 2.0 because that is where the `tanh` behind it stops being worth asking for**,
that the stage is **transparent below the ceiling to the bit** (0 of 96 000 samples move at the
shipping master, measured with `Engine::bypass_limiter()`, which exists for that assertion and
nothing else), and that it is **not additive and not idempotent** - one gain per block, following that
block's own peak - which makes it a compressor with the threshold at the ceiling. Its cost is below what
either meter resolves (0.39 % of one core against a 0.39-0.51 % band offline, 0.87-1.05 % on the device
against 1.09-1.39 %). What it is not is a knob: the ceiling is deliberately absent from the parameter
table, because a ceiling you can set above where the chain runs out of curve is a make-work knob.

Optional, only if asked for: Linux/ALSA + macOS build, virtual MIDI
port, MIDI clock/sync (MIDI time code / external clock), automation/param OSC, CI release
artifacts, A/B patch compare, keyboard zones (lower/upper split or layer across the 88
keys), a modal/inharmonic synth piano (only if sampling is ever abandoned), **waterfall
look-ahead** (notes falling in from the top, fed by the `.mid` timeline — needs a `VizSource` abstraction and would only
apply during file playback), **oscilloscope and spectrogram panels** (the other two shapes a spectrum
view could take, rejected for the shipped one and kept here: a chord is a scribble in the first and
the second is the waterfall's motion wearing a colour ramp).

## 5. Git workflow

- `main` = released. `develop` = integration. Never commit straight to `main`.
- Branches: `feature/<name>` off `develop`, `fix/<name>` for bugs, `release/x.y` when we
  cut one, `hotfix/<name>` off `main`. Merge back with a merge commit (no squash — the
  phase history is our documentation).
- Messages: **Conventional Commits** — `feat: add state-variable filter`, `fix: reset
  oscillator phase on retrigger`, `test: cover ADSR retrigger`, `build: pin miniaudio`,
  `chore: ignore build dir`. Scope is the module, not the phase: `feat(dsp):`, not
  `feat(phase-1):`. No stage/phase/TODO-ticket wording.
- Every phase = one PR into `develop`, reviewed before merge.
- **No secret and no machine-specific value is ever written in code.** Anything the code needs
  but must not ship — audio/MIDI device names, ports, paths, cache dirs, tokens — is read from
  the environment, backed by a `.env` loaded at startup (`BOREAZ_*` keys). **`.env` is never
  committed**; a committed `.env.example` lists every key with an **empty value** and no real
  machine data, and `docs/PRIVACY.md` says what we store and why. Runtime config (devices,
  last patch) lives in `%APPDATA%\Boreaz\`. Deleting `.env` must still leave a build that runs
  with sane defaults — a missing `.env` is never a crash.
- No machine-absolute paths in tracked files. No assistant or model names in names,
  comments, logs, or fixtures. `.gitignore` covers `build/`, `out/`, `*.wav` except fixtures, IDE dirs
  and `.env`.
- **No binary assets in git.** Neither the 748 MB upstream tree nor the 147 MB subset.
  No git-lfs either: the instrument is a build artifact and a release attachment, not
  history. Third-party audio is fetched by `mkinst` from a pinned URL + sha256 recorded in
  the recipe, and CC-BY attribution is generated from that recipe, so provenance is
  reproducible rather than folklore.
- I will not push or merge to `main` without your explicit approval. Pushing to
  `develop`/`feature/*` and opening PRs I may do freely.
- Anything I cannot run for safety, I hand you an exact copy-paste command plus the
  directory to run it in.

## 6. Risks

| Risk | Mitigation |
|---|---|
| miniaudio's `ma_midi` is thinner than RtMidi (winmm only, no virtual ports, weak SysEx) | **phase 3 outcome: the risk was the wrong shape — there is no `ma_midi`.** Our own winmm wrapper is ~200 lines, parses System Real Time off a live cable (the case RtMidi is careful about and a keyboard sends), and refuses SysEx rather than truncating it. Swap to RtMidi only if virtual ports are wanted — engine untouched |
| WASAPI shared-mode latency surprises / device format resampling adds cost | prefer exclusive WASAPI at device's native rate; print the *actual* period + rate, not the requested one. **Measured in phase 3, and the first half of this row is wrong for us**: the dev box's default endpoint sits at 192 kHz, so "native rate" meant 2.7 ms deadlines and 3× the engine cost (9 % vs 3 % of a core at 4 voices). We request 48 kHz shared, print the delivered period and rate, and keep exclusive mode as a later option, not a default |
| GUI loop and audio loop fighting over CPU → glitches | GUI renders on vsync/idle, never blocks; audio callback never waits on GUI |
| Filter self-oscillation blows up at high resonance | clamp resonance, tanh-limit filter input, golden test with resonance at max |
| Voice stealing producing clicks | 5 ms release-on-steal ramp before the voice becomes reusable |
| 88 keys × sustain pedal ≫ 16 voices; a glissando sounds like note-stealing chaos | keep pool at 16 but make it a spec'd, tested behaviour (steal oldest+quietest, 5 ms ramp), shorten release on steal, and measure the 88-key sweep fixture. Only raise the pool if the sweep sounds bad, not because 88 is a bigger number |
| Top octave aliases with a naive saw | polyBLEP (phase 8) + a measured aliasing report at C8, so "sounds cheap" becomes a number |
| A0 (27.5 Hz) through a resonant filter buzzes or self-oscillates | double-precision coefficient calc, resonance clamp, tanh limiter on filter input, golden test at both compass ends |
| Octave-number convention fight (C4 vs C3 vs C5) breaks patches and GUI labels | `C4 = MIDI 60` declared in §1, one `note_name()` helper, unit test on it |
| `.mid` parser is an unvetted dependency | `boreaz-midifile` is a thin lib we control, so swapping the parser touches one target; re-check licence/C++20 in phase 4 before writing code against it |
| Sequencer clocked by wall time drifts against the audio clock (samples ≠ ms under resample/underrun) | clock = audio-thread sample counter, tempo mapped to samples; drift test over 3 minutes is a phase-9 exit criterion |
| A `.mid` with 40 notes down lands on 16 voices and sounds like a stutter | expected, but tuned: verify on a real multi-track file and shorten release-on-steal if needed rather than raising the pool |
| "Waterfall" is promised but the source is snapshot-only, so nothing falls in from the top and it will not look like Synthesia | **accepted deliberately.** It is a note-history waterfall: works for live playing, costs no new engine path, never misrepresents timing. `VizSource` stays an interface so look-ahead is additive, and the switcher label says what it shows |
| UI polls at 60 fps, so a 5 ms note between two frames is invisible and the waterfall looks like it drops notes | that is why the display tap (edge ring) exists; the snapshot alone is not enough, and the 5 ms staccato test is the phase exit criterion |
| One FFT window resolves low notes or fast notes, never both | **two window lengths** (8192 where bins have to be narrow, 2048 where a transient has to be seen now), the crossover measured, and both ends of the compass are exit criteria rather than aspirations |
| A spectrum band at A0 would have to be 1.6 Hz wide to be a pitch, and no display can wait 600 ms for that resolution | **accepted and stated.** A band is a range with energy summed into it, the tooltip says so, and the bottom octave and a half reads as a rising shape. A spectrum that pretends to name pitches is worse than one that admits it is showing energy |
| A tap or an FFT in the wrong thread turns a display feature into an audio glitch | the callback only copies into a fixed ring and overwrites when the UI is slow, so it cannot wait; the FFT runs on the UI thread where the worst outcome is a dropped frame; µs/frame is printed and plan §3 already rules that a visualiser costing a glitch is a bug |
| 88 lanes + 40 quads per frame starves the audio thread of CPU | draw-list batching, no per-frame allocations, fixed < 2 ms/frame budget with a measured report; the glitch counter is the arbiter, not opinion |
| Naive oscillators sound cheap and alias | phase 8 polyBLEP; keep the naive version behind a clear `shortcut:` comment |
| CC-BY-3.0 needs attribution **and "indicate changes"** on a subset we modified | `NOTICE.txt` and the about-box credit are **generated** from the pinned `mkinst` recipe; CI fails a release whose instrument has no matching attribution |
| 394 MB upstream download for build and CI | **Recounted 2026-11-01: the download is 488.7 MB** (the 44.1/16 `.tar.bz2`) **or per-file FLAC from Git LFS for only what the subset keeps** — the second is what `mkinst` will do, which turns "download the whole library to build" into ~2.7 MB per recording kept. Pinned URL + sha1/md5 published by the host, sha256 computed and pinned here; cached; fetched by `mkinst` only. Unit tests use the synthetic SFZ fixture, so CI needs no download. The **starter** CC0 asset needs 32.4 MB (or 2.2 MB) and it too stays out of git; `tools/mkinstrument.py` is the only thing that fetches it, and `--verify` checks what came back |
| SFZ's opcode surface is open-ended (ARIA/OPCODEX extensions) | implement a **declared subset**; unknown opcodes warn once and are ignored; a region we cannot honour is refused loudly at load, never mis-played silently |
| `trigger=release` regions fight note-off voice reuse | release samples live in their own small pool outside the 16, so they survive the voice that spawned them |
| Bad or absent loop points → a click every cycle, or a note that dies at 5 s | **Not a thing on what we ship, measured 2026-11-01.** The flagship subset has no loop points because its upstream has none. 20 of its 120 note recordings end inside 5 s (shortest 2.63 s, `A7v1` — the top two octaves, which decay that fast on a real piano), and **every one of the 208 files ends at -40 dBFS or quieter** (loudest tail: 0.0110, a key-off recording whose own attack is 0.003), so a voice stopping at the end of its file is inaudible and there is no loop seam left to click. A held middle C on the subset measures 0.0047 / 0.0035 / 0.0042 rms in the 3-4, 5-6 and 7-8 s windows, so it lives as long as its 16 s recording. `mkinst` carries `loop_start`/`loop_end`/`loop_mode` when a source has them; it has never verified a seam or written a point, which is what this row used to promise. A source recorded short **and** loud at the end is the case that would need that tool, and it is not this one |
| A 280 MB preload is slow to start | measured: **480 ms for 208 files / 280 MB**, 205 ms for 66 files / 109 MB, printed at startup. That is a one-off on the setup path and it does not hurt, so mmap + warm-up stay rejected — they reintroduce the page-fault risk §3 forbids |
| A dry grand sample sounds worse than a good synth patch | reverb bus is **on by default** for the piano, and the off/on A/B is a phase-8 exit criterion. **The A/B is measured (phase 8b) and the room still ships at `mix = 0`,** because "on by default for the piano" is a decision about one instrument and `Params` has one default for both engines: turning it on moves every golden and changes what the app sounds like when it starts. **Resolved 2026-11-01: the room rides with a preset, not with `Params`** - see §7's reverb row. Phase 9's factory piano presets are the thing that owes this, and a factory piano patch with `fx_reverb_mix = 0` leaves this row open. |
| FetchContent needs network on first build | documented; vendoring stays an option (decision table row) |
| Windows-only accident creeping in | `dsp/` + `engine/` reviewed for `<windows.h>` includes; keep platform ifdefs in `io/` only |

## 7. Resolved / still open

Answered during planning, so nobody re-litigates them in a code review:

| Question | Decision |
|---|---|
| Voice pool | **16, fixed at device-open.** Stealing is a tested behaviour, not an accident. Revisit only if the 88-key sweep fixture sounds bad. |
| How is `KeyTrack` normalised? | **Shipped 2026-11-03 as this row always said: over the 88 keys with C4 (60) = 0.0, A0 = −1.0, C8 = +1.0.** The one thing the row left open is that 39 keys sit below middle C and 48 above, so the two halves are scaled separately and both endpoints are exact — the alternative is one slope with the exact end at one end and a clamp at the other, which puts the discontinuity where a hand is. The slope is therefore 2.6 % of the range per key below middle C and 2.1 % above. `mod_router.h` says so at `keytrack_value`, and `tests/test_mod_router.cpp` asserts the endpoints, the middle and both clamps. |
| Is the reverb on by default for the piano? | **Decided 2026-11-01: no, and it waits for presets (phase 9).** `fx_reverb_mix` keeps its `Params` default of 0, so nothing at startup moves and no golden is regenerated to buy a sound design opinion. The room arrives **as a patch** - which is what the row in §6 always wanted ("on for the piano" is a statement about an instrument, and one number in `Params` cannot be a statement about one instrument) - so **phase 9's factory piano presets must carry `fx_reverb_mix`**, and shipping them without a room counts as the row not being honoured. **The number arrived with the presets on 2026-11-04: the three factory piano dynamics send 0.35 / 0.30 / 0.18 (pianissimo, mezzo forte, fortissimo) and the five register patches keep 0.** Softer playing gets the bigger room, because the room is what makes a quiet note sound played rather than recorded in a cupboard, and a loud note gets the smaller one because a hall on top of a fortissimo reads as the hall and not as the playing. Measured: the same patch with `fx_reverb_mix = 0` against it running costs **+0.10 points on the four-voice fixture and +0.21 on the sixteen-voice one** (0.193 % -> 0.293 % and 0.359 % -> 0.569 % of one core), which is a cost of the FMO64 block per sample rather than per voice, exactly as phase 8 measured, and the peak does not move at all (0.123 and 0.282 both ways). The other option, `Engine` asking for a room when a sampled instrument attaches, was considered and declined: it makes the panel's slider jump under the hand when an instrument loads, which is a control lying about its own position. |
| Where do patches live, and which folder may the app write into? | **Decided 2026-11-04: two folders, and the shipped one is read-only to the app.** Factory patches are tracked in `patches/` at the repository root and shipped beside the executable in the release zip; a player's own saves go to `%APPDATA%/Boreaz/patches` (`config::user_patches_dir()`, or `patches-user` beside the app on a machine with no `APPDATA` at all), and the browser lists the shipped voices first with the player's under them. The reason is not tidiness: the moment `patches/` held a shipped voice called Bass, a save typed "Bass" was able to overwrite somebody else's name, and a folder git tracks is a folder where that shows up as a modified file in someone's `git status`. Nothing machine-local belongs in git (section 5), so the folder the machine writes and the folder the repository tracks are different folders by construction - which a test asserts rather than a reviewer remembering. **A factory patch names no instrument**, for the same reason: a library path is a path on one machine, and an empty one is already the loader's rule for "keep the library that is playing". **A patch is complete or it is not a patch.** Every factory file carries all 63 keys, because `adopt_patch` layers a file onto whatever is already playing, so a key left out is not "that key unchanged" but "that key whatever the last patch happened to leave". `tools/factory_patches.py` expands each patch's handful of deltas against `param_table.cpp`, which stays the only place a default lives, and `tests/test_factory_patches.cpp` fails when a parameter row is added and the patch set was not regenerated - the price of the rule, paid on purpose. |
| What does the app remember about the machine, and where does it live? | **Decided 2026-11-04: four names in one file - `%APPDATA%\Boreaz\settings.txt`, read at every start and written at the end of a session that had a window: the audio port, the MIDI port, the sampled library, and the patch that was open.** No parameters, and nothing on exit rewrites a parameter file: closing the application is not a thing that edits a sound, and a patch keeps the bytes the player saved. It is read line by line and it forgives what it does not know - an unknown key is skipped and named, a broken line is reported and the lines around it work - because a start that stops on a line nobody can see has no way out, while the parameter file keeps refusing whole files because a half-read sound is a sound nobody wrote. The newest thing said wins: the prompt, then this file, then the program's default. A remembered port that has gone away warns and takes the system default; a port typed at the prompt still fails, because somebody asked for that port. This pays the phase-9 row's "config file for audio/MIDI device prefs + last patch + instrument path, autosave on exit", and it is why `.env` and this file are separate: one holds a machine's secrets in a build tree, the other holds a player's habits in their own folder.  **Paths inside it are absolute, decided 2026-11-04** (`config::resolved_path()`, asked at the moment of writing): the memory is read back by a start that may begin in any folder, and the patch folder it points into is itself found by walking up the tree. A patch file keeps the name its author typed - one is a machine's note about itself, the other is a voice somebody carries to a different machine, and making the second one absolute would make it stop working there. |
| Is the voice kind a parameter, a number a patch can carry? | **Decided 2026-11-04: no. It is which library is playing, and it already travels - as a name, twice.** A patch names its library in the header the browser reads before it plays anything, and adopting the patch puts that name in front of the same queue the Load button uses: a patch that names nothing says nothing about the instrument and keeps the library that is playing, and a patch whose library has gone away keeps its sixty-three parameters and prints the reason in the panel's own line. The app's setup file names the library the next start opens. Making the kind a row would mean a value that can be true about a sound that is not happening: `voice_kind = sampled` with the samples missing describes a library nobody can load, and the phase-9 row has refused that shape of wrong since it was written (a missing instrument falls back to the synthesiser with a warning, never silence). It would also owe the engine a meaning - no parameter selects what a voice *is*, and the sampled path has no filter, no modulation matrix and no oscillator to switch. Nothing was invented for the criterion, and the criterion is met: `tests/test_live_params.cpp` for the three rows, `tests/test_params_panel.cpp` for the name that changes the kind, `tests/test_app_settings.cpp` for the name that survives a restart. |
| Which word is the modulation envelope, to the person holding the mouse? | **Decided 2026-11-04: “Mod env” is the word everywhere — `mod_env_attack_s`/`_decay_s`/`_sustain`/`_release_s` in a file, `ModSource::kModEnv` in the code, “Mod env attack” on the row, “Mod env” in the cable list.** It reverses the 2026-11-03 call (labels only, keys stay `env2_*`), and the reason for reversing is the same reason the old call was made: renaming is free until something writes the name down, and phase 9 is about to write it into saved patch files. Leaving `env2_*` in the file while the panel says “Mod env” would have kept two vocabularies for one thing, and the engine already has a third — the envelope a player hears as the second one is the amp envelope. **The break is loud, which is what made this safe to do:** `params_file.cpp` refuses an unknown key, so an old `.ini` gets `line 1: 'env2_attack_s' is not an engine parameter. Known keys are listed in --help` and the app does not start — measured, not assumed. Nothing in the repo wrote those keys (`config/params.ini`, the fixtures and the goldens never mention them), so the only files affected are hand-written configs nobody has asked for yet. `env1_*` still has no row: the amp envelope stays `attack_s`/`decay_s`/`sustain_level`/`release_s`, because that is the one a player already reads without a qualifier. |
| Latency target | **"Feels instant"** — no hard ms number. Print actual period/rate; tune later. |
| Stereo | **Mono voices → stereo FX bus.** Reversing this later is expensive, so it is decided now. |
| Zones | **None.** One patch across all 88 keys. Stays in the optional list. |
| `.mid` playback | **In scope** as a feature (phase 10), built on the phase-4 parser. Playback only: no editing, no arranging. |
| GUI layout | Keyboard along the bottom (all 88 keys), switchable visualiser panel above it, **slim transport strip always visible** above that. Layout sketch in §1. |
| Keyboard width | **All 88 keys, always, no windowing/scrolling.** Visualiser lanes are 1:1 with keys. Removes the octave-windowing idea from the GUI phase. |
| Waterfall source | **Display tap + voice snapshot. No look-ahead**, so notes scroll up out of the keyboard line rather than falling from the top. Look-ahead stays an optional add-on. |
| Spectrum shape | **Bars with a falling peak-hold line.** Not an oscilloscope (a chord is a scribble) and not a spectrogram (needs a colour ramp and scrolls like the waterfall, so it adds motion rather than pitch information). Both live in the optional list. |
| Spectrum source | **Master output, post-FX**, taken by an audio tap: the panel shows what comes out of the speakers, so reverb, distortion and the noise between notes are all in the picture. Per-key voice amplitude was rejected — it is a note display wearing a spectrum's clothes, blind to FX and to anything between the notes. |
| Spectrum axis | **Log, spaced from `KeyboardLayout`**, so bands sit 1:1 over the keys like the waterfall lanes. Linear Hz was rejected: a piano stops at 4.2 kHz, so three quarters of a linear axis is empty, and nothing above C8 is hunted here — the aliasing report is where spurs get chased. |
| FFT thread | **The frame loop computes, the audio thread only taps.** Two FFTs per frame is a fraction of a millisecond against a period deadline, and a stall on the UI thread costs a frame instead of a click. A dedicated analyser thread is a thread, a lifetime and a shutdown path bought for arithmetic the frame budget already affords. |
| Sample playback | **In.** The old "no sample playback" non-goal is deleted. Boreaz ships one recorded instrument; it is not a general sampler. |
| Instrument | **Salamander Grand Piano v3** (CC-BY-3.0, Alexander Holm), **mono subset ~100 MB, ≥ 4 velocity layers** — and the two cannot both hold: measured 2026-11-01, 4 velocity layers plus the 88 chromatic key-off recordings is 184 MB of FLAC and about 147 MB of mono 16-bit WAV, 3 layers with the same releases is about 111 MB. Keeping ~100 MB means dropping a layer or the key-off noise, and `tools/mkinst.py --dry-run` prints either choice before anything is fetched. Two upstreams, read on 2026-11-01, and neither is what this row used to say. **(a)** the original `SalamanderGrandPianoV3_44.1khz16bit.tar.bz2`, **488.7 MB**, archive.org, published sha1 `ec5a179a1218a590d2f88728f56b583e79876896`, md5 `bf8a7782f4e995a7beecd1e9110bb16f`: a flat directory of `<Note>v<layer>.wav` plus `harmL*.wav`/`harmV3*.wav`/`harmV5*.wav`, **no SFZ and no loop points in it**. **(b)** `github.com/sfzinstruments/SalamanderGrandPiano` @ `3382bf9496bba2486f5ab0de55a264d1dfc38404`: the same 641 recordings as 48 k/24 **stereo** FLAC, 748.4 MB in total, as **ordinary git blobs - not LFS** (a raw URL per file, averaging 1.17 MB; the 394 MB in the row above is what a 16-bit stereo decode of all 641 costs) **plus the mapping as authored data** - `Data/notes.txt` gives 16 `<group>`s with their own `lovel`/`hivel` bands, `Data/region.txt` the 30 minor-third regions, `Data/hammer.txt` the chromatic hammer-noise releases, `Data/str_res.txt` the string resonances, `Data/pedal.txt` the pedal noise. **(b) is the one to build on: it has the velocity bands and the release groups someone else already measured.** A third file, `SalamanderGrandPianoV3_OggVorbis.tar.bz2`, **78.0 MB** (sha1 `5b0247d375ff55aea8a58f775b448e4c4c0e642a`), is all 641 recordings at 44.1 lossy, and it is the cheapest way to get every layer if bandwidth starts to matter. **Built 2026-11-01, and the build corrected the arithmetic in this row: the 88 key-off recordings cost 3.5 MB of the 147.3 MB (2.4 %), so "~100 MB means dropping a layer or the key-off noise" was wrong — dropping every release saves 2.4 %, and the ~100 MB target means two velocity layers, not three.** Measured out of `boreaz-sfz-probe` on the built subset: **208 files, 120 regions playable, 88 release recordings, 280 MB resident, 0 refused, 509.9 ms**; on disk 208 WAVs, 147.3 MB; 184.1 MB of FLAC fetched. The subset ships at `volume=+6` on its 120 note regions (upstream's `set_cc7 100`/`amplitude_oncc7` are opcodes this engine does not read), which lands the listening ladder at **0.6941** against the shipped upright's **0.6121** at the same 0 dB master gain. |
| **Loops** | **Not available, and this row used to promise "loops kept".** Neither upstream carries loop points: the original ships bare WAVs, the SFZ carries no `loop_*` opcode, and the only loop data in either family is the CC0 upright's own mapping. Salamander's recordings are long, so a held note plays to the end of its file instead of a seam, which is a different behaviour and must be measured as one (the 8 s hold in `tools/listen.py`). Looping the flagship means *computing* loop points in `mkinst`, which is a task with its own risk and is not implied by anything here. |
| Instrument format | **SFZ read at runtime, plain SFZ only.** But `mkinst` is not "subset and stamp provenance", which is what this row used to claim: **(b) has no `<region>` in it at all** - the master `Salamander Grand Piano V3.sfz` is 120 lines of `#define`s, `<control>`, `<global>` and four `<master>` switchboards (`sw_last` keyswitches for natural/retuned, `amplitude_oncc7`, `amp_veltrack_oncc$VELTRACK=-100` with `set_hdcc$VELTRACK=0.73`, `label_cc*`) and the regions arrive through **sforzando-only `#include`** directives: `<group> #include "Data/vel_01.txt" lovel=1 hivel=26 #include "Data/region.txt"`. So `mkinst` **assembles** that into one flat plain-SFZ: resolve `#define`/`$VAR` (`$EXT`, `$VEL`, `$TUNEnn`, `$OFFnn`), inline the includes, bake the CC-controlled opcodes at their upstream defaults (`amp_veltrack = round(-100 × 0.73) = -73`), drop what a sampled voice cannot do (`offset_oncc`, `*_curvecc`, `<curve>`, the keyswitch pair - natural tuning only, `rt_decay`, `note_polyphony`) **and print every one it dropped**. The loader still parses nothing but plain SFZ, which is the part of the original decision that survives. |
| Tuning for samples | **Linear 12-TET, always.** A recorded piano is already stretched, so stretched tuning and inharmonicity are **rejected for sampled voices** (they stay legal for synth voices if ever wanted). |
| Notes outside A0–C8 | Pitch-shift the nearest edge sample, keep the `EXT` flag. No silence, no fake key. |
| Sample rate and mode | **Ask for 48 kHz on shared WASAPI** (phase 3, measured). The dev box's default endpoint is a 192 kHz endpoint, so "use the native rate" meant 2.7 ms deadlines and 9 % of a core at 4 voices instead of 3 % — and nothing audible to buy. `--report` prints the rate and period actually delivered. Exclusive mode stays a possible flag, never the default. |
| What counts as a glitch | **A gap of 2 or more periods**, which is a buffer the callback failed to fill. The counter also reports 1.5–2 period gaps, and the dev box lives there (~15 % of gaps at 512/48 kHz, none reaching 2): that is ring jitter, not a dropout. A `>1.5` count is a tripwire, not the exit criterion — quoting it alone once made a clean run look like 15 % glitches. |
| Engine cost per voice | **Fixed in phase 8, and the diagnosis here was right while the fix named here was wrong.** The cost was the 2 `std::sin` calls per sample per voice: **48.0 ns each against 2.9 ns** for a 4096-entry table read, which is ~80 % of what a voice costs. Same build directory, medians of 5: `pad-chords.mid` at 16 voices **6.07 % -> 1.14 %** of one core offline, and on the audio thread — the number the criterion uses — **10.9 % -> 2.32 %** at 48 kHz / 1024 frames. So the `< 8 % of one core` budget that has failed since phase 3 passes for the synthesiser with 3.4x margin, and recorded voices stay where phase 9 left them (0.43 % per voice, golden byte-identical). **polyBLEP is therefore a sound fix, not a CPU fix**: the headroom it was supposed to buy already exists. The old numbers stay below as the history they are: ~0.5 % of one core per sounding voice at 48 kHz (1 voice 1.55 %, 4 voices 3.14 %, 16 voices 9.40 %), which is what the sine cost before it was a table. |
| Asset in git | **Never.** Text recipe in, binary asset out, release artifact only, no git-lfs. |
| `Leethring/piano-sound-samples` | **Evaluated and rejected as the instrument** (MuseScore-rendered 128 kbit MP3, 1 velocity layer, no release samples, ~5 s cut tails, 5.5 dB key-to-key level spread). May be used as throwaway test data only. |
| Per-key gain normalisation | **Not required** for Salamander (properly voiced); `mkinst` reports per-region peak so a lumpy library is visible. |
| A region's own loop points beat the file's `smpl` chunk | **Kept, 2026-11-01.** The mapping is the author's instruction for that region and `smpl` is the file's default, which is also what other SFZ engines do; it is documented at `include/boreaz/samples/sample_store.h:33`, asserted in `tests/test_sample_store.cpp`, and a handoff note calling it backwards is not evidence. A region that gives points the wrong way round is warned about and played once rather than falling back to `smpl`, so the precedence never hides a broken mapping. |
| The master bus runs `tanh(x)` on every block, at a drive nothing can set | **Still kept as it is, and now it can be: phase 8e put a limiter in front of it.** The curve is no longer the thing holding the promise, so its 1.82 dB at `0 dB` master is level the limiter is free to hand back with the fader rather than a ceiling. |
| The master bus used to be the limiter, and a promise nothing can break is not a test | **Landed 2026-11-02 (phase 8e).** `dsp::Limiter`, ceiling 2.0, after the fader and in front of the curve. The 1.82 dB the `tanh` charges at `0 dB` master is now level the limiter hands back instead of level the curve deletes, and the promise has an owner that can be tested: 66303 samples pinned at full scale became 0, and the bit-identity at the shipping `-12 dB` is asserted with a bypass hook rather than claimed. See §8, phase 8e. |
| The distortion is a driven stage in front of a clipper that nothing can set | **Measured (phase 8d), and it is not what the limiter is for.** `dsp::Distortion` is `tanh(drive·x)/drive`: small-signal slope 1.0, peak `tanh(drive)/drive`, so turning the drive up can only ever pull peaks down - 13.2 dB of peak against 1.8 dB of body on the flagship's hardest fixture. The level rule therefore still passes at a full send and drive 10, and what the limiter still owes this table is the thing the row above is about: the 1.8 dB a fixed `tanh` charges at `0 dB` master. |
| Licence | **PolyForm Noncommercial 1.0.0** (`PolyForm-Noncommercial-1.0.0`) on Boreaz's own source: free to use, study, change and share non-commercially; selling it or commercial use needs a separate agreement. `LICENSE` is byte-identical to the SPDX canonical text. Bundled code and the sampled instrument keep their own terms in `NOTICE` — the CC-BY-3.0 piano is deliberately **not** re-covered by the non-commercial clause. MIT stays available later: while the authorship is sole, relicensing is one commit. |

Still open, none of them blocking phase 0:

1. **Which velocity layers** of Salamander's 16 — **answered 2026-11-01 by the ear: v1, v6, v10,
   v14**, the four `tools/mkinst.py` keeps by default. Upstream's bands (1-26, 47-50, 73-80,
   105-112) leave velocity 0 and three gaps uncovered, and this engine says so out loud, so the
   subset re-cuts them to 0-34, 35-64, 65-96, 97-127. Those three splits are what
   `tools/listen.py`'s velocity ladder deliberately walks over. What the four layers cost is in
   §1's asset table; the decision was 280 MB resident or a quieter piano, and the piano won.
2. **Release region groups** — **answered: the 88 chromatic hammer noises are in, the minor-third
   string resonances and the pedal noise are not.** Not by ear alone: the releases are **3.5 MB of
   the 147.3 MB (2.4 %)**, so the size argument against them never existed, and they are the part
   of a piano the upright cannot demonstrate at all — the shipped CC0 instrument has **no release
   regions**, so `tools/listen.py` on it proves the pool is wired up, not that a hammer noise
   sounds right. String resonance is the next candidate and it is a different question: it wants to
   be *held* rather than one-shotted, which §6 of phase 6c parked behind one flag on `VoiceSample`.
3. **Who runs `mkinst`** — we publish the 147 MB subset as a release artifact, or the user
   runs the subsetter after downloading the upstream library. Publishing is friendlier and
   puts the redistribution on us, which CC-BY allows.
4. **A/B patch compare** — parked in the optional list.
5. **Where the two analyser windows cross over**, and whether the bottom two octaves are worth
   showing at all when they resolve to one rising shape. Both are measurements to take while building
   it, not guesses to make now.
6. **Whether it feels right.** `python tools/listen.py` renders six things to `build/listen` on the
   shipped instrument and prints how loud each came out, because "it sounded quiet" should be
   answerable with a number. Three things it measured, none of them a bug, all of them decisions
   nobody has made with an ear yet:
   - the two layers really do switch at velocity 81, on the real files: middle C's soft recording is
     `C4vL.wav`, its loud one is `B3vH.wav` centred a semitone below and played a semitone up, and the
     rendered waveforms match those files' own zero-crossing rates either side of 81 (0.0211 against
     0.0150).
   - a held note keeps going: 0.0182 rms in the first second, then flat at 0.0025 / 0.0021 / 0.0023
     over the 3-4, 5-6 and 7-8 second windows of an 8 s hold, dropping to 0.0008 after the key-up.
     That is the loop doing its job, and it is 18 % of the attack's level: how long a real piano holds
     is a recording question, but whether that reads as "alive" or "thin" is an ear.
   - velocity spans **30 dB** on one key (0.0049 peak at v=15 against 0.1556 at v=127), so soft
     playing is very quiet, and there is a step in the middle where the recording changes hands. Both
     are normal sampler behaviour and both may still be wrong for this piano.
   - **it is quiet, deliberately, by 12 dB.** Every one of the six came out between **0.1132** and
     **0.2070** peak, which is -19 to -14 dB full scale and thin on a laptop: `master_gain_db` starts
     at -12 because sixteen summed voices need headroom. Two hands now reach that level and both are
     measured rather than assumed. `--gain DB` on all three front ends is the same table the slider
     uses (so `--gain 100` refuses with `wants -72 to 12`, and `--gain loud` refuses for being a word),
     and the pack renders at `--gain 0` by default: the same six files land at **0.4109** to
     **0.8186**, the instrument's own ceiling is **0.6974** at 0 dB, and at +12 dB it is **0.9979**,
     which is the soft clipper and not another 12 dB of piano. CC7 per MIDI channel is the other hand
     — linear, 0 to 127, heard by the note already held — and it was previously decoded, named, and
     thrown away, so a file asking for 100/127 played at full level: that is why one golden moved,
     from **0.382302** to **0.306908** on `lead-simple.mid`, which is 100/127 with the clipper
     compressing a little less than proportionally. Whether -12 is the right thing to ship, given that
     the ear has now heard it at 0 dB, is still open.
   - **`volume=` is a pure gain stage; the output peak is not a level.** Measured 2026-11-01 because
     the flagship's `volume=+6` only moved the output 4.7 dB and someone had to know whether the store
     or the dB reference was wrong: it is neither. Render the same file at `--gain -40`, where the
     master-bus `tanh` is a straight line, and `volume=+6` against `volume=0` measures **+6.02 dB**;
     at `--gain 0` the same pair measures **+4.71 dB**. The difference is the clipper, which runs
     `tanh(x)` on every block at drive 1.0 - nothing in this repository calls `SoftClip::set_drive`
     outside `test_gain.cpp`, so drive is a knob with no dial. The curve costs 0.03 dB at 0.1
     pre-clip, 0.45 dB at 0.4, **1.82 dB at the 0.8545 the flagship's loudest transient reaches**
     (which is what arrives as 0.6941), and the error against a linear render is -14.5 dBFS peak /
     -26.3 dB rms - a distorted grand, not a quieter one. Two numbers follow and both are worth
     remembering before anyone tunes by ear again: `tanh(1.0) = 0.7616`, so the output ceiling at
     drive 1.0 is **-2.37 dBFS** and the `clamp(-1, 1)` after it is unreachable; and `drive_peak`,
     which is what the GUI meters, is the **pre-clip** number, so a meter saying 0.85 next to a file
     peaking at 0.69 is two honest measurements of two different places. Where it matters is decided
     by the master gain, not by the instrument: at the shipped `-12 dB` that same transient is 0.2145
     pre-clip and the curve costs **0.13 dB**, so `volume=+6` ships at a level where the clipper is
     not in the way, and the 1.82 dB only arrives if someone turns the master up to 0 dB by hand.
   - **8e: and the limiter landed in front of it, which retires the row above.** `dsp::Limiter` is an
     instantaneous-attack / one-pole-release peak limiter with a linked stereo gain, sitting after the
     fader and **in front of** the `tanh`, which is the only place in the chain where it can engage:
     a limiter behind a `tanh` at drive 1.0 is a box in a diagram, because the curve's own output
     cannot reach 1.0. Measured at the top of the master row (`--gain 12`, 88-key glissando) the old
     chain wrote **66303 samples of 1565550 pinned at exactly +-32767** - 4.2 % of the file, at a peak
     1.66 dB below full scale, pinned anyway because that is what a curve does to a transient. With
     the stage: **0.964020, none pinned**, and the file is a scaled copy of itself instead of a
     square wave top. The ceiling is **2.0**, which is a property of the curve behind it and not of
     full scale, because `tanh` is nowhere near 1.0 at the levels this engine actually reaches: a signal
     at 1.0 leaves it at 0.7616 (**-2.37 dBFS** - a cap at 1.0, the obvious number, is a ceiling 2.37 dB
     under full scale and throws away most of the master row), at 1.5 it leaves at 0.9051 (-0.87 dBFS),
     at 2.0 at 0.9640 (-0.32 dBFS), at 3.0 at 0.9951 (-0.04), and it only rounds to exactly 1.0 by 9.02.
     At 2.0 the composite output ceiling is `tanh(2.0) = 0.9640`, the curve is asked for 6.34 dB of
     compression it is happy to give, and the top of the fader buys **1.51x the rms for 1.16x the peak**
     (0.8279/0.2003 → 0.9640/0.3021, i.e. the crest factor goes 12.33 dB → 10.08 dB) — not the 1.0x peak
     a limiter with makeup gain would promise, because this stage deliberately has no makeup gain: it
     stops the pinning, it does not pretend to make the file louder. At the top of the fader **every** committed fixture, not just the
     glissando, lands on 0.964028 - `lead-simple`, `bass-running-status`, `pad-chords` too - so the top of
     the fader is a ceiling now, not a limiter that taps occasionally and hopes. At the shipping `-12 dB` the stage is not asked for anything -
     the four committed synthesiser fixtures peak at 0.6479 (glissando-88), 0.6047 (pad-chords), 0.3069
     (lead-simple), 0.2993 (bass-running-status), and the shipping recorded upright at the same `-12 dB` peaks at
     **0.281859** - a long way under an internal ceiling of 2.0 - and not one of those renders prints a
     `limiter held` line at all - and at `+12` the same upright render lands on 0.964028 like everything else.
     `Engine::bypass_limiter()` proves what that costs rather than promising it: the same file rendered with and without the stage differs in
     **0 of 96 000 samples**, which is also the only reason that hook exists - a matching golden after
     a new stage is otherwise just evidence the golden was refreshed. Cost measured the same way,
     **0.187 vs 0.167 % of one core** (median of 5, 16 voices), i.e. 0.02 points. The argument
     against, in its own words: the release is a moving gain and a moving gain is phase modulation,
     so a 100 Hz tone at 1.5x the ceiling comes back with **14.9 % third harmonic**; the `tanh` it
     relieves reports **23.5 %** at the same level, and both reach ~30 % by 4x the ceiling, where
     nothing is a tone any more. `drive_peak()` now meters the **demand** rather than the result,
     because after a limiter the output peak is a constant the stage guarantees and a meter on top of
     a guarantee reports nothing: 0.7 is a mix with room, 3.0 is a mix being held down by a dB and a
     half, and the GUI clip light already reads this scalar.

## 8. Commands (phase 0 onward)

```bash
cd "$(git rev-parse --show-toplevel)"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/boreaz --version         # works from phase 0 on
./build/boreaz --list            # audio + MIDI devices
./build/boreaz --device 1 --latency 256    # output by index or name, buffer in frames
./build/boreaz --midi 0          # keyboard by index or name from --list
./build/boreaz --demo --report   # no keyboard needed: plays a written part, prints the counters
./build/boreaz --demo --chord 16 --report  # the 16 voice load the CPU budget is quoted against
./build/boreaz --range           # print the 88-key compass: A0 27.5 Hz .. C8 4186.0 Hz
./build/boreaz                   # start the app (sampled piano if an instrument is set)
./build/boreaz --no-instrument   # synth only
./build/boreaz --instrument build/piano/instrument.sfz
./build/boreaz --play demo.mid   # app, playing a MIDI file
./build/boreaz-render demo.mid out.wav demo.params   # .mid + params file -> 16 bit stereo .wav
./build/boreaz-render --gain 0 demo.mid out.wav      # ... or one knob, 12 dB hotter, no file
./build/boreaz-render --golden demo.mid              # one line of measurements, which is what a golden is
#   ^ --gain works on all three front ends and takes its range and its refusal from the same table as
#     the slider: --gain 100 answers "wants -72 to 12" and exits 2. A DSP change moves the golden, and
#     the last one was CC7: only lead-simple.mid asks for a level (CC7=100), which used to be decoded
#     and dropped, so its peak fell 0.382302 -> 0.306908. That is 100/127 = 0.787 with the sum
#     compressing a little less once quieter. Both golden files were regenerated in that one commit.
python tools/midifile_fixtures.py --verify "boreaz midifile fixtures"   # the .mid fixtures match their recipe

# the CC0 starter instrument --instrument plays: 66 recordings + one 2 s demo WAV, 54.7 MiB, ~12 s, nothing written in the repo
python tools/mkinstrument.py --build compact --out build/assets/instruments/boreaz-cc0 --force
./build/boreaz-sfz-probe build/assets/instruments/boreaz-cc0/UprightPianoKW-20220221.sfz
#   ^ 66 regions playable, 0 release recordings, 109 MB, 0 refused - and it names every opcode it ignores
#   ^ Both numbers used to be quoted for this instrument and only one of them is the same thing: the
#     66 WAVs under `samples/` are 54.7 MiB of 16-bit stereo on disk (57.4 MB in decimal, which is
#     the "57 MB" this line used to carry) and 109 MB of float32 in RAM, and the "67 WAVs" counted
#     the release's own 2 s demo file, which `mkinstrument.py` decodes and nothing plays.

# things to listen to, on that instrument, with no audio device involved (renders through boreaz-render)
python tools/listen.py                        # build/listen/*.wav + how loud each one came out
python tools/listen.py --gain -12             # ... at the level the app comes up with, not 0 dB

# the flagship: build the Salamander subset from upstream's own mapping (network, 184.1 MB of FLAC in)
python tools/mkinst.py --dry-run   # cost before bandwidth: layers, bands, regions, MB in and out
python tools/mkinst.py             # ffmpeg needed; cache in build/mkinst-cache, output third_party/instruments/salamander
./build/boreaz-sfz-probe third_party/instruments/salamander/salamander.sfz
python tools/listen.py --instrument third_party/instruments/salamander/salamander.sfz
#   ^ 208 files, 120 regions playable, 88 release recordings, 280 MB resident, 0 refused, ~480 ms.
#     It assembles, it does not copy: upstream's `.sfz` wraps its mapping in `<master>` key switches,
#     `locc20/21/22` gates and `*_oncc` opcodes this engine does not read, so the tool writes the
#     regions this engine can honour and says what it dropped in `provenance.txt`.
#     Measured on it, at 0 dB master: `volume=0` renders the velocity ladder at 0.4036, `volume=6`
#     at 0.6941, against the shipped upright's 0.6121. At `--gain -40` the same pair is 6.02 dB
#     apart, which is the proof that the missing 1.3 dB is the master-bus `tanh` and not `volume=`
#     (see §7, "`volume=` is a pure gain stage"). Tune anything by that measurement or by
#     `drive_peak`, never by an output peak that is already inside the curve.
# the gate, in one command: build, the suite, both render paths, and the release behaviour in audio
python tools/release_check.py           # prints the window table; exit 0 means every gate ran
```

**The render-contract gate compares values, and until 2026-11-02 it did not.** It ran `--golden` on
`one.mid` twice - synthesiser and sampled - and asserted only that a line containing `fnv1a64=` was
printed, which is true of every render this tool has ever produced at any level: `--gain 12` moves
that hash from `e124adf1851914b9` to `ff6d22238f543655` and the peak from 0.142584 to 0.964028, and
the gate passed both. Every sentence in this file that says the committed hashes are what a level
change trips over was true of the numbers and false of the gate. They are now constants in
`release_check.py` (`GOLDEN_SYNTH`, `GOLDEN_SAMPLED`, hash and peak each) and the gate exits 1 when
either moves - checked by making it move, not by reading the code. The two values are the shipping
`master_gain_db` of `-12`; raising the shipped default, which §7 says is an open question, has to
change them, and that is the point of writing them down.

Toolchain facts, measured in phase 0 rather than assumed:

- **Ninja + GCC (MinGW-w64 UCRT)** is the primary configuration. Clang is built as a second
  build directory; both have to stay warning-free because our own targets compile with
  `-Werror`. Dependency headers are included as *system* headers so third-party warnings
  cannot break our build.
- **A sanitizer gate nobody builds silently stops gating — and this one came back.** `build-clang`
  (the ASan tree) had stopped compiling on toolchain drift alone: VS 18's STL headers mark `sscanf`
  deprecated and clang-cl turns that into an error under `-Werror` (`src/tools/render/main.cpp`);
  two tests carried unused names (`test_io.cpp`, `test_midifile.cpp`); and `test_tuning.cpp` hit an
  incomplete `std::basic_ostream` inside the STL, because doctest stringifies a `string_view` and
  clang-cl instantiates that `operator<<` where `<ostream>` has to have been seen. All four are
  fixed, and the suite is green under AddressSanitizer again: **233 cases, 7 934 953 assertions,
  `ASH_NO_THREAD_SAFETY_ANALYSIS=1 ASH_OPTIONS=no_leaks=true ASH_VSEH=0 ./build-clang/boreaz_tests.exe`**
  (leak detection is off on Windows; `ASH_VSEH=0` because miniaudio's SEH frames confuse ASan).
  Say "sanitizer clean" only against a run like that one.
- **A golden is a promise from one compiler, so there are now two files.** `fnv1a64` moves under
  clang-cl because it contracts multiply-add and the MSVC build does not: the same four fixtures
  render to the same `frames`, the same `peak` to six decimals and the same `steals`, and a
  different hash — checked field by field before this was believed. `tests/golden/midifile-clang.txt`
  holds the clang measurements and the test picks the file by `__clang__`; a DSP change means
  regenerating **both**, in one commit, or one of the two trees starts failing loudly.
- **A stale executable is a witness that will say anything.** A `build/Debug/` tree belonging to a
  different working copy answered `boreaz_tests`, `boreaz_bench` and `boreaz-render` questions for
  most of one session, and its numbers went into a commit message and into this file: an "engine"
  test suite (`-ts=engine`, which selects nothing here — the only suites in this tree are
  `gui frame view` and `computer keyboard`), a `boreaz_bench` with 47 benchmarks and a "3286
  assertions" total (no bench source, no bench target, no commit on any branch with one),
  `--headless --ticks=N`, `--asset-root`, a `boreaz-cc0` preset name, `-ts=release`, and audio
  measurements of the release feature. None of those commands exist in this repository. Two habits
  follow: run the command with `--help` before quoting it, and `command -v`/`ls -l` the binary
  you are about to believe. Totals move with every phase, so the number to believe is the one the
  binary in front of you prints (**283 cases / 8 861 608 assertions** today) in
  `boreaz_tests`, and `boreaz --help` and `boreaz-render --help` are the CLI contract.
- **Executables link `libgcc`/`libstdc++`/`libwinpthread` statically.** This machine has two
  MinGW runtimes on `PATH` (`mingw64` and a bundled one) and the loader takes the first
  match, so a dynamically linked binary can die at load with `0xc0000139` — an
  entry-point-not-found with no line number, far from any real bug. Static linking also
  keeps the shipped exe portable, which is the point of "lightweight".
- **Only `third_party/` is committed empty.** The rest of the §3 tree is created by the phase
  that first fills it. A CMake binary directory is git-ignored and skipped by the hygiene
  test *by the presence of `CMakeCache.txt`*, not by directory name: CMake caches are full of
  this machine's absolute paths, and a build tree called `b` or `_build` is still a build
  tree. Related habit, because it has already caught two things: run `ctest` once from a
  fresh clone with a non-default `cmake -B <dir>` before calling a phase done.
