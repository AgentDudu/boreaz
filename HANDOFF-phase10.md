# Phase 10 handoff — the sequencer plays, now somebody has to drive it

Phase 9's handoff (`HANDOFF-phase9.md`) still explains the patch format, the panel and the settings
file. This one is about the transport only.

## Read this if nothing else

Phase 10 is `feature/seq-playback`: in-app `.mid` playback with a transport. **The playing half has
existed since `feature/file-playback`** — a file named on the command line is loaded, fed into the
engine's event ring ahead of the audio thread's position, and the display parks at its length. What
phase 10 adds is control. **10.1 landed the machinery** (`feature/transport-core`, PR #45/#46):
`boreaz::midifile::Transport`, play / pause / stop / seek / whole-song loop, and
`Engine::request_transport_flush()`. **10.2 wired it** (`feature/transport-routing`): `main.cpp` has no
`feed_up_to` and no `playing` flag any more - it holds a transport, and the file named on the command
line goes through the same object the buttons will use. What is left in the phase is the **controls**:
the panel block, a key binding, an open dialog, and the criteria that need a human at them.

## Where the tree is

`develop`, everything merged, `release_check` green, 292 cases / 8 861 643 assertions. No CI exists —
the gates are `python tools/release_check.py` and whatever you run by hand.

## The API (all in `boreaz/midifile/transport.h`)

```cpp
void load(const Song& song);                                 // borrowed, not copied
void play(boreaz::engine::Engine&);                          // from where it stands, or from the top
void pause(boreaz::engine::Engine&); void stop(boreaz::engine::Engine&);
void seek(boreaz::engine::Engine&, std::int64_t song_sample);
void set_loop(bool);                                         // whole song; the only shape so far
std::int64_t position() const;                               // song samples, one feed behind
FeedResult feed(boreaz::engine::Engine&, std::int64_t engine_position);  // once per block, before process()
```

`feed()` is the pump: call it with `Engine::sample_position()` **before** `Engine::process()`, every
block or every pump tick. `Transport::kLookaheadSamples` is 512 = one block.

## The decisions, and why

- **One clock.** The transport holds no counter, only `base` = engine position − song position, so a
  position is a subtraction and the "no drift over 3 minutes" criterion cannot fail. Do not add a
  timer, a `double` accumulator, or a `frames / sample_rate` advance: each of those reintroduces the
  thing the design removes.
- **State changes go through `remap()`**, which moves the mapping, moves the cursor with
  `upper_bound` (first event *after* the position — a cursor that points back at what already fired
  plays it twice), and asks the engine for a flush.
- **The flush is a flag, not a call.** `EventQueue::clear()` is consumer-side only — single producer,
  single consumer, the audio thread owns the tail. `request_transport_flush()` sets an atomic;
  `process()` consumes it at the top of the block, clears the ring and panics.
- **The block after a state change feeds nothing** (`change_pending_`). Without it the flush eats the
  events pushed for the new position in the same block — the seek test caught exactly that. Cost: the
  first events of a resume land up to one block (10.7 ms) late, applied on arrival.
- **The loop guard is `cursor_ > 0`**: one crossing must not wrap twice, and the second wrap would
  drop a length of the song.
- **`panic` releases, it does not mute.** Zero stuck notes means "no voice sounding that is not
  released", never "the buffer is silent".
- **A seek does not retrigger a note that started before the seek point**, and a loop wrap cuts what
  was held across the boundary. Both deliberate; both are in the phase row.

## What is left, in the order to take it

1. ~~Route `main`'s pump through the transport~~ - done on `feature/transport-routing`. One detail to
   keep: the mixer's lookahead is half a second (`set_lookahead`), not the transport's default block,
   because the app's pump sleeps ~10 ms and sometimes sits in a window frame or a stalled callback.
2. **The panel block**: play/pause, stop, a position readout (`mm:ss` off `position()`, or bar:beat
   off `Song::sample_to_tick`), a loop checkbox, and a seek control. Remember there is **no click
   harness** — the suite executes the panel but presses nothing, so the honest proof is a live run.
3. **A key binding**, probably Space = play/pause, and the existing computer-keyboard path. That is
   the control surface a test can at least reach partly (`tests/test_keyboard_layout.cpp` covers the
   mapping, not the effect).
4. **An open dialog**, which the phase row asks for and which does not exist: ImGui has no native file
   dialog, so this is either a Win32 `GetOpenFileName` behind a small `#ifdef _WIN32`, or a text field
   with a Browse button that walks `patches`-style folders. Decide, do not improvise twice.
5. Loop *points* (not whole-song) — a decision about held notes at a mid-song boundary, not a gap.

## Gotchas

- `Transport` is **not** thread-safe and does not need to be: it lives on the pump thread, the one
  that owns the buttons. Nothing in it is touched by the audio callback. If a future slice calls it
  from the ImGui callback *and* the pump, that is the same thread; if anything ever moves the pump,
  re-read this.
- **Absolute CPU readings on this machine drift by tens of percent between hours.** The phase-8 band
  was 0.71 %-1.01 % with 0 late periods; the *same old binary* measured today gives ~1.2 % with 42-43
  late. Any CPU claim is an A/B on the same box at the same hour, and phase 12's 1.2 % budget must be
  judged that way, not against a stored number.
- The headless run's exit position varies by a few blocks (142 336 - 143 872 for a 2.96 s file),
  because the pump's own tick decides when "the tail has rung out" is noticed. That predates the
  transport. Do not chase it as a drift bug: the transport's position is exact, and `main.cpp` prints
  the engine's position, not the transport's.
- The engine's finish/park path in `main.cpp` decides "the song is over" from
  `song_cursor >= events.size() && sample_position >= end_sample`. With a transport the question is
  `!playing()` plus a wrap, so a loop that parks the display at the end is the bug to expect.
- `feed()` returns `FeedResult{cursor, dropped}` and `dropped` is not optional reading: a ring
  overflow is a note lost in silence.
- The `boreaz_midifile` target has a CMake guard: it must not link a device library. The transport
  keeps that true (engine only).

## Verified vs not — do not overclaim

Proven by `tests/test_transport.cpp` (eleven cases): the 3-minute exactness, no stuck notes after stop,
queued notes dropped by pause, seek forward/back, loop wrap, 20-into-8 stealing, keys 10 and 120
sounding and flagged, what one press of one button means, and a slider pushed past either end.
Engine flush proven on its own case. `release_check` green, goldens byte-identical, live headless run
at 0.80 % audio-thread CPU. Whole suite: 295 cases, 8 861 666 assertions.

Engine side: A/B interleaved against the code it replaced, 10 runs each, headless with a song -
278-281 callbacks, 0 restarts, 0 dropped, 0 steals on both sides, audio-thread CPU 0.82 %-1.25 %
against 0.94 %-1.29 % for `feed_up_to`. Nothing measurable, which is the claim.

Not proven: what a mouse does. Nobody has clicked Play, the Loop checkbox or the position bar, and
`main.cpp` is not in the test suite, so "the file still plays" rests on those live runs plus the
transport's own cases.

## 10.3 — the controls, and how to test a window nobody can see

`feature/transport-controls`. The row is drawn in the window shell (`AppWindow::draw`), between the
view switcher and the panel, and it is drawn from `Options::Transport`: nine `std::function` the app
fills in (`active`, `playing`, `looped`, `position`, `end`, `toggle`, `stop`, `toggle_loop`, `seek`).
The window owns no song and no clock, and every hook runs on the thread that pumps the window — the
thread the app's pump loop is already on — so none of them is a lock and none reaches the audio thread
except through `Engine::push_event`. That is why the row disappears when there is no song: with no
song, Play would be a button that lies.

`Transport::toggle` is the whole "what does this button do" answer (running -> pause, pause or
finished -> run, finished at the end -> from the top). `Transport::seek_fraction(engine, 0..1)` clamps,
because a pointer leaving a bar is normal. `end_sample()` exists so the panel can print the song's
length without reaching into `Song`. The bar is click-to-seek, not a drag: a widget that follows the
playhead and is also the thing you drag fights the player every frame. `Enter`/numpad `Enter` calls
`toggle`, the same call the button makes. Backspace still resets the *strip's* clock — that clock is
not the song's playhead and conflating them is a bug waiting to happen.

**The live test, which is the part worth keeping.** An app with a window cannot be watched, but the app
prints `finished` when a file runs out, so the log is the instrument:

- A file that had already ended printed `finished` once. One `Enter` later it printed it twice: the key
  restarted the song and it ran to the end again.
- The same 2.958 s file, `Enter` at 1.2 s: still no `finished` at 5.7 s — impossible unless the pause
  took — and `finished` after a second `Enter`.

Drive it with `PostMessage(hwnd, WM_KEYDOWN/WM_KEYUP, VK_RETURN, ...)` on the process's
`MainWindowHandle` (see `tools/key-in-window.ps1`, and `tools/key-in-window.md` for why the obvious ways deliver nothing). Do **not** try `SetForegroundWindow` +
`keybd_event`, or `WScript.AppActivate` + `SendKeys`: the desktop refuses foreground from another
process, both reported success, and neither delivered a single key — three runs of "the shortcut does
not work" were three runs of testing nothing. `PostMessage` needs no focus, and GLFW's window proc
answers it. Proof it landed: a temporary `fprintf` in the stroke loop printed `key=257 action=1`
(`GLFW_KEY_ENTER`) and the log gained the second `finished`.

**Bug this found, now fixed:** typing in a text field played notes, because every letter of a patch
name is a key on the computer keyboard. The guard is `ImGui::GetIO().WantTextInput`. It was
`WantCaptureKeyboard`, which in this app goes true for reasons unrelated to any field (keyboard
navigation, `ConfigNavCaptureKeyboard`) and would have muted the piano. Measured on the live window:
`WantCaptureKeyboard=0, NavActive=0, WantTextInput=0` while nothing is being typed. Strokes are still
drained while typing, so closing a field does not release a queue of letters as a chord.

Also deduplicated: `Transport` had both `looped()` and `looping()`; the pre-existing `looping()` is the
one that stays.

**10.4, a song chosen while the app runs, is in** (`feature/open-a-song`). No modal dialog: its message
loop would run on the thread that feeds the event ring, and a picker open past the lookahead stutters
the song. Two routes instead — drop the file on the window (GLFW's drop callback copies one name into
`Impl`; the drawing frame acts on it), or `F1` / `Change` to open a path field that takes the keyboard
the frame it opens and loads on `Enter`. The app does the reading: `Options::Songs::load(path, reason)`
parses into a temporary and only then stops the transport, moves the `Song` and reloads, so **a path
that will not read leaves the session alone**. `song_path` in `main.cpp` is the one name for the loaded
file (startup print, runtime print, `finished`), and `set_song_path` tells the window what to display.

Live recipe, in order: `-vk 112`, `-text "C:/…/pad-chords.mid"`, `-vk 13`. An app started with **no file**
printed `48 events, 6.5 seconds` and then `finished`. An app started with a real file and handed
`C:/songs/does-not-exist.mid` printed the loader's reason and changed nothing. The tool's `-ctrl` flag is
gone because posted modifiers do not reach GLFW's modifier state — `mods=0` measured — which is also why
`F1` is the shortcut and every shortcut in this app is a bare key.

Still open in the phase: **the mouse** (nobody has clicked Play, Loop, the bar, `Change` or `Load`) and
**the OLE drop itself** (wired, same code path as the typed field, never driven by a real drag).
