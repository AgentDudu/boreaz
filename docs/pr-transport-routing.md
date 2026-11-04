# feat: the app plays a file through the transport, not through its own loop

## What changed

`main.cpp` used to feed a song itself: a cursor, a `feed_up_to` call, a local `bool playing`, and a
half-second lookahead it worked out from the device rate. Now it holds a `boreaz::midifile::Transport`,
hands it the song, starts it, and every pump tick calls `transport.feed(engine,
engine.sample_position())`. The local `playing` flag is gone - the three places that asked whether a
song is still running ask the transport. When the file ends the transport is paused, not stopped, so
the position stays where the song ended and the next `play()` is what puts it back at the top.

The transport gained `set_lookahead`. Its default is one block (512), which is right for a pump that
wakes every block; the app's pump sleeps ~10 ms and can also be inside a window frame or a driver that
stalled a callback, and it has always needed half a second of runway for that. Feeding one block ahead
in the app would not break anything loudly - it would make notes arrive late, which is the one thing
this engine refuses to do, because it applies an event on the sample it names.

## Why

Phase 10's controls arrive in the next slice, and a file that plays by one route while a clicked song
plays by another is two players to keep in time. Routing the command-line file through the transport
first means there is one path, and it means the live measurements cover the transport instead of the
code that is about to be deleted.

## Evidence

**Interleaved A/B, 10 runs each, same machine minutes apart, headless, one instrument, `lead-simple.mid`
(2.96 s), alternated so machine drift hits both sides equally:**

| build | callbacks | late | restarts | dropped | audio-thread CPU |
|---|---|---|---|---|---|
| `feed_up_to` (before) | 278-280, median 279 | 42-43 | 0 | 0 | 0.94-1.29 %, median 1.20 |
| transport (after) | 278-281, median 278 | 42-43 | 0 | 0 | 0.82-1.25 %, median 1.06 |

So: nothing measurable. The engine's event ring took the same events (`0 dropped`, `0 steals`, `0
restarts` on both sides) and the CPU difference is inside the spread, with the transport side actually
reading lower.

**The number that fooled me, written down so nobody re-fools themselves:** those 42-43 late periods
look like a change, because the runs recorded during phase 8 and 9 reported 0 late. They are the
machine: the *old* binary, run today, reports 42-43 late and a median of 1.20 % against phase 8's band
of 0.71-1.01 %. Absolute percentages on this box drift by tens of percent between hours. Phase 12's
"audio thread stays under 1.2 % CPU" budget therefore has to be judged against an A/B on the same
machine at the same hour, never against a stored number.

Also visible in both columns: the position the run exits at varies (142 336 - 143 872 samples). That is
the pump's own tick deciding when "the tail has rung out" is noticed, and it behaved that way before
this change too; the audio before that moment is unaffected.

`release_check` green, 292 cases / 8 861 643 assertions, goldens byte-identical.

## What this does not prove

Nobody has pressed anything. There is still no transport control in the window, so the phase-10
criteria that name a person at a button are untouched. And `main.cpp` is not in the test suite - the
routing is proven by the transport's own cases plus the live A/B above, not by a unit test of the app.

## Risk

The only behaviour change a user could notice: at the end of a file the transport is *paused* rather
than the loop's flag being cleared, and a pause asks the engine to drop queued events and release what
is sounding. That is asked at a moment when the song's tail has already rung out (the finish condition
requires the position to pass `end_sample`, which is the last event plus a second), so there is nothing
to release. A file that ends with an unfinished tail is not possible in this engine: the tail is part of
the condition.
