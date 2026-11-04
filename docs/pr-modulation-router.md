# Modulation router: eight cables, and the polarity is a per-slot flag

> **Partly superseded 2026-11-04.** The source listed below as `env2` is `ModSource::kModEnv` and
> its rows are `mod_env_attack_s`/`_decay_s`/`_sustain`/`_release_s`. Everything else is what merged,
> and the naming argument in it is why the keys moved before phase 9 rather than after.

## What landed

`ModSource` {off, lfo1, lfo2, env2, velocity, keytrack, wheel} × `ModDest` {off, cutoff, pitch, osc2,
osc3, noise, pan} in eight slots. The enums and the free-function contract (normalise, shape, clamp,
`mod_amount`) are `include/boreaz/engine/mod_router.h`; the destination arithmetic is the slot loop in
`SynthVoice::apply_modulation`, and the flags that make an unwired voice free are `prepare_modulation`.
Neither the `Params` nor the matrix is a member of the voice: the voice keeps one block-rate copy of
`Params` (`applied_`), so a hand on a knob moves a note that is already sounding.

A slot is four fields — source, destination, depth, **unipolar** — and the unipolar flag is the one
nobody asked for. 38 table rows, 0 removed, **25 → 63 knobs**: four per slot (`mod1_source`,
`mod1_dest`, `mod1_depth`, `mod1_unipolar` … `mod8_*`), plus the six the cables cannot work without
(`env2_attack_s`/`decay_s`/`sustain`/`release_s`, `lfo1_rate_hz`, `lfo2_rate_hz`). `ModDest::kOff = 0`
so an unwired cable is the stored default rather than `cutoff`.

**Nothing about an unwired instrument moved.** 269 tests (17 new), goldens compared by value and
unmoved, ASan clean, `release_check` green.

## What the measurement says

`lead-simple.mid`, offline, engine call only — `boreaz-render`'s own `engine work … % of one core`
line, median of 5 runs each:

| | |
|---|---|
| unwired | **1.36 %** of one core |
| one cable, lfo1 → pitch | **1.37 %** |
| eight cables across every destination | **1.38 %** |
| run-to-run spread of every set above | 1.33 – 1.79 % |
| plan criterion | "per-voice modulation costs < 1 op/sample at default settings": at defaults the voice reads eight `int`s and one flag, and none of those numbers is resolvable against any other |

The flags are what buys it, and they are the construction rather than an optimisation: `mod_active_` is
false when no slot wants the block, `mod_used_lfo1_`/`_lfo2_`/`_env2_` mean an unrouted source is not
even advanced, and the three level destinations need no flag at all because they are added where the
level is read (`osc2_level()` is `mixed_level(applied_.osc2_level, mod_.osc2_level)`).

## The bug this PR really owes its existence to

`ParamSpec` addresses a parameter by one of three means — `real`, `integer`, or `array_kind` + `slot` —
and **two callers asked it a question that has no honest answer**: `if (spec.real != nullptr) { … } else
{ read through spec.integer }`. For an array row both pointers are null, so:

- a patch file refused `mod1_depth = 0.4` — "wants a whole number" — on the one row of the four whose
  whole job is to be continuous, and
- a row it did accept was written by `params.*(spec.integer) = parsed` through a **null member
  pointer**, and the panel did the read equivalent for 33 of the 63 rows, which is 33 sliders drawn from
  wherever a null member pointer lands.

Both are one question now: `param_is_integer(spec)`, in the table, where the three means of addressing a
parameter are known. The loader writes through `param_write` like everything else, the panel reads with
`param_read`, and `tests/test_mod_router.cpp` now walks **every** row of the table and asserts that what
the table wrote the table can read back — the guard the array rows nearly went without. Nothing found it
because nothing reached that far: the file test used scalar rows and the panel is not in the suite.

**The lesson for the next row anyone adds:** the table has three ways to address a parameter, so a
caller that branches on which one is set has to be told which question to ask. `param_is_integer` is
that question. The two places that guessed are both fixed here, and the round-trip loop over all 63
rows is what stops a third from appearing.

## What surprised me during implementation

**The polarity cannot be a per-source rule, and that is the whole design.** Velocity, wheel and keytrack
are levels that rest at 0; an LFO is a swing that rests at ±1; and **Env2 is a level that falls back to 0
when the note lets go** — so "env2 → pitch, bipolar" under a per-source rule is an envelope *pitch drop*
whose depth depends on how long you hold the key, which is not the patch that was named. Only a flag on
the slot separates "a thing that rests at 0" from "a thing that swings". It arrived as a question, and it
was the right one: five of the six sources rest at zero, and for the one that does not (the LFO) the flag
is a convenience rather than a contradiction.

The second surprise is that `depth × source` is not one destination's arithmetic: a cutoff is a
*multiplier*, so an octave has to be `2^x` or the depth means hertz at A0 and kilohertz at C8; and levels
must be floored at 0 while pitch, cutoff and pan must be allowed both ways, because flooring a pitch at
0 Hz is not a floor. The units are the destination's own — pitch ±2 semitones, cutoff ±3 octaves, **pan
the whole field** added to the key's own position, levels ±1 floored at 0 — and they are written once, in
`mod_amount`, and quoted at by the tests.

