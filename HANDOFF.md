# HANDOFF — 2026-11-02 (seventh pass)

State of the session, what was decided, and what to do next. Read PLAN.md alongside this; where the
two disagree on a number, this file is newer and PLAN is the thing that needs fixing.

## Where the tree is

| | |
|---|---|
| develop | **`f7ff4a6`** — PRs **#32** (the drive stage), **#31** (the CPU, measured), **#30** (the room waits for presets), **#29**, **#28** (the gate runs the app), **#27** (the real-time counters), #26 (the room) |
| open PR | none. **This repo has no CI at all** — `origin` is GitHub, `gh api …/actions/workflows` is empty, `gh pr checks` says "no checks reported". The gate is `python tools/release_check.py`, run locally, and that is the only thing between a branch and develop |
| current branch | `develop` |
| instrument on disk | `third_party/instruments/salamander/` — 208 WAVs, 147.3 MB, gitignored |
| gate | `python tools/release_check.py` → `OK: every gate ran` (now including the app on a real device); `ctest --test-dir build` → 1/1, `ctest --test-dir build-asan` → 1/1; **no golden moved** |
| instruments on disk | also `third_party/instruments/upright-piano-KW/` — 66 regions, 109 MB, the gate's default `--instrument` |
| tests | **252**, 17 in `tests/test_fx.cpp` (7 delay + bus, 5 room, 3 drive, 6 limiter) |

**Phase 8, one slice at a time.** In: `dsp::Delay` (ring per ear, 0.5 s, integer tap, feedback ≤
0.95), `dsp::Reverb` (4 damped combs per ear on one shared mono excitation + 2 all-passes per ear,
30 Hz leak in every loop), `engine::FxBus` (skips itself when every send reads 0),
`Engine::render_chunk` = voices → **FxBus** (`drive → delay → room`) → master gain → `tanh`, and
seven table rows (`fx_distortion_mix/drive`, `fx_delay_mix/time_ms/feedback`,
`fx_reverb_mix/seconds`). Still out: the mod router and polyBLEP — and nothing you hear changed,
because every send ships at silence.

## What phase 8 costs (measured 2026-11-02, PR #31)

Two instruments, both now in the tree: **`boreaz.exe`'s exit line prints `cpu_fraction()`** (the
callback's own busy ns over the wall time the device ran) and **`boreaz-render` prints `engine work …
% of one core`** on stderr, clocking the engine call alone. Probes: `build/cpu_measure.py` (device,
interleaved medians), `build/offline_cpu.py` (offline, 5 runs), `build/room.ini`, `build/delay.ini`,
`build/room-delay.ini` — all gitignored.

```
the load is pad-chords.mid: the only committed fixture that reaches the pool (16 voices, 8 steals).
glissando-88, the fixture every older real-time claim used, peaks at 7 voices.
                                  offline (median of 5)   device 48 kHz / 1024
sampled instrument, dry                    0.43 %              1.09 %
  + room, full send                        0.59 %              1.39 %
  + room and delay                         0.69 %           1.14-1.57 %
synthesiser, dry                           6.81 %              8.03 %
  + room                                   7.12 %              8.63 %
