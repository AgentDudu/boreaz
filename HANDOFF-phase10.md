# Phase 10 handoff — the sequencer plays, now somebody has to drive it

Phase 9's handoff (`HANDOFF-phase9.md`) still explains the patch format, the panel and the settings
file. This one is about the transport only.

## Read this if nothing else

Phase 10 is `feature/seq-playback`: in-app `.mid` playback with a transport. **The playing half has
existed since `feature/file-playback`** — a file named on the command line is loaded, fed into the
engine's event ring ahead of the audio thread's position, and the display parks at its length. What
phase 10 adds is control. **10.1 landed the machinery** (`feature/transport-core`, PR #45):
`boreaz::midifile::Transport`, play / pause / stop / seek / whole-song loop, and
`Engine::request_transport_flush()`. **It is not wired to anything yet.** `main.cpp` still feeds with
`feed_up_to` and does not know a transport exists. The next slice is the wiring and the controls, and
that is where the phase's remaining exit criteria live.

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

1. **Route `main`'s pump through the transport** (replace the `feed_up_to` call, keep the finish/park
   behaviour, keep `song_dropped` reported). Then the live criteria are measurable: CPU with the
   transport in the loop, stuck notes at stop, seeking while playing.
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
- The engine's finish/park path in `main.cpp` decides "the song is over" from
  `song_cursor >= events.size() && sample_position >= end_sample`. With a transport the question is
  `!playing()` plus a wrap, so a loop that parks the display at the end is the bug to expect.
- `feed()` returns `FeedResult{cursor, dropped}` and `dropped` is not optional reading: a ring
  overflow is a note lost in silence.
- The `boreaz_midifile` target has a CMake guard: it must not link a device library. The transport
  keeps that true (engine only).

## Verified vs not — do not overclaim

Proven by `tests/test_transport.cpp` (nine cases): the 3-minute exactness, no stuck notes after stop,
queued notes dropped by pause, seek forward/back, loop wrap, 20-into-8 stealing, keys 10 and 120
sounding and flagged. Engine flush proven on its own case. `release_check` green, goldens
byte-identical, live headless run at 0.80 % audio-thread CPU.

Not proven: anything a human does. Nobody has pressed play. Nothing has been wired, so "the file still
plays" is a claim about `main.cpp`, which this slice did not touch.
