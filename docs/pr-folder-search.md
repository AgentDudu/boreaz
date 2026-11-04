# The folder search — and the frame that was reading a listing it had just replaced

**"The folder browser should be like in those normal apps (modern) for faster search rather than
manually go up or down one dir at a time, especially when searching at a different drive."**

The explorer from the previous increment had one way to move: `Left`, one folder at a time. Reaching
a song on another drive from the folder this repo sits in is dozens of presses, and a folder of a
few hundred names is a scroll. Three things replace that:

| Piece | What it does |
|---|---|
| Address bar (`##where`) | The folder you are in, said absolutely, and editable. Type another folder or another drive and press Enter (or `Go`). F4 hands it the keyboard. |
| Drive buttons | `boreaz::io::drives()` — one button per drive letter the machine answers on. One press, no climb. |
| `narrow` field | A case-insensitive substring that filters the rows **as it is typed**, plus a `clear`. F5 hands it the keyboard. Enter in the field takes the top row the filter left. |
| `absolute_folder` | `.` / `..` / `C:` become a folder a person can read and paste back, with one separator. The bar shows this, so it is never an expression. |
| `name_contains` | The search uses the same case folding as the sort, so the two rules about case cannot drift. |
| pending pick | A row chosen by click or by filter-Enter is noted and acted on **after** the child window closes. See the crash below. |

Keys, in full: F3 opens, F4 the bar, F5 the filter, Up/Down walk the rows the filter left, Enter
picks the highlighted row (or, while the filter holds the keyboard, the top one), Left goes out one
folder, Escape leaves a field and then closes the explorer. F-keys and not letters because every
letter on this keyboard is a pitch.

## Measured, keyboard only, no mouse

`tools/walk-to-song.ps1` starts the app with no song and drives the whole thing with posted keys.

| Step | Keys | Result |
|---|---|---|
| open | F3 | explorer at the folder the app stands in |
| jump to another drive | F4, `C:/`, Enter | the listing is the drive root — no walking |
| six hops | F5, a few letters, Enter — `Users`, the account folder, `VSC_Projects`, `boreaz`, `build`, `.mid` (the account name is read from the environment by `tools/walk-to-song.ps1`, never written into it) | each hop landed on the folder (or file) the letters named |
| the last hop | `.mid` + Enter | `boreaz: "<home>\VSC_Projects\boreaz\build\drive-ab.mid": 176 events, 17.75 seconds.` (the home part scrubbed; the hygiene test in `tests/test_hygiene.cpp` is why) |

So: drive root → a song file, seven keystroke groups, no mouse, and the proof is the load line on
stdout, which only appears if every hop picked the right row. The same run reported
`1628 callbacks, 251 late, 0 restarts, 0 dropped, 0 steals, 2772 frames, 0.70 % audio-thread cpu` —
filtering every frame did not steal a single buffer.

The filter's cost, measured on 300 names of the shape the listing holds:

| needle | µs per frame (300 names) |
|---|---|
| `glis` (no match) | 60 |
| `so` (all match) | 45 |
| `number-299` (last name) | 59 |
| `zzz` (no match) | 50 |

~0.3 % of a 16.7 ms frame, rebuilt every frame rather than dirty-tracked. (Bench in
`build/_scratch/filter_bench.cpp`, not shipped.)

`drives()` on this machine answers one root (`C:`; `Get-Drives` agrees: one NTFS drive), so the
button row here is one button — the drive *row* is proven, several buttons are not, on this box.

Tests: 325 cases / 8,877,474 assertions, `ctest` 1/1. Three cases added — `absolute_folder` (folds
`..`, one separator, idempotent, `"C:"` → `C:` root), `name_contains` (folds both sides, empty
needle matches), `drives` (non-empty on Windows, every entry `X:/`).

## The bug this found, because it was in the code before this increment

The walk crashed twice before it worked:

```text
#1 std::__throw_length_error()      what(): basic_string::_M_create
#2 boreaz::gui::AppWindow::draw(...)
#3 boreaz::gui::AppWindow::frame(...)
```

A temporary `fprintf` probe in the same place died one way (`ucrtbase!strlen`, a segfault) and the
shipped code another (`length_error`), which is the same root seen twice: **the rows of the explorer
are positions in `browse_entries`, and picking a folder replaces that vector.** With the click and
the new filter-Enter picking *inside* the row loop, the loop went on reading positions of a listing
that no longer existed — out of bounds, and a `std::string` built out of whatever was there. The old
build had the same shape; it just had fewer ways to pick mid-frame. A pick is now noted
(`pending_pick`) and performed after `ImGui::EndChild()`, when the vectors are no longer being read.
Reproduced twice, absent in the run after the fix.

## Not proven, and where it bites

- **Nobody has seen the pixels**, as in phases 10-12: a GL read-back is black on this box. The bar,
  the drive buttons, the filter and the `N of M names` line are proven as code paths, counters and
  the load line.
- **The mouse routes are not clicked.** `Go`, a drive button, `clear` and a row click share their
  functions with the measured keyboard routes.
- Only one drive exists here, so `drives()` returning several roots is unproven on this hardware
  (the test asserts the shape of every root it does find).
- Enter in the filter takes the **top** row the filter left. To walk the rest: Escape (leaves the
  field), then Up/Down. Deliberate, and written in the tooltip.
- The filter belongs to the folder it narrowed and is cleared when the folder changes — the way
  Explorer behaves. Keeping a filter across a jump shows an empty listing under text typed for the
  previous folder, which is worse than losing it.
- The address bar refuses a file with `not a folder: <what you typed>` rather than loading it; the
  field at the top of the main window is the way to load by name.
- The 4000-name bound is unchanged: a monster folder still costs ~0.4 s of frozen window on the
  click, and the filter only narrows what that walk managed to return.
- The `7680 frames` startup backlog from phase 12 is untouched.