4 voices (lead-simple): sampled 0.23 -> 0.42 with the room; synthesiser 1.34 -> 1.58
```

- **The bus costs per sample, not per voice**: the room is +0.16-0.24 points and the delay +0.02-0.04
  at 4 voices *and* at 16. A heavier FX rack cannot blow the budget by adding polyphony.
- **The shipping path is 1.1-1.4 % with everything on** — six times inside PLAN's 8 %.
- **The synthesiser is at the budget and has been since phase 3** (8.03 % here, 9.40 % in PLAN's
  phase-3 row). It is not phase 8's doing, and §7's diagnosis is true: `synth_voice.h` builds
  **`osc1_`/`osc2_` as `Waveform::Sine`** — two `std::sin` per sample per voice, `step()`ed even when
  the oscillator's level reads 0 — while `Waveform::Saw` is one FMA. So **polyBLEP is a CPU saving**;
  re-measure this row when it lands. `oscillator.h`'s `sin` is only reached for `Sine`.
- Offline sits under the device number (0.43 vs 1.09) for the same config: engine-only in a hot loop
  versus the same call with a driver and a poller on the core. Unexplained arithmetically; use the
  device figure for claims and the offline one for A/B comparisons in the same hour.

## What the drive stage is, and its numbers (measured 2026-11-02, PR #32)

`dsp::Distortion` is **`tanh(drive·x) / drive`**, first in the bus. Two properties were the whole
reason for that curve, and both are asserted, so do not "improve" the curve without reading them:

- **small-signal slope exactly 1.0** → colour, not level. The difference the bus adds is the cubic
  term `d²x³/3`, which is why the wet line on a 0.01 note is 3e-6 of that note.
- **a peak comes out at `tanh(drive)/drive`** → the stage can squash and **can never lift a peak**,
  at any mix, over the whole 1-10 range. That is why the level rule still passes with a full send at
  drive 10, and why the limiter inherits one fewer job.

The wet line carries **`shaped - in`, not `shaped`**, so `dry + mix·wet` is a crossfade between the
clean note and the driven one. Carry `shaped` instead and mix 1 at low drive is the sum of two
nearly identical signals: a 6 dB jump, and the bus-level test catches it at 1e-6.

```
glissando-88, sampled, master -12 dB          peak       rms      crest
dry                                       -16.86 dBFS  -37.38   20.53 dB
drive 3, full send                        -22.11       -37.75   15.64
drive 8, full send                        -30.07       -39.21    9.14
drive 8, mix 0.5                          -21.13       -38.28   17.15
```

Peak -13.2 dB against rms -1.8 dB. **The curve predicts a real render to 0.05 dB**: the pre-fader
peak is 0.572, `tanh(8*0.572)/8 = 0.102`, the render measures -30.07 dBFS. Third partial of a 0.9
sine: 5.6 % at drive 1 → 31.3 % at drive 8, fifth 16.6 % (a square wave is 33 % / 20 %, so drive 10
is a fuzz).

Cost, median of 5, glissando (7 voices): engine call 0.29 % dry → 0.38 % driven; audio thread
48 kHz/1024 0.74 % → 0.94 %; all three effects 1.43 %. 16 sampled voices 0.42 % → 0.49 %.
**+0.09 offline, +0.20 on the device** — per sample, not per voice, like the room.

Two bugs this found, both fixed: **`boreaz-render --golden` refused a parameter file while its own
`--help` advertised one** (so the mode that prints measurements could only print the defaults — now
takes `IN.mid [PARAMS]`), and the drive's own first test asserted a harmonic ratio against the
*block's* fundamental, which for a 16000-sample block is 3 Hz and measures nothing. Correlate at
`cycles` times the bin, not at the bin.

## The room's numbers (re-deriving these costs an hour)

All asserted in `tests/test_fx.cpp`; probes live in `build/reverb_rms.cpp`, `build/reverb_decay.cpp`,
`build/ab_reverb.py` (all gitignored, all disposable).

```
full send, held 0.4 A3, 1.8 s room :  -13.5 dB vs the note (rms, 1 s in)   peak -10.2 dB
                                      0.5-1.5 s after the key: -33.6 dB (peak)
aperture                            first non-zero wet sample 1215 frames in (one lap of comb 1116)
RT60 asked vs measured (mid-band)   1.8 s -> 1.60 s  |  12 s -> 7.63 s  (damping + leak take a
                                      little off every lap, and feedback clamps at 0.995)
worst case, mix 1.0, near-DC 0.5    12 s room: 0.90 unleaked  ->  0.14 with the 30 Hz leak
                                      the leak also costs 1.4 dB of send level
flagship, master -12 dB, mix 1.0    total rms +0.16 dB, peak 0.0545 -> 0.0530 (does NOT rise)
  lead-simple.mid                   quietest 100 ms: 0.000000 -> 0.000128 (-77.9 dBFS of room)
                                    0.3-0.8 s after the last note: 0.000063 (-84.0 dBFS) vs 0.0 dry
