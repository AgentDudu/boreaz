# perf: the sine is read from a table, and that is where the CPU was

## What this changes

`waveform_at(Waveform::Sine, phase)` stops evaluating `std::sin` and starts reading a
**4096-entry `float` table with linear interpolation**. Five files, 162 insertions, no engine
file touched: the oscillators, the voice, the allocator and the router never knew how a sine got
made, and they still do not.

`detail::sine_exact` is a constexpr 13-term Taylor that stops when a term becomes zero, so the
table is `.rodata` built by the compiler — 16 KiB, no generator script to keep honest, nothing at
runtime to initialise. The index is masked with `kSineSize - 1` rather than `%`: `phase` may land on
exactly 1.0, where `phase * 4096.0f` is 4096.0, and 4096 % 4096 is the same wrap the phase counter
already performs, so entry 0 is read instead of running off the end of the array.
`static_assert(1 << 12 == kSineSize)` is what stops someone resizing the table into a discontinuity
at the top of every cycle of every voice. The read is `std::fma(fraction, next, here - fraction *
here)`, which is one multiply instead of two and is why `here - here * fraction` did not become
`here * (1.0f - fraction)` and cost a subtraction.

The table is at namespace scope and **not** a function-local `static`. A local static looks tidier
and puts an initialisation guard on every call: the first probe, written that way, measured 4.46 %
instead of 1.1 %.

## Why, with the numbers

One binary, `-O3`, medians. The A/B was done **in one build directory** by stashing the header and
rebuilding, because an earlier attempt to compare a second build tree turned out to compare an
unoptimised configure against an optimised one and showed a 9x win that was a flags artefact.

| | `std::sin` | the table |
|---|---|---|
| one `waveform_at(Sine, phase)` call | 48.0 ns | **2.9 ns** (nearest-neighbour: 2.7) |
| `pad-chords.mid`, 16 synth voices, offline render | 6.07 % of one core | **1.14 %** |
| `lead-simple.mid`, 4 synth voices, offline render | 2.28 % | **0.34 %** |
| `pad-chords.mid` on the device, 48 kHz / 1024 frames | **10.12 – 11.50 %** | **1.58 – 2.35 %** |

- **`std::sin` was about 80 % of what a synthesiser voice costs.** PLAN §7 diagnosed "2 `std::sin`
  calls per sample per voice" back in phase 2 and was right; it then named polyBLEP as the fix, and
  polyBLEP would have fixed the *saw* and left the sine exactly where it was. An entire FX rack —
  room, delay, drive, eight cables — costs 0.2 CPU points. A header with a table in it costs 8.
- **The criterion that has failed since phase 3 now passes.** `< 8 % of one core` was measured at
  8.03 % for the synthesiser on this machine (9.40 % in PLAN's phase-3 row, 6.81 % in HANDOFF's
  phase-8 row). It is **2.32 %** here. Today's baseline measured 10.12–11.50 %, higher than the
  8.03 % on record, which is machine variance pointing the same way; the pair above is same-day and
  same-directory, so the ratio is the trustworthy part.
- **polyBLEP is a sound fix, not a CPU fix.** The headroom it was supposed to buy now exists before
  it lands.

## Accuracy, because a table is a claim

The test samples **half a step off the table's own nodes** — a real voice's phase increment never
lines up with a table entry, so sampling on the nodes would measure the reference's rounding instead
of the interpolation. Worst absolute error over a whole cycle: **4.76837e-7**, bounded at 1e-6. The
`float` reference `std::sin` carries **4.023e-7** of that error itself, so the interpolation adds a
little under one unit in the last place of a `float`. A table four times bigger or interpolation two
steps instead of one cannot help, which is why the table stops at 4096.

Nearest-neighbour was measured, not assumed: 2.66–2.70 ns against 2.89–3.70, so 0.2 ns, and **4.2e-5**
error — eighty times worse. Linear stays. `Oversampler::upsample` already refuses to interpolate when
the gap between two neighbours exceeds 0.5 (it reads a jump as anti-aliasing to be kept), so a naive
`Saw` deliberately does not get the table, and `advance_block_sinc` still suppresses discontinuities
in the 4x oversampled path.

## The goldens moved, in the hash and nowhere else

PLAN's rule is that a golden refreshed quietly has stopped protecting anything, so:

- `tests/golden/midifile.txt`: four `fnv1a64` changed. **Every peak identical** — 0.299295, 0.647880,
  0.306908, 0.604739 — and every steal count identical (0, 0, 0, 8).
- `tools/release_check.py`: `GOLDEN_SYNTH` `e124adf1851914b9 → 08eb2e60c42ca135`, peak still
  0.142584. The comment above it now carries the reason and the date.
- `GOLDEN_SAMPLED` **did not move at all** — recorded voices never ask the oscillator for anything,
  which is the cleanest evidence in this PR that the change is confined to the oscillator.
- The flagship rendered by both binaries and compared sample by sample: of **573 300 int16 samples,
  573 146 are bit-identical and 154 differ by exactly 1 LSB** of 32767 — about −96 dBFS. A
  sample-exact match was never the contract; the contract is "the render is within 1 LSB of 16-bit".
- The 25 `waveform_at` points in `tests/golden/oscillator.csv` are unchanged, so the phase-1
  oscillator contract is still held by exactly the samples that pinned it.

## The finding this turned up, and it is a product question

**No parameter selects a waveform.** `synth_voice.h:37-39` builds `osc1_`/`osc2_` as `Waveform::Sine`
and `osc3_` as `Waveform::Saw`, and there is no `osc1_waveform` in the parameter table. So
`Waveform::Square` and `Waveform::Triangle` have never been reachable from a patch, and the aliased
saw PLAN's shortcut row promises to band-limit reaches the output only through osc3, at the default
`osc3_level` of 0.12. polyBLEP's audible scope is three times narrower than the row implied, and
whether players should be able to pick a waveform at all is a decision for the plan, not for this PR.

## Gates

- 270 doctest cases, 8 859 317 assertions, all passing; the new case pins the table's accuracy claim
  and the seam at phase 1.0, and a `static_assert` pins the mask against the table size.
- `python tools/release_check.py` — **every gate ran**, including the real audio device, the render
  contracts for both paths, and the live-parameter bus.
- Offline CPU, medians of 5, before and after, in the table above.
- `boreaz.exe tests/fixtures/midifile/pad-chords.mid --no-instrument --gain -24 --period 1024` on the
  Realtek: 305 callbacks, 0 late, 0 restarts, 0 dropped, 8 steals (matching the fixture's own golden),
  1.89 % audio-thread CPU.

## Not proven, and known noise

- **Nobody has listened to this.** The case for it is 154 samples at −96 dBFS against a 5x cut in CPU;
  if a listener hears a difference in a sine at 220 Hz, this measurement is wrong somewhere.
- The four `non-case-label-in-switch` findings in `oscillator.h` are **false positives**: a MISRA C
  query reading `case Waveform::Sine:` through a C grammar, which cannot parse a qualified enum label
  and treats it as a label. The identical four lines are in the file at HEAD, unchanged since phase 3,
  and clangd reports the header clean. They are marked as false positives with that reason; the
  alternative was restructuring valid C++ to satisfy a C grammar, which the diagnostic itself advises
  against.
- `synth_voice.h:10` has a **pre-existing** clangd warning — `voice_common.h` is included but not used
  directly. Not this PR's file, not touched, flagged here so it is not discovered later as mine.
- The sampled instrument's own CPU (0.43 % per voice) is untouched and unmeasured here.
