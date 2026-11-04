# The spectrum panel: the master bus, cut into the 88 keys

Branch `feature/spectrum-panel`. Plan phase 12.

## What is in it

- **`boreaz::dsp::Analyser`** (`include/boreaz/dsp/analyser.h`, header-only like the rest of
  `boreaz_dsp`): Hann window, in-place radix-2 real FFT, magnitude summed into 88 semitone bands,
  dB relative to full scale with the floor at -120, and a peak-hold that falls at a settable rate.
  Two window lengths, `kWideWindow` 8192 and `kFastWindow` 2048, because one window cannot do both
  jobs. It reports its own cost (`last_microseconds`) and how much of what it was handed it could
  not keep (`dropped_input_frames`).
- **`boreaz::dsp::SampleTap`**: a fixed SPSC ring, 8192 frames of interleaved stereo, one atomic
  cursor each way. `AudioDevice::set_tap()` gives the callback a place to `memcpy` into. The callback
  never waits on it and never reads what the reader reads; when nobody drains, it overwrites the
  oldest sound and counts it.
- **`src/app/main.cpp`**: drains the ring once per frame, runs both analysers, and hands the numbers
  to the window. Nothing about the FFT reaches the audio thread.
- **`FrameView::spectrum`** (`SpectrumSnapshot`) and **`AppWindow::set_spectrum`**: the same one-copy
  per frame every other panel uses. A panel still reads no engine, no device, and no analyser.
- **`src/gui/viz/spectrum.cpp`**: the drawing. Bands at the x of `KeyboardLayout` asked for the same
  width as the keyboard below, so a band sits over the key that plays it; dB grid at 0/-20/-40/-60;
  a hold line above each bar; a `Window` combo (2048/8192) and a `Hold` checkbox; and a line that
  prints the window's size, the transform's microseconds, and what the measurement is missing.
- **`peak_decay`**, the plan's one knob, in `%APPDATA%\Boreaz\settings.txt`.

## Why the maths is not in the callback

The plan's shape: the tap copies, the frame loop thinks. An 8192-point transform measured **368-545
us** on this machine against a 512-frame period of **10.7 ms**: inside the audio thread it would be
5 % of the callback's entire budget, spent on a picture. So the callback does a `memcpy` and a branch
on a null pointer, and everything else happens on the thread that pumps the window. The consequence
is deliberately not hidden: the ring holds 8192 frames, so a frame loop that stops for more than
171 ms starts losing sound, and the panel prints how much it lost.

## What the unit tests prove (no window, no device)

314 cases, 8 877 437 assertions, the whole suite green; 25 of them are the analyser and the tap.

- A -20 dBFS sine lands in the band of the note it is, for **A0, C2, A2, C4, A4, C6, A6, C8** - within
  one band of it, and reading within 1.5 dB.
- Two notes a fifth apart (220 + 330 Hz) light two bands with a valley at least 6 dB deep.
- Silence reads below -100 dB in all 88 bands; a steady DC offset does not light the lowest band.
- A 20 ms burst in the top octave lights its band on the 2048 window and misses on the 8192 one, in
  the same number of pushes - which is the whole argument for shipping two window lengths.
- A writer 10 000 blocks ahead of a reader that never comes: the ring stays exactly 8192 frames, the
  writer never blocks, and the reader can say afterwards how much it missed.
- The peak falls at the rate it was set to, and not below the current band level.
- A hidden panel drains the ring and resets the analyser: the bands and the hold lines go back to the
  floor, and the next note lights its own band instead of the frozen one.

## What it resolves, measured from the shipped header

`build/_scratch/band_probe.cpp`, compiled against `include/boreaz/dsp/analyser.h`, this machine, 48 kHz.
A Hann window's main lobe is four bins wide, which is the number that decides what can be told apart.

| window | length | one bin | Hann main lobe |
| --- | --- | --- | --- |
| 2048 | 42.7 ms | 23.44 Hz | 93.75 Hz |
| 8192 | 170.7 ms | 5.86 Hz | 23.44 Hz |

| note | frequency | one semitone | in main lobes, 2048 | in main lobes, 8192 |
| --- | --- | --- | --- | --- |
| A0 | 27.50 Hz | 1.64 Hz | 0.02 | 0.07 |
| A4 | 440.00 Hz | 26.16 Hz | 0.28 | 1.12 |
| C8 | 4186.01 Hz | 248.91 Hz | 2.66 | 10.62 |

