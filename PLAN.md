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
full width) → switchable visualiser panel → slim transport strip that never disappears**.
Licence: MIT unless overruled. **Boreaz is also a sample player for a real recorded grand
piano** — that decision and its consequences are in §1 "The piano (sampled)".

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

| Item | Decision |
|---|---|
| Upstream build | **44.1 kHz / 16-bit WAV, 394 MB** (not the 48 k/24 1.18 GB build) — no resample step, smaller download |
| Shipped asset | `boreaz-mkinst` **mono subset, ~100 MB, ≥ 4 velocity layers** of the 16, loops preserved |
| Format | **SFZ read at runtime** (plain text, both good libraries ship it). No home-grown format, no converter |
| In git | **Nothing binary.** The 394 MB source and the ~100 MB subset never enter the repo; only the `mkinst` recipe (text) is committed |
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
| MIDI I/O | **miniaudio** `ma_midi` (winmm backend) | removes RtMidi as a second dependency |
| GUI | **Dear ImGui + GLFW + OpenGL3** | immediate mode, zero state, knobs/meters in an afternoon |
| Tests | **doctest** (single header) | fast unit tests + offline render assertions |
| `.mid` parsing | `midicpp` (header-only) — **licence + C++20 support unverified, re-check before use**; fallback `minimidi` | needed by both the offline renderer and in-app playback |
| Instrument | **Salamander Grand Piano v3**, 44.1/16, mono subset ~100 MB (CC-BY-3.0) | a real recorded grand is the only route to "sounds like a piano" |
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
1. **Params** — UI owns a `Params` struct, on change makes a copy and atomically swaps a
   pointer. Audio thread reloads it once per block. No locks, never per-sample.
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
- Preload is capped by the ~100 MB instrument budget. Print resident MB and load time at
  startup, and refuse to start quietly when the asset blows the budget.
- Per-voice resampling is **cubic at minimum** — the library is minor-third spaced, so most
  keys are pitch-shifted up to ±1.5 semitones. Linear interpolation is audible on a grand.