```

## The reverb default is decided: it waits for presets

PLAN's risk table said the reverb is **"on by default for the piano"**; it ships at `mix = 0`.
**Decided 2026-11-01 (the user's call): it stays 0 and the room arrives as a patch.** No golden is
regenerated, the app's startup sound is untouched, and the debt moves to **phase 9: a factory piano
preset with `fx_reverb_mix = 0` counts as that risk row still being open** — written into §6, §7 and
the phase 9 row so it cannot be quietly forgotten when the patch browser is built. `Engine` asking
for a room when a sampled instrument attaches was considered and declined: a control that jumps
under the hand when something else loads is a control lying about its own position.

## Decisions taken (do not re-litigate without new evidence)

- **4 velocity layers, 280 MB resident, accepted.** PLAN says so in §1, §2, §3, §5, §6, §7, §8.
- **Level lives on the instrument.** `volume=+6` on the note regions, `volume=-31` on the 88 releases
  (upstream's own −37 + 6). `master_gain_db` default stays **−12 dB**; `--gain` defaults to 0 dB.
- **The master-bus `tanh` at drive 1.0 stays.** Nothing sets drive, the clamp behind it is
  unreachable, and that is now a row in PLAN's decision table: it costs **0.13 dB** on the flagship's
  loudest transient at the −12 dB the engine starts with, 1.82 dB only at 0 dB master. Raising or
  exposing drive moves both goldens to buy headroom nothing asks for; the FX-phase limiter should
  own this once.
- **A region's own loop points beat the file's `smpl` chunk.** A handoff note called it backwards;
  the header, the store test and PLAN §6a all call it deliberate. Kept, and written into the table.
- **`mkinst` assembles, never copies.** Key-off noise **does not** steal the note it follows.
- **The FX return is inside the fader, and an effect is a send.** Bus before the master gain, dry
  never attenuated to make room for the wet. `mix=0` means the bus is *not called*, which is what
  lets the goldens prove it changed nothing.
- **Effect parameters stay out of `Params::operator==`.** The bus reads them every block; counting
  them there would refresh every voice on every knob move.
- **The room's size is RT60 in seconds, and the leak inside the comb loops is not optional.** A 12 s
  line is a water tank at 20 Hz: unleaked, the longest room at a full send accumulated to **0.90 of
  full scale** from a near-DC input. 30 Hz takes the DC, costs 1.4 dB of level, and is why the knob
  reads 1.6 s where 1.8 was asked. Removing the leak to gain 1.4 dB buys a reverb that has to be
  clipped.

## The thing that used to be missing 1.3 dB

Same file (`velocity-ladder.mid`), `volume=6` against `volume=0`:

| master gain | v0 | v6 | Δ |
|---|---|---|---|
| 0 dB | 0.4036 | 0.6941 | **+4.71 dB** |
| −40 dB (tanh ≈ a straight line) | 0.004272 | 0.008545 | **+6.02 dB** |

`volume=` is exact. The whole difference is `SoftClip`, which runs `tanh(x/drive)·drive` every block
at **drive 1.0** — nothing outside `test_gain.cpp` calls `set_drive`. Loss: 0.03 dB at 0.1 pre-clip,
0.45 dB at 0.4, **1.82 dB at the 0.8545 the flagship's loudest transient reaches** (arriving as
0.6941); error against a linear render **−14.5 dBFS peak / −26.3 dB rms**. `tanh(1.0)=0.7616`
= **−2.37 dBFS**, so full scale is unreachable by turning up and the `clamp(-1,1)` after the tanh
never fires. `drive_peak` (the GUI meter) is **pre**-clip, the WAV peak is post-clip.

**Rule: compare levels at `--gain -40`, or read `drive_peak`.** Every peak already written in PLAN,
in commit messages, and printed by `boreaz-render` is post-clip.

## The real-time promise, measured (this session's whole point)

PLAN said "zero file I/O or page faults in the callback" from phase 1 and counted it never. §3
parked the counter in a `--diagnostics` build that does not exist, so `boreaz-render --counters`
reads this process's own page-fault and IO counters **in a window around the engine call and nothing
else**, and prints loading / first 32 blocks / every block after them.

```
flagship 141 MB, glissando-88 (3058 blocks)   loading    104196 faults,   209 reads
                                              first 32         9 faults,     0 reads
                                              the rest      1-2 faults,     0 reads, 0 writes
four fixtures x {synth, flagship}, 16 runs     file reads 0 in EVERY block window
                                              post-warm-up faults 0-14 per render, worst of 27
                                              renders (2 once, 14 the next run of the same command:
                                              noise, not a path)
boreaz.exe, real device (VB-Cable, 1024),      72 callbacks, 0 late, 0 dropped, 0 steals
  58 MB library, sampled from outside          0 file reads, 151 page faults (all soft)
