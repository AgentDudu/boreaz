feat: a patch is a parameter file with a header, and the writer proves the round trip

Phase 9 is the phase where a parameter name stops being internal, because it
starts living in a file on somebody's disk. The reader has been ready for it
since the format was written — `params_file.h` says the render parameter file is
"the same shape as the patch files the preset phase will ship" — but nothing
could write one. This is the writer, and the reason it is a small change is that
the decision underneath it is "do not invent a second format".

**A patch file is a parameter file.** Three comment lines on top, then every key:

    # boreaz patch format 1
    # name: Bright Grand
    # instrument: assets/instruments/grand/library.txt

The header is comments on purpose. The reader refuses an unknown key — that is a
decision from phase 7, and it is why a misspelled parameter cannot make a silent
patch — and a patch's name and its sampled instrument are not engine parameters.
A comment is the one place in this file the existing reader already ignores, so
`--params mypatch.txt` loads a patch with no new loading code, and every
parameter file that has ever existed is still a valid patch with empty name and
empty instrument.

**The numbers are written as the shortest text that reads back as the same
float.** `%g` gives six digits and loses a slider position a tenth of a decibel
away; `%.9g` keeps every bit and writes `0.100000001` for a value the person set
to 0.1. `std::to_chars` with no precision is the shortest round trip in both
directions, so the file says `amp_sustain = 0.7`. Whole-number rows are written
as whole numbers: `octave_shift = 2`, not `2.0`, because the reader rejects
`octave_shift = 1.5` and would reject `2.0` as well — a writer that printed a
decimal point would write files it cannot read back.

**Every parameter is written, every time.** A file that skipped the parameters
sitting at their default would be a dozen lines shorter and would change meaning
whenever a default changed, which is the quiet bug a preset format is most likely
to have.

The file is built in memory and written in one go. A patch that stops half way
through its parameters is a file that loads and is wrong, and that is the one
failure this format has to make impossible.

## Proof

272 cases, 8 860 423 assertions, up 1 034 from the new test:

- each of the **63 rows at its minimum, its maximum and its default** round-trips
  bit-exact — `==`, not `Approx`, because a value one bit away is a patch that
  does not sound like the one that was saved;
- **save, load, save: identical bytes**, at the defaults and at all 189 edge
  cases. That is phase 9's exit criterion "round-trips byte-for-byte", for the
  parameters. `transpose_semitones`, `octave_shift` and `tuning_reference_hz` are
  covered; `instrument` is carried in the header and comes back, `voice-kind` is
  not a parameter yet so it is not in the file at all;
- the eight cables that live inside an `std::array` are the rows a writer gets
  wrong when it guesses how to reach a parameter instead of asking the table, so
  they are written and read back at both ends of their range;
- a name or path containing a newline is written as a space instead of as a
  second header line that would read back as something else.

`release_check` green end to end, including the app on the real device — the
goldens did not move, because nothing here touches a render path.

## Not done

Nothing writes a patch from the app yet: there is no Save key, no save directory,
no browser. `save_params_file` and `read_patch_meta` are library calls, tested
against a temp directory. And PLAN's exit criterion still asks that a corrupt or
unknown-key patch file "loads with warning and keeps sane defaults instead of
dying" — today a bad key stops the app, which is right for a render (silently
wrong parameters are worse than no render) and probably wrong for a preset
browser. That is a decision rather than an omission, so it is left open rather
than changed under a rename-sized PR.
