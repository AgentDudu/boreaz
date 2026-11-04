# The song folder, opened from the keyboard — and the waterfall that reaches its own top edge

Two things the player asked for, both about seeing and reaching:

1. **"Add a folder explorer when I input a song file. Errors should just appear in that input
   field."** The field could already be opened with F1 and typed into, but the only way to *find* a
   file name was to remember it or paste it. There is now a listing: `Browse` beside the field, or
   F3, and the listing is walked with Up / Down / Enter / Left / Escape.
2. **"The scroll adjustment for speed in the waterfall isn't visible. There should be a bar that
   visualizes the adjustment (click and drag still). The waterfall effect should still go up until
   the edge of the area."** The speed control was a `DragFloat` — a number you rub sideways, with no
   shape to it. And the bars died 30 % of the plot height *below* the top edge they were travelling
   to, which is what "does not reach the edge" looks like.

## What is in it

| Piece | What it does |
|---|---|
| `boreaz::io::list_folder(folder, into, limit, more, error)` (`include/boreaz/io/directory.h`, `src/io/directory.cpp`) | One walk of one folder: sub-folders first, then the names `reads_as_song` accepts, case-insensitive, each ordered by name and then by bytes on a tie. `more` says how many did not fit. |
| `boreaz::io::current_folder()`, `parent_folder()`, `reads_as_song()` | The three questions the picker asks the filesystem, in one place. `current_folder` is absolute because `"."` has no parent: `parent_folder(".")` is `"."`, so a walk up from a seeded `.` lands on the folder you were already looking at. |
| `AppWindow::draw` | The explorer window: the folder in hand, its rows, `Up one folder`, `Close`. Opened on the click/F3, never re-read per frame. `ImGuiWindowFlags_NoNavInputs` on it, so the arrow keys and Enter belong to one handler and not to two. |
| F3, Up, Down, Left, Enter, Escape | The whole route without a mouse. Left goes out one folder; Enter on a file is the same call the `Load` button makes; a file that loaded closes the explorer. |
| `src/gui/viz/waterfall.cpp` | `SliderFloat` instead of `DragFloat` (click and drag unchanged, but the fill now shows the value as a length), and the leaving band is 20 fixed pixels at the top edge drawn as a gradient (`AddRectFilledMultiColor`) with the rest of the bar flat, instead of one alpha over the whole bar starting 30 % of the height down. |

`CMakeLists.txt` gained `src/io/directory.cpp` and `tests/test_directory.cpp`.

## Errors go where the name was typed

The reason a file was refused is drawn under the field, and the field itself turns red:

```cpp
const bool refused = !impl_->song_note.empty();
if (refused) { /* red text, red field background */ }
```

A listing that cannot be read says so **in the explorer**, under the folder name, and keeps the
folder it had: a junction the player cannot open is not a reason to lose the folder they were
standing in. The `...and at least N more` line is what a bounded walk can honestly say — it stopped
counting, it did not finish counting.

## Measured

**Unit.** 322 cases, 8 877 474 assertions, all green (9 cases are new: a folder of 20 mixed names
listed at a limit, folders-first order, the fold, `more`, a folder that is not there, the drive
root, `current_folder`, `reads_as_song`).

**The keyboard route, on a live window.** Started in `tests/fixtures/midifile` with no song, then
F3, Left, Down, Enter, Down, Down, Enter, each key posted through `tools/key-in-window.ps1`:

```text
boreaz: "...\tests\fixtures\midifile\glissando-88.mid": 176 events, 17.75 seconds.
boreaz: 1985 callbacks, 306 late, 0 restarts, 0 dropped, 0 steals, position 1016320 samples,
        3424 frames, 0.57957% audio-thread cpu.
```

Out of the starting folder, back into it, and the song plays. A second run (before the starting
folder was made absolute) picked row 0 of the starting folder and printed
`bass-running-status.mid": 32 events, 4.875 seconds.` — which is how the dead end was found.

**How long a walk costs.** `list_folder` timed on this machine (a scratch program, `limit` 300):

| folder | rows offered | `more` | wall |
|---|---|---|---|
| the build directory | 16 | 0 | 2.96 ms |
| `C:/` | 19 | 0 | 1.54 ms |
| `C:/Program Files` | 57 | 0 | 4.73 ms |
| `C:/Windows/System32` | 122 | 1 (the walk stopped at 4000 names) | **387.71 ms** |

387 ms is the honest number for a folder that size, and it is spent **on the click that opened the
folder**, once — not per frame, and not on the audio thread: that run answered 1985 callbacks with
0 dropped and 0 steals. The walk is bounded at 4000 names precisely so the worst case is a hitch
rather than a hang; a bigger folder is reached by typing its name at the field.

**The waterfall with the bar and the new band.** Same recipe as the phase-12 measurement
(`--gui --midi none --params patches/piano-fortissimo.txt glissando-88.mid`, killed after the song):

| panel | callbacks | late | dropped | steals | frames | audio-thread cpu |
|---|---|---|---|---|---|---|
| waterfall (this change) | 1977 | 304 | 0 | 0 | 3414 | 1.081 % |
| spectrum, F2 partway | 2056 | 317 | 0 | 0 | 3540 | 1.541 % |
| waterfall, phase-12 binary (same recipe) | 1817 | 280 | 0 | 0 | 3127 | 1.010 % |

Up to two gradient quads per bar instead of one flat quad changed nothing measurable about the
audio, and the frame count went *up*, not down. Frame counting is the window's own, so it disagrees
with the wall-clock method from earlier phases in the absolute number and agrees between views; no
claim of ≥60 fps is made here beyond that.

**The key injector was wrong, and that was mistaken for a dead feature.** `VK_DOWN` (40) posted
with the scancode from `MapVirtualKey(vk, MAPVK_VK_TO_VSC)` arrives as **`GLFW_KEY_KP_2` (322)**,
not `GLFW_KEY_DOWN` (264): the arrows are extended keys and their make code needs the `0xE0` prefix,
which GLFW reads from bit 24 of `lParam`. `MAPVK_VK_TO_VSC_EX` does not report that prefix here, so
`tools/key-in-window.ps1` now sets the bit for the arrow / Home / End / PageUp / PageDown /
Insert / Delete virtual keys. Before the fix every "the app ignored the arrow keys" reading was the
harness, not the app. `tools/key-in-window.md` says so.

## Not proven, and where it bites

- **Nobody has seen the pixels.** A GL read-back of the window comes away black on this box, as in
  phases 10 and 11. The slider bar, the red error line, the folder listing and the 20-pixel band at
  the top edge are measured as code paths, counters and log lines; their appearance is not.
- **The mouse route to a row is not measured.** A click on a row and Enter go through the same
  `pick_folder_entry`, so the keyboard run proves the behaviour and not the click.
- A 4000-name folder costs ~0.4 s of frozen window on the click. It is one click, bounded, and off
  the audio thread; it is not invisible.
- The explorer's rows are `Selectable`s in a `BeginChild`, so a listing of 300 rows scrolls; there is
  no type-ahead filter inside the listing — typing a name at the field and pressing Enter is the
  fast path to the 30 000-file folder.
- ImGui's own keyboard navigation is switched off for that window. One Enter press therefore does
  one thing, which was not the case while both this handler and ImGui's focus walker were reacting to
  the same key.
- The startup backlog from phase 12 (`7680 frames` already in the tap before the window exists) is
  untouched.