boreaz.exe, Realtek speakers, flagship sweep   0 late of 833 at 1024 | 256 late of 1665 at 512
```

**The claim is therefore: no file I/O in the callback, ever** — and a live callback does take page
faults (151 in 1.5 s, driver threads included) while still being **0 late**, because none of them
wait for a disk. `tools/release_check.py` fails if reads or writes go nonzero, or if the post-warm-up
fault count reaches four digits; late callbacks are printed and never asserted (that is the Realtek's
512-frame behaviour, reproduced exactly, and a gate that fails on power-state changes is a gate
nobody trusts).

## Numbers that cost something to re-derive

```
.\build\boreaz-sfz-probe.exe third_party\instruments\salamander\salamander.sfz
  208 files, 120 regions playable, 88 release recordings, 280 MB resident, 0 refused, ~475 ms
  on disk 208 WAVs / 147.3 MB; fetch 184.1 MB FLAC; key-off recordings are 3.5 MB of it (2.4 %)
  the CC0 upright for comparison: 66 regions, 109 MB, 157 ms, 54.7 MiB on disk

loops:  0 of 208 regions loop, because upstream has none. 20 of the 120 note recordings end inside
  5 s (shortest A7v1 2.63 s, all top two octaves) and every file ends at -40 dBFS or quieter
  (loudest last-20 ms: 0.0110). Held C4, rms at 3-4 / 5-6 / 7-8 s: subset 0.0047 / 0.0035 / 0.0042,
  upright 0.0098 / 0.0082 / 0.0092. Nothing clicks, nothing dies inside the audible tail.
```

## Do next

1. **The limiter is built, on a branch that is not merged yet.** `feat/the-ceiling` @ `30375dc`,
   one commit off `f7ff4a6`; the PR **must** be opened `--base develop` (this checkout's default
   branch is `main`) and the base read back from the reply. The remote is `AgentDudu/boreaz` —
   `gh api` against the wrong owner 404s and proves nothing. It sits **behind** the `tanh` rather
   than replacing it, and it applies no gain at all, so the reason to merge it is the top of the
   fader: 66 303 pinned samples → 0, and the top of the fader buys 1.51× rms for 1.16× peak (no makeup gain, by design, so the peak does move a little: +1.32 dB). See
   PLAN §8.1 and the row in §6. **Next after it: the mod router, then polyBLEP** (the two phase-8
   items with no code yet, and polyBLEP is a CPU *saving* — see §7's engine-cost row).
   The one thing NOT to copy from the `tanh` argument: the limiter is the last stage, so it is the
   one effect that cannot be a bypassed send without a level jump; it has no parameters, by design.
2. **The rest of phase 8's exit criteria**: per-voice modulation < 1 op/sample at defaults (the mod
   router is not started), and CPU < 8 % — **met by the shipping sampled path** (1.1-1.4 % with all
   three effects) and **not met by the synthesiser** (8.03 %), which is a phase-3 number and wants
   polyBLEP. Measure anything level-related at `--gain -40`, never at a post-`tanh` peak.
3. **The panel.** `--gui` ran once by hand (window opened, file played, killed after 6 s) and nobody
   has *looked* at 25 rows drawn from one table — the drive's two sliders arrived with nobody
   touching the panel, which is the point of the table, and nobody has seen them either. PLAN's
   phase 7 criterion that `--gui` run **with no device** is still unmet: `main()` opens the device
   first and returns 1 when that fails, so the panel cannot be proven on a machine with no driver.
4. **Still open in PLAN §7:** who runs `mkinst` (we publish the 147 MB artifact, or the user does);
   whether −12 dB is the right thing to ship now the ear has heard 0 dB; string-resonance releases,
   which want to be *held* rather than one-shotted — one flag on `VoiceSample`, not a third pool;
   and the reverb default above.

## Gotchas, all paid for

- **A comb output is a spike train.** Peak and rms of the same room tail differ by 20 dB and more,
  so every room claim has to be an rms-over-a-window claim; the first version of the level assertion
  used a peak and passed for the wrong reason. `window_rms` in `test_fx.cpp` is there for this.
- **A filter inside a feedback loop changes the decay**, not just the colour. The leak moved RT60 by
  11 % at 1.8 s; the "seconds" arithmetic in `feedback_for` cannot see it, so RT60 is *measured* off
  the tail in a test and the header quotes the gap.
- **`python - <<'PY' … s.replace(a, b)` fails silently** when `a` contains `\n` written for the file
  it is rewriting (the shell heredoc eats one level). Three "edits" in this session did nothing and
  reported success. Verify with `sed -n`, or write the whole file.
- doctest has no tags in this project: `-ts=reverb` selects **nothing** and exits 0 with "SUCCESS".
  Use `-tc=` with a pattern, or no filter at all.
- **A `--help` string that lists the parameter names is a second source of truth.** `boreaz-render`
  had one and it was five names behind. It now walks `param_table` (`src/tools/render/main.cpp`).
- **`set_params` does not arrive, it travels.** `engine.cpp` one-poles *every* parameter toward its
  published target at `kParamSlewSeconds = 10 ms`. An engine-level test that asks for 120 ms and
  measures 30 ms into the render sees the glide (it looks exactly like an effect that loses level).
  Burn 250 ms of silence — `kSettleFrames` in `tests/test_fx.cpp` — then count samples.
- Event ticks are **absolute** samples, so arming a note after a settle means `kSettleFrames + n`.
- A test buffer sized for the *first* run and fed by the longest one is a heap overflow that reports
  as `0xC0000374` in some later test: `test_fx.cpp`'s decay case runs 30 000 frames, and the crash
  showed up 200 tests away from the write.
- `ctest --test-dir build/fx` (a build dir two levels deep) fails `config/params.ini`: that test looks
  in `cwd` and then `../`. Use `--test-dir build`.
- SFZ comments in this engine are `//` and `/* */`. **`;` is not a comment here.**
- SFZ velocity ranges are inclusive and **start at 0** (`lovel=1` leaves velocity 0 uncovered).
- A `<group>`'s opcodes may sit on **bare lines under the header** (`Data/hammer.txt`).
- `Data/notes.txt` puts `#include` **inside** a `<group>` line, twice, and relies on `#define`s
  persisting across them.
