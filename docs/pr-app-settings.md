feat: the app remembers the machine it was closed on

Phase 9's last deliverable was "a config file for audio/MIDI device prefs + last
patch + instrument path, autosave on exit". It is in, and it is smaller than it
sounds: **four names in one file**, `%APPDATA%\Boreaz\settings.txt`, read at every
start and written at the end of a session that had a window.

```
# boreaz app setup 1
audio_device = Speakers (Realtek High Definition Audio)
midi_port = auto
instrument = D:\instruments\grand\library.txt
patch = %APPDATA%\Boreaz\patches\bass.txt
```

**No parameters, and the file is not a patch.** `audio_device` is not a row in
`param_table.cpp`, and a test asserts that in both directions: none of the four
names is an engine parameter, and the strict parameter reader refuses this file as
an unknown key. Nothing at exit rewrites a parameter file either — closing the
application is not allowed to edit a sound, and the patch that was open keeps the
bytes the player saved. What the file holds is the *name* of the patch, which is
why phase 9's byte-for-byte round trip still belongs to the patch and not to the
session.

**Two files, two rules, and the asymmetry is the decision.** A parameter file is
taken whole or refused whole (a half-read sound is a sound nobody wrote and nobody
can name). A setup file is read line by line and it forgives: an unknown key is
skipped and named with the number of the line it was on, a line that is not an
assignment is reported and its neighbours still load, no file at all is a first run
that says nothing, and a file written by a newer build starts on an older one. A
start that dies on a line nobody can see has no way out. §7 records this.

**Precedence is the newest thing said** — the rule `--gain` already follows. A flag
at the prompt, then this file, then the program's default; `--no-instrument`
outranks a remembered library. Telling "typed" from "defaulted" needed two booleans
in `Options` (`midi_given`, `instrument_given`), because an empty `--device` and an
`--midi auto` are also what the program does when nobody says anything.

**What is written is what was used, not what was asked.** The exit writes
`port_name` — the name the driver answered with — rather than the `--device 3` that
asked for it. A remembered index moves when the order of the ports changes; a name
does not.

**Stale memory is a decision, not an edge case, and all three kinds are handled.**

- A remembered audio port that has gone away (asleep interface, laptop off its dock)
  is named, called the app's own memory rather than a request, and the system
  default is opened. A port **typed** at the prompt still fails hard: a person asked
  for that port and deserves to be told they did not get it.
- A remembered patch that cannot be read says `starting without the patch from last
  time: ...` and plays from the defaults. That path is a promise this app made, not
  one the player made today, so it is not allowed to stop the instrument.
- A remembered library that cannot be read keeps phase 9's rule: warn, play the
  synthesiser, never silence. And because the exit writes the library that is
  *actually playing*, one session clears a stale path by itself.

**A run with no window writes nothing.** The save sits behind `if (window !=
nullptr)`: playing one file headless is a command, not a preference, and it would
otherwise remember the port a test happened to use.

`ParamsPanel::last_patch()` is the one fact the panel hands the app at close —
recorded inside `adopt_patch`, at the moment the parameters go in — and `main` falls
back to the patch the session *started* with when nothing was clicked. The library
is main's own bookkeeping (`live_library`, set at startup and in the load
callback), so it is the library the mixer asked for and got, which can be one still
waiting for the notes to stop.

**Measured by running the app, not by render.** A start with a remembered library
and patch loads both: `samples: 66 regions ... 109 MB resident, 165.508 ms to load`,
`patches/piano-fortissimo.txt`, no complaint. A window session then rewrote the file
with the port it used, that library and that patch. `--device 1` beat a remembered
`3`; `--no-instrument` beat a remembered library. A remembered
`Nonexistent Interface XYZ` was named, the default was opened, and that run still
played (283 callbacks, 0 restarts, 0 dropped). A remembered `patches/nope.txt`
reported and started from the defaults.

**`tests/test_app_settings.cpp`, four cases:** the file the app wrote is the file the
app reads, byte-stable across a second save, with spaces, brackets and backslashes in
the values; an unknown key is skipped and named with its line; no file is a quiet
first run and a broken line is reported without taking its neighbours down with it
(and a value that contains `=` survives, because the split is at the first one); and
the separation from the parameter file, in both directions.

**The gate is kept hermetic in the same change.** `release_check`'s real-device run
now starts `boreaz.exe` with `APPDATA` pointed at an empty folder under `build/`,
because the app reads the machine's memory at every start and that run passes no
`--patch`: without it, whatever patch the last windowed session left behind would be
part of what the gate measures. `--list-devices` gets the same environment, since its
output feeds the port the gate then opens.

281 cases / 8 861 575 assertions. `release_check` green end to end, both render
gates passing, goldens unmoved (synth `08eb2e60c42ca135` / peak 0.142584, sampled
`8d2611517da3c819` / peak 0.117014).

Not done, and not pretended: **nobody has clicked a patch row.** "The patch you
clicked is the patch the next start opens" is therefore proven only for the patch a
session *started* with — `last_patch()` is assigned inside `adopt_patch`, which the
suite draws but never triggers, and there is still no click harness. The
`voice_kind` half of phase 9's round-trip criterion still needs an operator answer
(no such parameter exists: the voice kind is implied by whether a library loaded,
which is one of the four names here). One thing noticed and not fixed: the hygiene
test's Windows machine-path pattern does not match the escaped `C:\\Users\\` form
that appears in C++ string literals, so a machine path can hide in a test body;
these tests use `D:\` paths instead, so nothing leans on the gap.
