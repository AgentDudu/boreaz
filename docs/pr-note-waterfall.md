# feat: the note waterfall, in the keyboard's own geometry

Phase 11's view: the second entry in the panel table. What has been played, drawn as the keys that
played it, with time running upwards.

## What changed

Three additions, which is the whole cost the plan priced at "one enum value + one file":

- `PanelKind::kWaterfall` in `include/boreaz/gui/panel.h`, and one row in the table in
  `src/gui/panel.cpp`. The `static_assert` beside the table is what stops the enum growing without a
  row; the switcher's buttons and the new keyboard route walk that table, so the view is reachable
  without touching either.
- `src/gui/viz/waterfall.cpp` — the drawing. Reads `FrameView` and nothing else: no engine pointer, no
  device, no way to delay a block. Same contract the transport strip documents.
- `include/boreaz/gui/waterfall_view.h` + `src/gui/waterfall_view.cpp` — the arithmetic, with no ImGui
  in it: which column a note occupies, how tall its bar is, when it has left the plot.

`F2` moves to the next view. The switcher is a row of buttons, and a view that only a mouse can open is
a view a hand that is on the keys never sees — which is also what made "switching views does not disturb
playback" measurable at all. `F2` rather than a letter for the usual reason: every letter on this
keyboard is a pitch.

The view itself:

- **88 columns, one per key, A0–C8.** A lane's x and width are `KeyboardLayout::rect_for(note)` — the
  same call the keyboard under it is drawn from, at the same width, because the panel and the keyboard
  both take the full content region (`AppWindow::draw`). Black-key lanes are inset and narrow for the
  same reason the black keys are.
- **Time runs up.** The bottom edge of the plot is the keyboard line: a note starts there, grows while
  it is held, then travels up at the length it actually sounded and leaves through the top. Nothing
  falls in from above, because the display has no look-ahead.
- **Height is duration, brightness is level, colour is the MIDI channel.** Sixteen colours because the
  waterfall is the only view in which the channel a note arrived on is visible; a one-channel song is
  one colour, and that is the truth rather than a limitation of the drawing.
- **Two controls**: `Scroll` (pixels of height per second of music, 12–320) and `Lane height`
  (1–12 px, the shortest bar a note is drawn at). Both are held in the view's own file, because a panel
  is a view and the app has no reason to know what a waterfall is scaled at. The readout beside them
  says how many seconds of music the plot can show at the current scroll speed, so the number being set
  is in the unit the player is thinking in.
- Notes outside A0–C8 are not silently dropped: the plot prints how many there are, the same honesty the
  on-screen keyboard shows with its `EXT` well.

`bar_for` clips a bar at the edges of the plot rather than dropping it, because a note that is half on
screen is still sounding. The two cases it refuses are a note whose newest end has already gone past the
top, and a note outside the instrument.

## Why the arithmetic is in a header that includes no ImGui

Because the plan's criterion is "lanes line up with the keys below them **to the pixel**", and that is
only an assertion if the numbers can be compared in a binary with no window, no GPU and no theme. The
keyboard geometry already lives in `KeyboardLayout` for the same reason (phase 5), so the waterfall
borrows it instead of restating it: the lane for note 60 *is* the rect of key 60, and the test compares
them with `==` and no tolerance. Anything but bit-identical would mean two pieces of arithmetic had been
written instead of one.

## Why `Lane height` is a floor and not a row size

In a waterfall whose lanes are the keys, the lane's width is already decided by the keyboard, and the
dimension the player can meaningfully scale is the time axis. A staccato note off a cable is a few
milliseconds, which at any scroll speed worth reading is a fraction of a pixel, and a note the ear heard
that the screen refuses to draw is a screen that disagrees with the ear. So the control sets the minimum
thickness of a bar. It grows upwards, never below the line: a note struck this instant is one pixel tall
*above* the keys.

## Measured

Geometry — `tests/test_waterfall.cpp`, no window and no device. Suite is now **303 cases /
8 865 045 assertions** (was 295 / 8 861 666):

- All 88 lanes, at four widths (88, 480, 1428, 2548 px) and three x-offsets (0, 12, 137.5): lane x and
  width equal the keyboard's own `rect_for` **exactly**, and the white-key width matches. 3 370
  assertions of `==` in that one case.
- A 5 ms note at the slowest scroll speed (5 ms = 0.06 px of travel) still draws a bar at the lane-height
  floor, sitting on the keyboard line and not through it.