- **`GetProcessMemoryInfo` is a macro to `K32GetProcessMemoryInfo`.** In C++ the macro saves you; in
  ctypes `windll.kernel32.GetProcessMemoryInfo` raises `function not found`. Ask for the K32 name.
  `IO_COUNTERS` has no `cb` member and ctypes structures want `_fields_`, one trailing underscore.
- **`boreaz.exe song.mid` exits by itself** when the file's tail has rung out (`if (window == nullptr)
  break;`), so the app has always been scriptable. The old note in `release_check.py` saying it was
  not was wrong, and is now corrected there.
- **`release_check.py` runs its children with `cwd=build/release-check`.** Pass **absolute** paths to
  `--binary-dir` and `--instrument` or the sampled gates fail with "cannot open …".
- Counting page faults from outside a running app attributes the whole process to the audio thread.
  Fine for "did anything read a file", not fine for anything per-thread: the 151 includes winmm
  polling, the C++ runtime, and driver threads.
- PowerShell needs `.\build\boreaz-sfz-probe.exe …`; bash needs `./build/...`.
- `third_party/` is gitignored, so the built instrument is never reviewable in a PR; only the tool is.
- `gh pr merge 23` needs an explicit `--merge` (non-interactive) and this repo keeps merge commits
  and remote branches — do not `--squash` or `--delete-branch` to look tidy.
  **#27 and #28 were squashed and their branches deleted anyway** (GitHub keeps them as
  `refs/pull/N/head`, and `7ac2862`/`c9cf801` are still in the object database, so the individual
  commit messages survive only on the PR pages). develop's content is complete either way; use
  `--merge` from here on so the history matches the rule.
- Python 3.14 here: `wave.open()` wants a file object, not a `Path`, and `Wave_read` has no `getpos`.

## Commands

```powershell
python tools\mkinst.py --dry-run                 # cost, no bandwidth
python tools\mkinst.py                           # build the subset (ffmpeg; cache in build\mkinst-cache)
.\build\boreaz-sfz-probe.exe third_party\instruments\salamander\salamander.sfz
python tools\listen.py --instrument third_party\instruments\salamander\salamander.sfz
python tools\mkinstrument.py                     # the shipped upright, from the FreePats tar
ctest --test-dir build --output-on-failure
python tools\release_check.py                    # the whole gate, one command
```