So the panel separates the top octave easily and the middle octave about at a semitone (the plan's
"about a semitone at C8" for the short window is right in the same sense: 23.4 Hz is one semitone at
C8 and eleven at A0). **The plan's "a band around A0 (27.5 Hz) resolves it apart from A#0" is not met,
and no window length in this program meets it.** Splitting 27.50 Hz from 29.14 Hz wants a main lobe
narrower than 1.64 Hz, which wants `4 * 48000 / 1.64 = 117 073` samples - 2.4 seconds of sound, which
would smear every note in the piece. That is Fourier, not a bug: at the bottom of the keyboard a
spectrum says which region is sounding, and the waterfall is the view that says which key was pressed.
What is proven at the bottom end is the weaker and still useful claim, in the first test above: a lone
A0 peaks in the A0 band and within one band of it.

## What it costs, measured

The probe's wall clock around 500 complete transforms, and then the app's own exit report, which prints
the analyser's self-reported microseconds per transform:

| | 2048-point | 8192-point |
| --- | --- | --- |
| probe, wall per transform | 103.8 us | 378.4 us |
| probe, self-reported | 78.2 us | 368.4 us |
| live app, self-reported (three runs) | 86.5 - 249.8 us | 482.9 - 544.6 us |

The long transform does not run every frame and cannot: a new 8192-sample window arrives every 171 ms,
so a 20 second run logged **89 long and 354 short transforms** - the long bands refresh about 6 times a
second and the short ones about 23. Averaged over that run's 3186 frames the spectrum costs about
**23 us/frame**, and the worst single frame is the one that pays for an 8192-point transform: about
**0.55 ms**, inside the plan's 2 ms/frame with room to spare. A panel that transformed both windows
every frame would cost 0.7 ms and show nothing extra, because there would be no extra sound to look at.

## Live, on a window and a real device

Window, default device 48 kHz / 512-frame period, `patches/piano-fortissimo.txt`,
`tests/fixtures/midifile/glissando-88.mid` (176 events, 17.75 s), panel switched with `F2`
(`tools/key-in-window.ps1 -vk 113`), window closed with `taskkill` (a `WM_CLOSE`, so the exit report
prints). Both rows below ran the same 22.9 s wall clock, and the second one sent two `F2` at 2 s and
3 s exactly like the first sent two harmless shifts:

| run | callbacks | late | restarts | dropped | steals | frames | audio cpu |
| --- | --- | --- | --- | --- | --- | --- | --- |
| transport panel the whole time | 1817 | 280 | 0 | 0 | 0 | 3127 | 1.01 % |
| spectrum from ~3 s | 1846 | 283 | 0 | 0 | 0 | 3186 | 0.97 % |

The row that matters for the audio thread: **callbacks 1817 vs 1846, late 280 vs 283, 0 restarts, 0
dropped, 0 steals both times.** 15.4 % late in both, which is the same ratio phase 11 measured on an
idle window on this box, so it is the machine being loaded and not the panel. Frame counts are within
2 % of each other, in the wrong direction to be a cost.

Switching mid-song - spectrum at 3 s, away at 9 s, back at 12 s, five `F2` in a 30 s run: **one
`finished` line, 0 restarts, 0 dropped, 0 steals**, 2536 callbacks with 389 late (15.3 %). A view change
changes the drawing and nothing else, which is the criterion inherited from the waterfall and it holds.

The panel's own line from that same run:

```text
boreaz: spectrum 89 long + 354 short transforms, 495.4 us at 8192 samples + 86.5 us at 2048 samples
per frame, hold falls at 22.5 dB/s, 7680 frames of sound the copy threw away, 12800 frames older
than what its own window can hold.
```

The `peak_decay = 22.5` in that line came from `settings.txt`. With `peak_decay = banana` the app says
`peak_decay = banana is not a rate between 0 and 120 dB/s; using 14 dB/s.` and starts anyway, which is
the same rule the settings reader applies to every line it cannot use. The file is rewritten with all
five keys at the end of a windowed session, the knob carried through unchanged.

**7680 frames is 15 callbacks, and it is a real wart.** The device is told to start before the window
exists, and GLFW, D3D and ImGui take about that long to come up, so the ring fills and starts
overwriting before the frame loop's first drain. It is counted and printed rather than smoothed over,
the counters are cumulative for the session, and the picture that appears is the sound that was
happening when the loop started draining. A `--wait-for-window` ordering fix belongs to whoever next
needs those first 171 ms.

