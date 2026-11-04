feat: a transport over a song, on the engine's clock and nothing else's

Phase 10's exit criteria start with a claim about time — "a `.mid` plays in time with no drift
against the audio clock over 3 minutes" — and the way to make that true is not to measure the drift
of two clocks until it is small enough. It is to have one clock. This is that transport: play, pause,
stop, seek, loop, with **no counter of its own**. It keeps one number, `base`, the offset between
`Engine::sample_position()` and the position inside the song, so a position is a subtraction. There
is nothing here that can accumulate: no float accumulator, no `frames / sample_rate` per block, no
timer thread.

Not wired to the mixer yet, and this PR does not pretend otherwise: the panel block, the key binding
and routing `main`'s pump through the transport are the next slice. What is here is the machinery and
the measurements the criteria ask for, at 48 kHz against a real engine.

## The mechanism that did not exist: dropping a future that is no longer wanted

Events are pushed ahead of the position on purpose — the engine applies an event on the sample it
names, so being early costs nothing and being late moves a note. Which means that at any moment the
engine's ring holds notes for the next block or so. Pause, seek or loop has to throw those away, or
pausing starts a note after the player stopped the song.

`EventQueue::clear()` is marked **consumer side only**, and that is not decoration: the ring is a
single-producer/single-consumer ring, so the only thread allowed to move its tail is the audio
thread. So the request is an atomic flag (`Engine::request_transport_flush()`) and the drop happens
in `process()`, which also panics. The cost on the audio thread is one atomic exchange per block,
nothing in the per-sample path, no allocation.

The first version fed the new position in the same block as the request, and the seek test failed:
the flush consumed at the top of the next block dropped the events that had just been pushed for the
new position. So the block after any state change feeds nothing (`change_pending_`), and the events
that belong to the new position go out one block late — 10.7 ms at 48 kHz with a 512-frame period —
which the engine applies on arrival. That is the price of a pause being a pause rather than a wobble,
and it is in the header comment rather than in a commit message someone has to `git log` for.

The loop guard is `cursor_ > 0`: without it one crossing wraps twice, and the repeat silently loses a
length of the song.

## What `panic` does, which I had wrong

Stop and pause assert "zero stuck notes". My first version asserted `active_voices() == 0` and it was
wrong, not the code: `VoiceAllocator::panic` **releases** every active voice — dampers, not an instant
mute — so a voice that is still ringing while released is correct behaviour. The tests now assert
"no voice is sounding that is not released" (`max_unreleased == 0`), which is what stuck means, and
the flush's own case asserts the honest thing: a note already queued for the next block never starts.

## Measured

| criterion | measurement |
|---|---|
| no drift against the audio clock over 3 minutes | 16 740 blocks of 512 frames = 8 570 368 samples on a looped song; final position equals `(start + elapsed) % end_sample` **exactly**, wrap count exact |
| stop and panic leave zero stuck notes | after `stop`, no unreleased voice at any block, and one queued for the next block never starts |
| seeking/looping retrigger correctly | seek forward past a note: it does not sound; the next one does. seek into a held note: it does not start again. loop: position wraps with the overshoot kept, the note at the top sounds again |
| more than a pool of notes at once | twenty note-ons into a pool of eight: sounds, `active_voices <= 8`, `steal_count > 0` |
| keys outside A0–C8 | keys 10 and 120 sound (pitch from the tuning table) and are flagged `is_extended_key` |
| CPU stays inside the phase-8 budget | live on the device, headless with a song: **0.80 %** audio-thread CPU, 280 callbacks, 0 restarts, 0 dropped, 0 steals — inside the 0.71 %–1.01 % band recorded before the engine change, which is the point: the flush costs nothing measurable. The mixer-side number comes with the wiring. |

`boreaz_tests` is 292 cases / 8 861 643 assertions, nine of them new; `release_check` green end to
end, every golden unmoved — the render gates run the engine through the same `process()` with the
flag never set, and they came back byte-identical.

## Not done

- **The mixer does not use it yet.** `main` still feeds the song with `feed_up_to` and parks the
  transport at the end. The panel block (play/pause/stop, the position, loop) and a key binding are
  the next slice, and so are the live criteria that only exist once a human can press the buttons.
- The loop is whole-song only. A loop between two points wants a `Song`-level tick range and a
  decision about what a held note does at a mid-song boundary; that is a decision, not a omission.
- A seek does not sound a note that started before the seek point. Deliberate: nothing was sounding
  at the position you jumped to, so a piano has nothing to play. If the mixer wants otherwise, it is
  a retrigger, and it should be said out loud in the phase row.
