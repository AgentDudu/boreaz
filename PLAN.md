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
Kawai upright, recorded January 2017 by Gonzalo (humanogonzalo@gmail.com) and Roberto
(roberto@zenvoid.org) with a Zoom H1, **CC0 1.0, dedicated by the recorders themselves**
(`LICENSE`, `github.com/freepats/upright-piano-KW`): 66 regions, 2 velocity layers, stereo,
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
  on both engines the post-warm-up count is **0–9 soft faults per render** (the sweep is 3 058
  blocks, so ≤ 0.3 % of blocks, reads still 0), which is the process first touching its own memory
  and not disk — 2 faults on one run and 14 on the next proves it is noise, not a path. So §3
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
| 8 | `feature/modulation-fx` | Mod router: fixed source list (Env2, LFO1, LFO2, Velocity, KeyTrack, Wheel) × destination list, 8 slots, bip+unipolar depth. `KeyTrack` is normalised over the 88 keys with **C4 (60) = 0.0**, A0 = −1.0, C8 = +1.0. FX bus: distortion, stereo delay, freeverb-style reverb. polyBLEP oscillator replaces the naive saw. **Sampled voices carry no filter and no modulation matrix — amp and pan only — but they do go through the FX bus**, which is where a dry grand becomes an instrument | per-voice modulation costs < 1 op/sample at default settings; wet/dry bypass A/B is exact for `mix=0`; CPU budget still < 8%; the piano with reverb off vs on is a documented A/B, since a dry sample is not a shippable sound. **phase 8a and 8b outcome: the bus exists and holds two effects, and the A/B is measured.** `dsp::Delay` (8a) and `dsp::Reverb` (8b, Schroeder: 4+4 damped combs sharing one mono excitation, 2 all-passes per ear) sit behind `engine::FxBus`, which is walked by the engine after the voice sum and before the master gain, so the fader and the clipper are downstream of everything. Four rows in the parameter table (`fx_delay_mix/time_ms/feedback`, `fx_reverb_mix/seconds`), all defaulting to silence, so `FxBus::active(params)` is false and **the whole block is skipped**: 247 tests and every golden are unmoved. The delay is asserted arithmetically (an impulse arrives `n` frames late at exactly the send, the tail reaches *exact* zero, and it holds). The room is asserted as a room: the first sample out is 1215 frames after the first sample in (one lap of the shortest comb, so it is an aperture and not a phase effect), energy falls monotonically in 250 ms windows, the two ears are not the same signal from a single source, and RT60 is a measured thing on the tail rather than a coefficient nobody checked. **A full send sits 13.5 dB under a held note** (rms, 1 s in), and asking for 1.8 s measures as 1.6 s because the damping and the leak each take a little off every lap, which the header quotes instead of pretending the label is exact. The room has a **30 Hz leak inside every comb loop**, added because measuring the worst case found a 12 s room accumulating **0.90 of full scale** from a near-DC input at a full send; leaked it is 0.14, and what it costs is 1.4 dB of send level. Rendered on the flagship at the shipping `-12 dB` master, mix 1.0, 1.8 s: total rms **+0.16 dB**, peak **does not rise** (0.0545 → 0.0530, because a decorrelated tail does not stack peaks), the quietest 100 ms of `lead-simple.mid` goes from **digital silence to -77.9 dBFS of room**, and 0.3-0.8 s after the last note there is **-84.0 dBFS** where the dry render is exactly zero. What is left in the phase: the distortion and mod router, polyBLEP, and `mix = 0` for a *shipping default* being an open decision rather than an inherited one - see §7. **The first never-measured real-time claim is measured:** `boreaz-render --counters` puts the process's page-fault and IO counters around the engine call and prints load / first 32 blocks / the rest, and `tools/release_check.py` asserts the last line reads 0 reads and 0 writes - see §3 for the numbers. The same gate now runs `boreaz.exe` itself: the app, a device, the library, and its counters sampled from outside while the driver pulled 72 callbacks - 0 late, 0 reads. `--gui` has been *run* by hand since: the window opened, the file played and parked its transport, no `no window:` line, killed after 6 s - which proves the window path initialises and nothing else, least of all that the panel is drawn right. Phase 7's criterion that `--gui` run **without a device** is still not met: `main()` opens the device first and returns 1 when that fails, so the panel cannot be proven on a machine with no driver at all. |
| 9 | `feature/presets` | save/load patch files (`key = value`, versioned header), factory patches (one per register: sub/bass/low-mid/upper/brilliance, each written for the A0–C8 compass, plus piano presets at several dynamics), patch browser, config file for audio/MIDI device prefs + last patch + instrument path, autosave on exit | patch round-trips byte-for-byte, including transpose/octave/tuning-ref/voice-kind/instrument; a missing or unreadable instrument **falls back to the synth with a visible warning**, never silence; corrupt/unknown-key file loads with warning and keeps sane defaults instead of dying |
| 10 | `feature/seq-playback` | in-app `.mid` playback using `boreaz-midifile`: transport (play/stop/pause, position, loop), tempo from the file + user override, sequencer clocked by the **audio thread sample counter** and pushing into the same event ring, keyboard + playhead follow, `boreaz -- song.mid` (the file is named positionally; **the part that plays it is already in**, on `feature/file-playback`: the file is loaded once, fed into the same event ring ahead of the audio thread's position, and the transport starts with it and parks at its length — what is left here is the controls, i.e. pause, loop, seek and an open dialog, none of which exist yet) | a `.mid` plays in time with no drift against the audio clock over 3 minutes; stop and panic leave zero stuck notes; seeking/looping retrigger correctly; a file with >16 simultaneous notes plays as specified stealing, no crash; CPU stays inside the phase-8 budget; notes outside A0–C8 flagged `EXT`, not dropped (on the piano these are pitch-shifted edge samples, which must sound wrong enough to be honest) |
| 11 | `feature/note-waterfall` | **Note waterfall** in the switchable panel: 88 lanes 1:1 with the keyboard, notes leaving the keyboard line and scrolling up, length = held duration, brightness = velocity, colour per MIDI channel, scroll-speed + lane-height controls, toggle kept in the panel switcher. Source is the display tap + voice snapshot — **held and recent notes, no look-ahead**, so nothing falls in from the top | lanes line up with the keys below them to the pixel (geometry assertion test); a 5 ms staccato note still appears (proves the tap, not frame polling); 40 concurrent notes over a glissando file drops zero notes at 60 fps; panel costs < 2 ms/frame and introduces **zero** audio glitches (glitch counter stays 0); switching to transport or back does not stop, reset or retrigger playback |
| 12 | `feature/spectrum-panel` | **Spectrum** in the switchable panel: log-spaced bands, one per key range from `KeyboardLayout` so a band sits over the key that plays it, with a falling peak-hold line over the top. Source is the **master output post-FX** — what the speakers get, reverb and noise included — taken by a new **audio tap**: a fixed SPSC ring the callback memcpy's into and overwrites when nobody drains, so it can never wait. `Analyser` lives in `boreaz_dsp` (Hann window, radix-2 FFT, energy summed into bands, dB, peak decay) so the panel stays one file that includes no FFT, and **the frame loop does the maths while the audio thread does the copy**. **Two window lengths**, because one window cannot do both jobs: 8192 samples (~171 ms, ~5.9 Hz per bin) where the bins have to be narrow, 2048 samples (~43 ms, ~23 Hz, about a semitone at C8) where a transient has to be lit now. Peak-decay rate is the one knob, in the `.ini` | on synthetic buffers, no window and no device: a lone −20 dBFS sine lands in the correct band and within 1 band of it for A0, C2, A2, C4, A4, C6, A6, C8, reading within 1.5 dB; sines a fifth apart (220 + 330 Hz) show two bands with a valley ≥ 6 dB deep; silence reads below −100 dB and DC does not light the lowest band; a 20 ms burst lights the top octave within two frames, which is what the short window is for. Real-time: glitch counter **0** across an 88-key sweep with the panel open; a frame loop that stops draining for 10 000 blocks neither blocks nor grows the ring, and the panel says how much it dropped; the analyser prints its own µs/frame inside the < 2 ms/frame panel budget; switching to transport or back does not stop, reset or retrigger playback (inherited from the waterfall) |
| 13 | `feature/polish-release` | error surfacing (no silent `return`s at startup), panic button, latency readout, window icon, single-binary release build, README with build + first-sound steps + how to fetch/subset the instrument, **`NOTICE.txt` and about-box attribution generated from the recipe that built the shipped subset** | a stranger follows README and hears sound on a clean Windows machine from the release zip; the attribution in the zip matches the instrument actually in the zip (CI check, fails the build if not) |

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
| Bad or absent loop points → a click every cycle, or a note that dies at 5 s | `mkinst` verifies loop seams (zero-crossing / short crossfade) and writes loop points where the source has none; audible-click test over the 10 longest notes |
| A 280 MB preload is slow to start | measured: **480 ms for 208 files / 280 MB**, 205 ms for 66 files / 109 MB, printed at startup. That is a one-off on the setup path and it does not hurt, so mmap + warm-up stay rejected — they reintroduce the page-fault risk §3 forbids |
| A dry grand sample sounds worse than a good synth patch | reverb bus is **on by default** for the piano, and the off/on A/B is a phase-8 exit criterion. **The A/B is measured (phase 8b) and the room still ships at `mix = 0`,** because "on by default for the piano" is a decision about one instrument and `Params` has one default for both engines: turning it on moves every golden and changes what the app sounds like when it starts. The two ways to honour this row are a preset layer (phase 7's unfinished half) or `Engine` asking for a room when a sampled instrument attaches; both are a user-visible change to the shipping sound and neither is inherited silently. |
| FetchContent needs network on first build | documented; vendoring stays an option (decision table row) |
| Windows-only accident creeping in | `dsp/` + `engine/` reviewed for `<windows.h>` includes; keep platform ifdefs in `io/` only |

## 7. Resolved / still open

Answered during planning, so nobody re-litigates them in a code review:

| Question | Decision |
|---|---|
| Voice pool | **16, fixed at device-open.** Stealing is a tested behaviour, not an accident. Revisit only if the 88-key sweep fixture sounds bad. |
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
| Engine cost per voice | **~0.5 % of one core at 48 kHz per sounding voice** (dev box, Release: 1 voice 1.55 %, 4 voices 3.14 %, 16 voices 9.40 %). So phase 3's "< 3 %" is met for a melody and a few voices and **not** met by a 16 note chord. The cost is the 2 `std::sin` calls per sample per voice in the phase-2 placeholder oscillator, which phase 8's polyBLEP wavetable replaces — optimising it now would be work on code we are about to delete. Re-measure and restate the budget at phase 8. |
| Asset in git | **Never.** Text recipe in, binary asset out, release artifact only, no git-lfs. |
| `Leethring/piano-sound-samples` | **Evaluated and rejected as the instrument** (MuseScore-rendered 128 kbit MP3, 1 velocity layer, no release samples, ~5 s cut tails, 5.5 dB key-to-key level spread). May be used as throwaway test data only. |
| Per-key gain normalisation | **Not required** for Salamander (properly voiced); `mkinst` reports per-region peak so a lumpy library is visible. |
| A region's own loop points beat the file's `smpl` chunk | **Kept, 2026-11-01.** The mapping is the author's instruction for that region and `smpl` is the file's default, which is also what other SFZ engines do; it is documented at `include/boreaz/samples/sample_store.h:33`, asserted in `tests/test_sample_store.cpp`, and a handoff note calling it backwards is not evidence. A region that gives points the wrong way round is warned about and played once rather than falling back to `smpl`, so the precedence never hides a broken mapping. |
| The master bus runs `tanh(x)` on every block, at a drive nothing can set | **Kept as it is, 2026-11-01.** It is not a bug and it is not a knob waiting to be wired: at the `-12 dB` the engine starts with, the flagship's loudest transient is 0.2145 pre-clip and the curve costs **0.13 dB**, which is below the ear; at `0 dB` it costs 1.82 dB, which is why levels are measured at `--gain -40` and not at the file's peak. Raising the default drive, or exposing it, is a DSP change that moves both goldens to buy headroom nothing currently asks for — the FX-phase limiter is the thing that will actually own this, and it should own it once. |
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
  you are about to believe. This tree's real totals are **233 cases / 7 934 953 assertions** in
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
