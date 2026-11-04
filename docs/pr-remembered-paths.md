fix: a remembered patch and library are remembered as paths that still resolve

Phase 9.4 wrote down *the name the app had used*. For the two ports that was right, and it is still
the rule: the port the driver answered with, not the index that asked. For paths it was wrong,
because the name the app had used was relative to wherever that process happened to start.

Reproduced on the machine rather than reasoned about — `settings.txt` holding exactly what 9.4 wrote,
started from `build/`:

```
boreaz: starting without the patch from last time: cannot open the parameter file 'patches/piano-fortissimo.txt'
```

The voice the player had open was gone and the shipped defaults came up instead. That is this phase's
own fallback — warn, keep playing, never silence — reached for a reason nobody could read out of the
file in front of them.

**Fixed where the path is written, not by searching when the read fails.** `config::resolved_path()`
absolutizes a path and collapses `.` and `..` lexically; `patches_dir()` answers absolute; the exit
writes `patch` and `instrument` through it.

Two things that looked cheaper and are not:

- *Try the two patch folders when the remembered path does not open.* A bare `patches/x.txt` is then
  ambiguous — one `patches/` ships voices, one is the player's — and the app would choose one of them
  silently. The write is the only place the truth is known: the folder whose file this session opened.
- *Resolve with `weakly_canonical`.* That is the obvious call and it does not do the job here: in this
  standard library it leaves a **relative path relative** whenever the file does not exist yet, and a
  lot of these paths are files the first save is about to create. It failed two assertions the first
  time. `absolute()` plus `lexically_normal()` does not touch the disk and does the job.

One trap kept on purpose: `resolved_path("")` stays empty. An empty library path is the synthesiser;
turned into a path it becomes the working directory, and the next start would be told to load a folder
as a piano. There is a CHECK for that, not a comment.

**A patch file still keeps the name its author typed.** That file is written to travel between
machines; `settings.txt` is one machine's note about itself, so the two get different rules.

Measured on the machine, in order, real device (Realtek, 48 kHz, 512 frame period):

| | start | remembered | result |
|---|---|---|---|
| A | `build/` | `patch = patches/piano-fortissimo.txt` (what 9.4 wrote) | **1 warning**, defaults |
| B | `build/` | started with `--params ../patches/piano-fortissimo.txt` | file rewritten absolute |
| C | `build/` | absolute | **0 warnings** |
| D | repository root, no flags | the same file, unchanged | **0 warnings** |

Both clean runs printed their usual exit line: 0.71 % and 1.01 % audio-thread CPU, 0 restarts,
0 dropped, 0 steals. `boreaz_tests` is 283 cases / 8 861 608 assertions, one new case;
`release_check` green end to end and every golden unmoved — nothing in the engine changed, this is
config-side.

Not done, and said plainly: **an old `settings.txt` still holds the relative line.** The fix is at
write time, so one session at the mixer rewrites it; a start that reads the old line from a foreign
folder still warns and starts from the defaults, which is the honest behaviour and not a silent one.
`defaults_path()` (`config/params.ini`) keeps its relative walk up the tree — a start from a folder
above the repository finds no shipped defaults — but that is a lookup at start, not remembered state,
so it is out of this fix and on the list instead.