## What I'd change with more time

**Modulation is block-rate, and that is a trade written in the code, not an oversight.**
`Oscillator::advance_block(frames)` returns the value where the block began, so a 20 Hz LFO gets three
steps per cycle at 44.1 kHz and 64-frame blocks: invisible on a cutoff or a pan, stepped on fast vibrato
at a high depth. The upgrade is a per-sample ramp on the destinations that want it — one add per sample
per routed destination, which the numbers above say there is room for — and it is not worth doing before
anybody hears whether the stepping bothers anyone. It is a `shortcut:` comment on `apply_modulation`.

The other thing I'd settle is a label, not code: the row is `env2_attack_s` ("Env2 attack") next to
`amp_attack_s` ("Attack"), and inside this engine the envelope a player hears as "the second envelope" is
the amp one. `env1_*` has gone un-named for four phases for the same reason. The panel is where the name
becomes the player's problem, so that is where I'd settle it rather than renaming a key a week before a
patch file has to be stable.

## What it breaks / what has to change later

- **The patch file already carries the cables.** `src/config/params_file.cpp` walks the table, so
  `--params`, `--load` and `--load-params` took all 63 rows the moment the rows existed — refused loudly
  when out of range, `param_write`'s clamp otherwise, unknown keys an error because a typo in a patch is
  a patch that quietly does not say what it says. No format change was needed, and `Params` is still the
  versionless struct, so the loader stays the compatibility surface.
- **The panel has no way to edit a cable.** It draws all 63 rows as sliders, including eight
  "M1 src" sliders reading 0–6 with no names; eight source/destination pairs want list widgets, and that
  is phase 8.4's, with the search box PLAN asks for.
- Keytrack ships exactly as PLAN's phase-8 row specifies (A0 = −1.0, C4 = 0.0, C8 = +1.0) with one detail
  the row did not settle: the two halves are scaled separately (39 keys below centre, 48 above) so **both**
  ends are exact, which makes the slope different either side of middle C. The alternative — one slope —
  puts the exact end at one end and a clamp at the other. `mod_router.h` says so at the function.
- Sampled voices are untouched, as the row requires: no filter, no matrix, amp and pan only. They do
  accept CC1 and ignore it, which is what they did before the router existed.

## One thing to know before reviewing the next file you touch

`cmake --build build 2>&1 | grep -E " error "` is a trap: it prints nothing when the failure is in a TU
you have not looked at, and the suite you then run is the **previous** binary. I read two green results
off a stale `boreaz_tests.exe` before noticing the build had stopped two files earlier. Run the build,
look at its exit status, then run the tests.

## Review focus

1. **`param_is_integer` and the two call sites** (`src/config/params_file.cpp`,
   `src/gui/params_panel.cpp`). Is asking the table the right shape, or should `ParamSpec` have carried
   one tagged accessor instead of three means and a question? The bug section above is what happened to
   the version where callers guessed.
2. `apply_modulation` and `prepare_modulation` — the flags. `mod_used_env2_` also decides whether the
   envelope is stepped at all, so a cable pulled while a note sounds cannot leave an LFO advancing; that
   is worth an eyeball because it is the one place where a re-patch and a running note meet.
3. `mod_amount` and the normalisation helpers in `mod_router.h` — the units and the two families of
   source. Every rule is asserted with an expected value in
   `tests/test_mod_router.cpp:the pitch destination is the destination's own units` rather than measured
   off a waveform.
4. The `keytrack_value` halves, if you care about 2.3 % of slope at the bottom of the keyboard.
5. **The wheel is not a parameter.** It is `Voice::set_mod_wheel`, set per channel by
   `Engine::push_channel_controllers` (renamed from `push_channel_gains`, and it now pushes both), because
   a patch that zeroed a mod wheel when it loaded is the bug the plan already names for program change.
   If you would rather it lived in `Params` so a patch could hold one, say so before the panel does.

## What is measured rather than asserted

- **Vibrato: 6.17 Hz against the 6.0 the LFO was set to** — the voice's own pitch per block, cycles
  counted, 17 of them in 2.757 s, so the resolution is a cycle and the drift is quantisation.
- **Env2 → cutoff opens the filter at the strike**: 0.0001 at the strike, **0.1128** mid-decay, 0.0001
  after the release, against an unmodulated control that stays at 0.0001 throughout. The cutoff starts
  below the 60 Hz floor of a 64-frame FFT, so what the test reads is the level that gets through, and it
  moves by a factor of a thousand.
- **Pan is arithmetic, not measurement**: both channels are the same sample with a constant gain, so the
  ratio of their peaks *is* `tan((pan+1)π/4)`. The test asserts 2.4142 where the pan is +0.5, exactly 1.0
  where a bipolar cable centred itself, and exactly 1.0 with no depth whatever the velocity does.
- **The levels are exact int16 peaks**: 0.297525 unmodulated, 0.111847 with osc3 taken to its floor,
  0.297639 where a second slot cancels it again — a cable that sums is a cable that can cancel.
