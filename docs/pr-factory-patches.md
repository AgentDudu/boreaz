feat: eight factory patches, and a saved patch can no longer overwrite one of them

Phase 9's browser had nothing to browse: `patches/` did not exist, so the panel's
list was empty and `fx_reverb_mix = 0` kept the "should a dry grand have a room?"
row open. Eight files now answer both: five registers written for the whole A0-C8
compass (`sub-register.txt`, `bass.txt`, `low-mid.txt`, `upper.txt`, `brilliance.txt`) and
three piano dynamics (`piano-pianissimo.txt`, `piano-mezzo-forte.txt`, `piano-fortissimo.txt`).

**They are generated, not typed.** `tools/factory_patches.py` holds one small
delta per patch and expands it against `src/engine/param_table.cpp`, which stays
the only place a name, range or default lives — the rule §3 states. 677 lines of
`key = value` copied by hand is a second copy of sixty-three defaults, and a
default that moves in the table would leave the copies behind. `--verify` prints
nothing while the files and the recipe agree and lists the differences when a
hand edit makes them disagree.

**Every patch is complete: 63 keys, every time.** `adopt_patch` layers a file onto
whatever is already playing, so a key left out is not "that key unchanged" — it is
"that key whatever the last patch happened to leave". A patch that opens by
accident has to be a whole sound, not somebody else's sound with one parameter
touched. `tests/test_factory_patches.cpp` asserts the properties that rot: the set
is the registers and the dynamics, one assignment per parameter row and no other
key, a name in the header, and at least three patches carrying a room. It also
asserts the whole 63-row sweep through `set_param_from_text` for every file, so a
patch that puts a row outside its own range fails at review, not in the mixer —
which is how one of these files got caught: the first `bass.txt` set
`mod_env_sustain = -0.35` because `Params::mod_env_sustain` is an old signed
depth (-1..1) while `mod_env_pos` treats that row as a 0..1 level, exactly the
sign convention the generator's own comment says it is not safe to read off the
`Params` field. The generator's numbers are levels; the table's ranges decide.

**A factory patch names no instrument.** A library path is a path on one machine
and §5 keeps machine-local paths out of git, so the instrument line is left off —
an empty one is already the loader's rule for "keep the library that is playing".
The sampled presets therefore work off the library the player already loaded.

**Measured by render, four fixtures, best of 3.** Nothing leans on the ceiling.
The loudest patch on the heaviest fixture is `piano-fortissimo` at **peak 0.511**
with 16 voices and 8 steals; the shipped `config/params.ini` on the same fixture
is 0.869. The dynamics presets measure the master gain they are named for:
**0.144 / 0.282 / 0.511** for pp / mf / ff, which is -6 / 0 / +6 dB. CPU on that
fixture: **1.01-1.08 % of one core** for the registers, **0.54-0.55 %** for the
piano presets, against 1.03 % dry synthesiser and 0.36 % dry sampled — all of it
inside the 8 % row, with the sampled side higher only because of the room:
`piano-mezzo-forte` measured with `fx_reverb_mix = 0` runs 0.19 % on four voices
and 0.36 % on sixteen, with it running 0.29 % and 0.57 %. That is a cost per
sample of the FMO64 block, not per voice, which is what phase 8 said; the peak
does not move at all (0.123 and 0.282 both ways).

**The room's shipped number, decided and written into §7: 0.35 / 0.30 / 0.18**
(pianissimo, mezzo forte, fortissimo), registers at 0. A full send sits 13.5 dB
under a held note, so 0.18 on a fortissimo puts the room about 21 dB under the
note instead of on top of it, and softer playing gets the bigger room because the
room is what makes a quiet note sound played rather than recorded in a cupboard.
This closes the §6 row about a dry grand the way phase 8's decision said it would
be closed: as a patch, not as a number in `Params`.

**The overwrite the factory patches made possible is closed in the same change.**
Once `patches/` holds a shipped voice called `Bass`, a save typed "Bass" lands on
it, and the folder git tracks makes that somebody else's modified file. Saves now
go to `%APPDATA%/Boreaz/patches` via `config::user_patches_dir()` (`patches-user`
beside the app where there is no `APPDATA` at all); the browser lists the shipped
voices first and the player's own under them; and a test asserts that nothing in
the folder the app writes to is one of the files the repository tracks. Nothing
machine-local is in git, and now nothing in git is machine-writable.

Two comments in the config files were found describing a design the engine does
not have: `osc2_level` is not "the saw an octave below" and `osc3_level` is not
"the fifth above", and `config/params.ini` calls the oscillators "three parabolas".
`synth_voice.cpp` and `synth_voice.h` put a sine at the note, a sine an octave
above and a saw in unison. The comments were fixed; the engine was not, because
the goldens are the evidence of what the engine does.

277 cases / 8 861 545 assertions. `release_check` green end to end, both render
gates passing, goldens unmoved (synth `08eb2e60c42ca135` / peak 0.142584,
sampled `8d2611517da3c819` / peak 0.117014).

Not done, and not pretended: nobody has pressed the new folder headers or a patch
button in a real window — this repo's ImGui tests draw the panel, they do not
click. And nobody has touched the shipped master default, which is worth noticing
rather than acting on: at the shipped `master_gain_db = -4` with
`fx_limiter_threshold_db = -4`, a unison saw held across all 16 voices peaks at
**-3.35 dBFS**, 8 dB above the limiter's threshold. A fortissimo factory patch
sits at 0.511 (-5.8 dBFS) and does not lean on that at all. The device config
file, autosave on exit, and the `voice_kind` half of phase 9's round-trip
criterion (no such parameter exists) are still open.
