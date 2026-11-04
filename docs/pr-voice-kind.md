test: a patch carries its voice kind, and now something proves it

PLAN's phase-9 criterion names five things the round trip has to cover:
"transpose/octave/tuning-ref/voice-kind/instrument". Three are parameters
(`transpose_semitones`, `octave_shift`, `tuning_reference_hz`), and they were
already asserted away from their defaults — `test_live_params.cpp` writes -3, +2
and 432 into a patch, reads them back, and writes the same bytes again. The
"instrument" is a header field that `save_params_file` writes and
`read_patch_meta` reads, also asserted.

That left `voice_kind`, which the tree does not contain: 0 hits. It looked like a
missing parameter. **It is not, and no parameter was added.**

**The kind is which library is playing, and it already travels — as a name,
twice.** A patch's header names the library; `adopt_patch` puts that name in front
of the same queue the Load button uses, so the samples change when the pool may be
rebuilt, not while notes are sounding. A patch that names nothing says nothing
about the instrument and keeps what is playing — a preset does not smuggle the
synthesiser in. A patch whose library has gone away keeps all sixty-three of its
parameters, changes no voice pool, and prints the reason in the panel's own line.
Across a restart the app's setup file names the library the next start opens.

**What was missing was not the mechanism, it was the proof.** Nothing in the suite
reached that path, because the suite draws the panel and cannot click it. Two
members are public now — `adopt_patch`, which is what a browser row calls, and
`status()`, which is the panel's line and the only way to tell a warning from a
silence — and one case, `a patch carries its voice kind`, asserts the three
branches: names a library (asked for by that exact string, applied once nothing
sounds, `last_patch()` moved, the tuning row arrived at the same moment), names
nothing (nothing asked for, the library stays), and library gone (asked, refused,
no second pool, the reason visible, the parameters still in).

**Why a `voice_kind` row is the wrong answer, in one sentence:** a kind is a value
that can be true about a sound that is not happening — `voice_kind = sampled` with
the samples missing describes a library nobody can load, and this phase's own rule
is "a missing or unreadable instrument falls back to the synth with a visible
warning, never silence". A stored kind would be contradicted by the first run that
followed it. It would also owe the engine a meaning that no parameter has today:
nothing selects what a voice *is*, and the sampled path has no filter, no
modulation matrix and no oscillator to switch, so the row would be a number with
nothing on the other end of it. PLAN §7 has the decision row; the phase-9 row now
says what its five names actually are.

**Not a behaviour change.** No engine file, no parameter, no patch file, and no
gate moved: 282 cases / 8 861 600 assertions, `release_check` green end to end,
goldens untouched (synth `08eb2e60c42ca135` / peak 0.142584, sampled
`8d2611517da3c819` / peak 0.117014).

Not done, and not pretended: **nobody has clicked a patch row.** The new case
calls the method a click calls, which is the same code reached by a different
trigger; the click itself, the two folder headers in the browser, and what the
eight factory patches sound like are still for a person. Phase 9's row is now
complete on paper — a human at the mixer is the thing between this phase and its
exit. One thing noticed and not fixed: PLAN's phase table has extra pipes inside
phase 9's row (7 where the table has 4 columns), so that row spills into columns
the table does not have. The text renders; the grid does not.
