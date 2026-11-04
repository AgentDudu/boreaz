# Handoff: phase 12, the spectrum panel

Branch `feature/spectrum-panel`, merged into `develop`. The full account, with the tables, is
`docs/pr-spectrum-panel.md`. This file says what was proved, what was not, and what is left.

## What it is

`boreaz::dsp::Analyser` (Hann, in-place radix-2, 88 semitone bands, dBFS against a -120 floor, a
peak hold that falls at a set rate) fed by `boreaz::dsp::SampleTap` (an 8192-frame SPSC ring the
audio callback writes and never waits on), drained once per frame by the app's own frame loop, drawn
by `src/gui/viz/spectrum.cpp` in the keyboard's own geometry so a band sits over the key that plays
it. Two window lengths, 8192 and 2048, because one window cannot resolve the bottom octaves and
answer a 20 ms transient at the same time.

## What was proved, and how

- **The maths is not in the callback, measured rather than asserted.** An 8192-point transform is
  368-545 us on this machine against a 10.7 ms period. Live, transport panel against spectrum from
  the same song: 1817 vs 1846 callbacks, 280 vs 283 late, **0 restarts, 0 dropped, 0 steals**. Same
  for switching away and back mid-song: still one `finished`, still 0 of everything.
- **Two windows, and the test that says why.** The same 20 ms top-octave burst lights its band on
  2048 and misses on 8192, pushed identically through both.
- **The band map.** -20 dBFS sines land in the band of the note for A0, C2, A2, C4, A4, C6, A6, C8,
  within one band and 1.5 dB. A fifth leaves a valley 6 dB deep. Silence is under -100 dB. DC does
  not light the lowest band.
- **A frame loop that stops is a hole, not a stall.** 10 000 blocks written against a reader that
  never came: the ring stayed 8192 frames, the writer never blocked, and the count says what was lost.
- **A hidden panel cleans up.** The bands and hold lines return to the floor and the next note lights
  its own band, which is the rule the waterfall's exit report forced onto this view too.
- **The knob.** `peak_decay` in `%APPDATA%\Boreaz\settings.txt`: `22.5` prints `hold falls at 22.5
  dB/s`; `banana` prints why it was refused and starts anyway at 14 dB/s. The file keeps all five keys.
- Suite: **314 cases, 8 877 437 assertions**, and `python tools/release_check.py` says `OK: every
  gate ran`.

## What nobody has seen

The pixels. Same limitation as phase 10 and 11: this box's screen capture is black over GL, so the
panel's shape, colours and text have not been looked at by a human or asserted by a test. What is
asserted is the numbers behind them and the audio around them. The waterfall PR's offer still stands:
the first person with a monitor that can capture the window should say what these three views look
like.

Frame counts disagree with phase 11 in the absolute (136 fps here, 27.6 there, same recipe) and agree
with each other between views. The number depends on window focus and what else the box is doing, so
the plan's >= 60 fps line is not claimed here beyond "the spectrum costs about as many frames as the
view it shares the window with".

## What was found on the way, and left

- **`7680 frames`** at the start of every session: the device starts rendering before the window
  exists, so the ring fills and overwrites before the frame loop's first drain. Counted and printed,
  not smoothed over. Fixing it means starting the device after the window, which is an ordering change
  in `main.cpp` worth its own branch if anyone needs those first 171 ms.
- **The plan's "a band around A0 resolves it apart from A#0" is physically unreachable** here: 1.64 Hz
  apart wants a 2.4 s window. Left as the doc says, with the arithmetic; the waterfall is the view
  that tells the bottom octaves apart.
- Formatter noise: the installed clang-format 23 wants to realign comments that were written by an
  older one (`frame_view.h`). Not applied; the repository has no format gate and the surrounding code
  already runs to 106 columns.

## Where the project stands

Phase 12 was the last row of PLAN's phase table, so phases 0-12 are done. What remains is the plan's
"optional, only if asked for" list: Linux/ALSA and macOS, a virtual MIDI port, MIDI clock/sync,
param OSC, CI release artifacts, A/B patch compare, keyboard zones, waterfall look-ahead, and the
oscilloscope and spectrogram panels.