- 40 notes sounding at once produce 40 bars, all inside the plot, none dropped. `kMaxTrail >= 40` is a
  `static_assert`, so the display's own budget cannot quietly fall under the figure the view is judged on.
- Clipping versus leaving: a note held longer than the plot can show is cut at the top and still drawn;
  one that ended long enough ago that its newest end has passed the top is not drawn.
- Notes 0, 9, 20, 109 and 127 are refused and the caller can tell that they were refused; the
  instrument's own two ends (21, 108) are placed.
- The clock going backwards (Backspace while the trail still holds notes from under the old clock) puts a
  note at the line rather than above where it has not happened.
- Both controls move the picture, and out-of-range values clamp instead of drawing nothing.

Live — window 1280×720, device 48 kHz / 512-frame period, `tools/key-in-window.ps1 -vk 113` for `F2`,
window closed with `WM_CLOSE` so the exit report prints. Same session, back to back, `pad-chords.mid`
(48 events, 6.5 s):

| run | callbacks | late | restarts | dropped | steals | frames | audio cpu |
| --- | --- | --- | --- | --- | --- | --- | --- |
| transport panel the whole time | 1077 | 166 | 0 | 0 | 8 | 353 | 1.67 % |
| waterfall from 2 s | 1075 | 166 | 0 | 0 | 8 | 349 | 1.54 % |
| switched twice mid-song (`F2`, `F2`) | 1545 | 238 | 0 | 0 | 8 | 511 | 1.43 % |
| idle window, no song (baseline) | 988 | 152 | 0 | 0 | 0 | 322 | 0.64 % |

The two rows that matter are the first two: **callbacks 1077 vs 1075, late 166 vs 166, dropped 0, steals
8 vs 8.** The view costs nothing the audio thread can feel, and the picture it draws changes nothing the
engine does.

The third row answers the plan's switching criterion: one `finished` line for a song that played through
two view changes, the same 8 steals and 0 drops as the run that never switched, and the engine's position
advancing at the same rate. A view change is a different drawing of the same state; it does not stop,
reset or retrigger anything — which is why `F2` calls nothing but `panel_`.

Over the file the plan names, `glissando-88.mid` (176 events, 17.75 s), waterfall open from 2 s to the
end: `2003 callbacks, 308 late, 0 restarts, 0 dropped, 0 steals`, one `finished`. The 308 is 15.4 % of
callbacks; the idle baseline above is 152/988 = 15.4 %. That ratio is this machine being loaded, not the
view.

## Not measured

- **Nobody has looked at the pixels.** Screen capture on this machine returns a blank client area for
  this window: `PrintWindow` with `PW_RENDERFULLCONTENT` and a `BitBlt` of the screen over a forced
  top-most window both came back 94 % white with only the frame borders in the app's own colours. The
  alignment claim therefore rests on the geometry test plus the code path that gives the panel and the
  keyboard the same x and the same width, and not on a screenshot. If a real capture path is wanted,
  it needs Windows.Graphics.Capture, which is a different piece of work.
- **`frames/sec ≥ 60` was not met on this machine, by any view.** 353 frames in 12.8 s is 27.6 fps with
  the transport view, 349 in the same time with the waterfall, and an idle window with no song at all
  manages 322 in 11.8 s. The three numbers agree, which is what makes them a baseline rather than a
  regression, but the 60 fps criterion stays unproven here and is not claimed.
- **`< 2 ms/frame` was not measured as microseconds.** It is measured as a difference that is not there:
  353 frames vs 349 over the same wall time, and audio-callback counts identical to within 2. A µs/frame
  print belongs to phase 12, where the panel does FFT maths and the number actually means something.
- **Whether a 5 ms note reaches the display at all.** The bar is drawn whenever the view is *told* about
  the note, and the view is told by `NoteTrail` watching one frame's voice snapshot against the next —
  the plan's accepted risk that the source is snapshot-only. At 27 fps a 5 ms note is inside a frame
  about one time in three, so the honest claim is "drawn if seen, and never too short to see", not
  "always appears". Making it appear every time needs the note-event ring the plan gives to the piano
  roll.
- Whether the two sliders and the readout are *comfortable*, and how the plot reads at 1920×1080 with
  the keyboard share at its cap. It is reachable, sized and clamped; that is not the same as tuned.