### Directory layout
```
include/boreaz/        public headers (engine types, Params, Event)
src/dsp/               osc, filter, adsr, math, waveshaper   (no I/O, no alloc)
src/engine/            Voice, VoiceAllocator, Engine, Params, EventQueue, Tuning (128-entry LUT)
src/io/                miniaudio device, MIDI input, ring
src/gui/               ImGui app, widget pages
src/gui/viz/           panel switcher + one file per visualiser (transport, waterfall)
src/app/               main, CLI flags, wiring
src/offline/           MIDI-file → WAV renderer (test/tool target)
src/midifile/          .mid parser static lib (shared by offline + seq)
src/sfz/               SFZ subset reader: text parse + region inheritance + opcode subset
src/samples/           SampleBuffer store (resident), SampledVoice, cubic resampler, release pool
tools/mkinst/          offline subsetter: Salamander → ~100 MB instrument + NOTICE
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
| 0 | `feature/project-scaffold` | CMake project, FetchContent pins, dir tree, `.clang-format`, `.gitignore`, `.env.example` (empty values only), warnings-as-errors, empty `boreaz` exe linking miniaudio/GLFW/imgui/doctest | clean build with GCC **and** Clang; `boreaz --version` prints; `ctest` runs 1 trivial test; a grep gate fails the build on a machine-absolute path, a committed `BOREAZ_*` value carrying real data, or a model name in any tracked file |
| 1 | `feature/dsp-primitives` | `Tuning` 128-entry LUT + `note_name()`, `Oscillator` (sine/saw/square/tri, phase accumulator), `SVF` filter (LP/HP/BP, resonance, per-block coeffs, double-precision coeffs), `ADSR` (accurate exponential-ish, retrigger-correct), `Gain`/`SoftClip` | unit tests: freq accuracy < 0.5 cent **at A0, C4, A4 and C8** (not 440 Hz alone); LUT holds 27.5 Hz and 4186.009 Hz and is monotonic over all 88 keys; `note_name(21)=="A0"`, `note_name(60)=="C4"`, `note_name(108)=="C8"`; ADSR segment durations ±1 sample; filter passes DC at 0 resonance and stays stable/bounded at A0 + max resonance; no NaN/denormal after 10⁶ samples |
| 2 | `feature/voice-engine` | `Voice` = 3 osc + noise → mix → SVF → ADSR amp (one class here; phase 6 splits synth/sampled behind it, so keep the boundary clean). `VoiceAllocator`, pool of 16, steal oldest+quietest. Note on/off, velocity, pitch bend, sustain, all-notes-off, transpose/octave. `Engine::process(block)` | render 16-note chord + held pedal: exactly 16 voices, no stuck notes after panic, output peak ≤ 1.0, deterministic (same MIDI + params → byte-identical WAV); **full 88-key chromatic sweep A0→C8 renders with no silence, no NaN, and peak ≤ 1.0 on every key**; notes 0–20 and 109–127 are accepted and reported as extended range |
| 3 | `feature/live-audio-midi` | miniaudio device open/choose, `ma_midi` input, SPSC event ring, real-time safety pass, CLI flags (`--device`, `--midi`, `--list`, `--latency`), auto-restart on device loss | real keyboard plays with no audible glitch at 512 samples period; CPU < 3% of one core; unplug/replug MIDI does not crash or leak voices |
| 4 | `feature/offline-render` | `boreaz-midifile` static lib (parser + tempo/time-sig/event merge, shared with the app), `boreaz-render` tool: `.mid` + params file → `.wav`; golden-file fixtures for engine states | parser licence + C++20 support re-checked and recorded here; multi-track + tempo-map file renders correctly; CI-comparable renders stable across runs; four fixtures (mono lead, pad, bass, **88-key chromatic glissando A0→C8**) committed with a documented "regenerate" command |
| 5 | `feature/gui-shell` | GLFW window + ImGui context inside the audio-coupled loop; device/audio status; CPU% + voice + clip meters; **on-screen keyboard with all 88 keys, A0–C8, always full width**, middle C marked, `EXT` keys dimmed, feeding the *same* event ring; **switchable visualiser panel** = one enum + one table, with the transport view as the first entry | window at 60 fps while audio runs; all 88 keys individually clickable with no scrolling or paging; adding a visualiser costs one enum value + one file; closing window exits cleanly with device stopped |
| 6 | `feature/sample-playback` | `src/sfz/` reader: `<control>`/`<group>`/`<region>` inheritance over a **declared opcode subset** (`lokey/hikey`, `lovel/hivel`, `pitch_keyroot`, `offset`, `loopstart/loopend/loopmode`, `volume`, `tune`, `amp_veltrack`, `trigger=release`, `group`). `SampleBuffer` store fully resident at open; `SampledVoice` = read head + cubic resampler + loop + gain; `trigger=release` regions served from a separate small pool; velocity crossfade between layers; `boreaz-mkinst` subsets Salamander 44.1/16 → mono, ≥ 4 layers, ~100 MB, emitting `NOTICE.txt` from the pinned recipe; `--instrument file.sfz`, `--no-instrument` | the subset plays **A0→C8 with audible dynamics**; a synthetic SFZ fixture (generated sine samples) covers the reader in unit tests so **CI never downloads licensed audio**; **zero file I/O or page faults in the callback** (no handle open while processing, glitch counter 0 across a full 88-key sweep); held notes sustain **past 5 s** through a loop seam with no click; note-off fires a release sample and the main voice still dies on time; unknown opcodes warn once and never abort a load; resident MB + load time printed and inside budget |
| 7 | `feature/parameter-ui` | `Params` panel wired by a single descriptor table (name, range, default, unit) → knobs/sliders auto-generated; param smoothing (zipper) so no clicks on jump; range controls: transpose ±24, octave ±4, tuning ref 415–466 Hz, current-key readout; **voice-kind (synth / sampled) + instrument selector, switchable live** | every engine parameter reachable; transpose/octave changes retune held notes without a click and cannot push a target frequency outside 0–Nyquist; a jump 0→1 on cutoff produces no click (verified by golden diff); one new param costs one table row; switching synth↔sampled with notes held neither clicks nor leaks a voice |
| 8 | `feature/modulation-fx` | Mod router: fixed source list (Env2, LFO1, LFO2, Velocity, KeyTrack, Wheel) × destination list, 8 slots, bip+unipolar depth. `KeyTrack` is normalised over the 88 keys with **C4 (60) = 0.0**, A0 = −1.0, C8 = +1.0. FX bus: distortion, stereo delay, freeverb-style reverb. polyBLEP oscillator replaces the naive saw. **Sampled voices carry no filter and no modulation matrix — amp and pan only — but they do go through the FX bus**, which is where a dry grand becomes an instrument | per-voice modulation costs < 1 op/sample at default settings; wet/dry bypass A/B is exact for `mix=0`; CPU budget still < 8%; the piano with reverb off vs on is a documented A/B, since a dry sample is not a shippable sound |
| 9 | `feature/presets` | save/load patch files (`key = value`, versioned header), factory patches (one per register: sub/bass/low-mid/upper/brilliance, each written for the A0–C8 compass, plus piano presets at several dynamics), patch browser, config file for audio/MIDI device prefs + last patch + instrument path, autosave on exit | patch round-trips byte-for-byte, including transpose/octave/tuning-ref/voice-kind/instrument; a missing or unreadable instrument **falls back to the synth with a visible warning**, never silence; corrupt/unknown-key file loads with warning and keeps sane defaults instead of dying |
| 10 | `feature/seq-playback` | in-app `.mid` playback using `boreaz-midifile`: transport (play/stop/pause, position, loop), tempo from the file + user override, sequencer clocked by the **audio thread sample counter** and pushing into the same event ring, keyboard + playhead follow, `--play file.mid` | a `.mid` plays in time with no drift against the audio clock over 3 minutes; stop and panic leave zero stuck notes; seeking/looping retrigger correctly; a file with >16 simultaneous notes plays as specified stealing, no crash; CPU stays inside the phase-8 budget; notes outside A0–C8 flagged `EXT`, not dropped (on the piano these are pitch-shifted edge samples, which must sound wrong enough to be honest) |
| 11 | `feature/note-waterfall` | **Note waterfall** in the switchable panel: 88 lanes 1:1 with the keyboard, notes leaving the keyboard line and scrolling up, length = held duration, brightness = velocity, colour per MIDI channel, scroll-speed + lane-height controls, toggle kept in the panel switcher. Source is the display tap + voice snapshot — **held and recent notes, no look-ahead**, so nothing falls in from the top | lanes line up with the keys below them to the pixel (geometry assertion test); a 5 ms staccato note still appears (proves the tap, not frame polling); 40 concurrent notes over a glissando file drops zero notes at 60 fps; panel costs < 2 ms/frame and introduces **zero** audio glitches (glitch counter stays 0); switching to transport or back does not stop, reset or retrigger playback |
| 12 | `feature/polish-release` | error surfacing (no silent `return`s at startup), panic button, latency readout, window icon, single-binary release build, README with build + first-sound steps + how to fetch/subset the instrument, **`NOTICE.txt` and about-box attribution generated from the recipe that built the shipped subset** | a stranger follows README and hears sound on a clean Windows machine from the release zip; the attribution in the zip matches the instrument actually in the zip (CI check, fails the build if not) |

Optional, only if asked for: Linux/ALSA + macOS build, virtual MIDI
port, MIDI clock/sync (MIDI time code / external clock), automation/param OSC, CI release
artifacts, A/B patch compare, keyboard zones (lower/upper split or layer across the 88
keys), a modal/inharmonic synth piano (only if sampling is ever abandoned), **waterfall
look-ahead** (notes falling in from the top, fed by the `.mid` timeline — needs a `VizSource` abstraction and would only
apply during file playback).

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
- No machine-absolute paths in tracked files. No `gpt5.4-codex`/`glm-5.2` in names, comments,
  logs, or fixtures. `.gitignore` covers `build/`, `out/`, `*.wav` except fixtures, IDE dirs
  and `.env`.
- **No binary assets in git.** Neither the 394 MB instrument source nor the ~100 MB subset.
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
| miniaudio's `ma_midi` is thinner than RtMidi (winmm only, no virtual ports, weak SysEx) | prove it in phase 3; if it fails, swap `io/` to RtMidi — engine untouched |
| WASAPI shared-mode latency surprises / device format resampling adds cost | prefer exclusive WASAPI at device's native rate; print the *actual* period + rate, not the requested one |
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
| 88 lanes + 40 quads per frame starves the audio thread of CPU | draw-list batching, no per-frame allocations, fixed < 2 ms/frame budget with a measured report; the glitch counter is the arbiter, not opinion |
| Naive oscillators sound cheap and alias | phase 8 polyBLEP; keep the naive version behind a clear `shortcut:` comment |
| CC-BY-3.0 needs attribution **and "indicate changes"** on a subset we modified | `NOTICE.txt` and the about-box credit are **generated** from the pinned `mkinst` recipe; CI fails a release whose instrument has no matching attribution |
| 394 MB upstream download for build and CI | pinned URL + sha256 in the recipe, cached, fetched by `mkinst` only; unit tests use the synthetic SFZ fixture, so CI needs no download |
| SFZ's opcode surface is open-ended (ARIA/OPCODEX extensions) | implement a **declared subset**; unknown opcodes warn once and are ignored; a region we cannot honour is refused loudly at load, never mis-played silently |
| `trigger=release` regions fight note-off voice reuse | release samples live in their own small pool outside the 16, so they survive the voice that spawned them |
| Bad or absent loop points → a click every cycle, or a note that dies at 5 s | `mkinst` verifies loop seams (zero-crossing / short crossfade) and writes loop points where the source has none; audible-click test over the 10 longest notes |
| ~100 MB preload is slow to start | measure and print load time in phase 6; only if it hurts do we reconsider mmap + warm-up, which reintroduces page-fault risk |
| A dry grand sample sounds worse than a good synth patch | reverb bus is **on by default** for the piano, and the off/on A/B is a phase-8 exit criterion |
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
| Sample playback | **In.** The old "no sample playback" non-goal is deleted. Boreaz ships one recorded instrument; it is not a general sampler. |
| Instrument | **Salamander Grand Piano v3** (CC-BY-3.0), 44.1/16 build, **mono subset ~100 MB, ≥ 4 velocity layers**, loops kept. |
| Instrument format | **SFZ read at runtime.** No home-grown index, no format converter; `mkinst` only subsets and stamps provenance. |
| Tuning for samples | **Linear 12-TET, always.** A recorded piano is already stretched, so stretched tuning and inharmonicity are **rejected for sampled voices** (they stay legal for synth voices if ever wanted). |
| Notes outside A0–C8 | Pitch-shift the nearest edge sample, keep the `EXT` flag. No silence, no fake key. |
| Asset in git | **Never.** Text recipe in, binary asset out, release artifact only, no git-lfs. |
| `Leethring/piano-sound-samples` | **Evaluated and rejected as the instrument** (MuseScore-rendered 128 kbit MP3, 1 velocity layer, no release samples, ~5 s cut tails, 5.5 dB key-to-key level spread). May be used as throwaway test data only. |
| Per-key gain normalisation | **Not required** for Salamander (properly voiced); `mkinst` reports per-region peak so a lumpy library is visible. |

Still open, none of them blocking phase 0:

1. **Licence** — MIT by default (my assumption until you say otherwise). Nothing is
   published, so deciding late costs nothing; deciding before phase 0 means the scaffold
   commits carry a `LICENSE` file.
2. **Which velocity layers** of Salamander's 16 go into the ~100 MB subset, and where the
   crossfade boundaries sit. A listening decision, made in phase 6, not now.
3. **Release region groups** — include the chromatic hammer-noise and minor-third string
   resonance releases in the first subset, or ship attacks first and add them once the
   release pool exists? They cost size and they are most of the realism.
4. **Who runs `mkinst`** — we publish the ~100 MB subset as a release artifact, or the user
   runs the subsetter after downloading the upstream library. Publishing is friendlier and
   puts the redistribution on us, which CC-BY allows.
5. **A/B patch compare** — parked in the optional list.

## 8. Commands (phase 0 onward)

```bash
cd C:/Users/Rafael/VSC_Projects/boreaz
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/boreaz --list            # audio + MIDI devices
./build/boreaz --range           # print the 88-key compass: A0 27.5 Hz .. C8 4186.0 Hz
./build/boreaz                   # start the app (sampled piano if an instrument is set)
./build/boreaz --no-instrument   # synth only
./build/boreaz --instrument build/piano/instrument.sfz
./build/boreaz --play demo.mid   # app, playing a MIDI file
./build/boreaz-render demo.mid demo.params out.wav

# build the shipped instrument from the upstream library (network, ~394 MB in, ~100 MB out)
bash tools/mkinst/mkinst.sh tools/mkinst/salamander.recipe   # pins URL + sha256, writes NOTICE.txt
```
