# Phase 11 — the note waterfall

Merged into `develop` as PR #50 (`feature/note-waterfall`, merge `fe374d1`).

## What landed

`PanelKind::kWaterfall`, one row in the panel table, and `src/gui/viz/waterfall.cpp` — which is the
whole cost the plan priced at "one enum value + one file". The drawing reads `FrameView` and nothing
else, so the waterfall has no engine pointer, no device, and no way to delay a block.

The arithmetic is separate: `include/boreaz/gui/waterfall_view.h` and `src/gui/waterfall_view.cpp`
decide which column a note occupies and how tall its bar is, and they include no ImGui. That is what
lets the plan's "lanes line up with the keys **to the pixel**" be an assertion: a lane's x and width
are `KeyboardLayout::rect_for(note)` at the width the keyboard below is drawn at, and the test compares
them with `==` and no tolerance (88 notes × 4 widths × 3 x-offsets).

Time runs upwards. The bottom edge of the plot is the keyboard line: a note starts there, grows while it
is held, then travels up at the length it sounded and leaves through the top. Nothing enters from above
because the display has no look-ahead, and the plan's risk table already accepts that.
Height is duration, brightness is level, colour is the MIDI channel (16 colours — the only view in which
channel is visible at all). Notes outside A0–C8 are counted and printed rather than dropped.

The two controls are `Scroll` (12–320 px per second of music) and `Lane height` (1–12 px), held in the
view's own file: a panel is a view, and the app has no reason to know what a waterfall is scaled at.
`Lane height` is the **minimum thickness** of a bar rather than a row size — in a waterfall whose lanes
are the keys, the width is already decided by the keyboard and the thing worth scaling is the time axis.
It grows upwards out of the line, never below it.

**`F2` moves to the next view** (`src/gui/app_window.cpp`, in the stroke drain). The switcher is buttons,
and a view only a mouse can open is a view a hand that is on the keys never sees — the same reason the
transport is `Enter` and the path field is `F1`. It walks `panel_table`, so a view added to the table is
reachable without touching that line. `F2` is not a letter because every letter is a pitch.

While in there: the comment above the path field still said `Ctrl+O`, from before the modifier discovery;
it says `F1` now.

## What is proven

`tests/test_waterfall.cpp` (no window, no device; the suite is now 303 cases / 8 865 045 assertions):
pixel-exact lane alignment, a 5 ms note drawn at the floor rather than as a sub-pixel nothing, 40
concurrent notes producing 40 bars (`kMaxTrail >= 40` is a `static_assert`), clipping versus actually
having left the plot, the outside-the-instrument notes being refused and countable, the clock-going-
backwards case, and both controls clamping.

Live, same session, `pad-chords.mid`: transport 1077 callbacks / 166 late / 0 dropped / 8 steals /
353 frames, waterfall 1075 / 166 / 0 / 8 / 349. Switching mid-song twice: one `finished`, same counters.
`glissando-88.mid` end to end with the waterfall open: 2003 callbacks, 0 restarts, 0 dropped, 0 steals,
and its late ratio (308/2003 = 15.4 %) equals the idle-window baseline's (152/988 = 15.4 %).

## What is not proven, and why

- **Nobody has seen the pixels.** `PrintWindow` with `PW_RENDERFULLCONTENT` and a `BitBlt` of the screen
  over a forced top-most window both returned a blank (94 % white) client area for this DXGI swapchain.
  The alignment claim rests on the geometry test and on `AppWindow::draw` giving the panel and the
  keyboard the same x and width. A capture path that works needs Windows.Graphics.Capture — not started.
- **60 fps was not reached on this machine by anything**: 27.6 fps with the transport view, 27.3 with the
  waterfall, 27.3 for an idle window with no song. The numbers agreeing is the point; the criterion is
  still open.
- **Whether a 5 ms note reaches the display.** `NoteTrail` builds history by watching one frame's voice
  snapshot against the next, so at 27 fps a 5 ms note is inside a frame about one time in three. The
  waterfall draws any note it is told about, at a thickness it cannot miss; making the note arrive every
  time needs the note-event ring the plan gives to the piano roll.
- Comfort: slider placement, how the plot reads at 1920×1080, whether the seconds readout is where the
  eye goes.

Carried from phase 10 and still open: nobody has clicked Play, Loop, the seek bar, `Change` or `Load`,
and the OLE drop has never been driven by a real drag. `F2` is one more thing measured by post instead
of by hand.

## Next

Phase 12, `feature/spectrum-panel`: log-spaced bands over the keyboard's ranges, fed by a new **audio
tap** (a fixed SPSC ring the callback memcpys into and overwrites when nobody drains), with `Analyser`
(Hann, radix-2 FFT, energy per band, dB, peak decay) in `boreaz_dsp` so the panel stays one file that
includes no FFT, two window lengths (8192 where bins must be narrow, 2048 where a transient must light
now), and the peak-decay rate as the one `.ini` knob. Its DoD is almost entirely testable on synthetic
buffers, which is the easiest place in the plan to bank certainty.
