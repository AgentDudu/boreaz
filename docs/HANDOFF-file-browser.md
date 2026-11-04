# HANDOFF — the song folder browser, and the waterfall's top edge (2026-11-04)

Not a PLAN phase. PLAN stops at 12 and phase 12 is merged; this is player-requested work on top of
it. Read `docs/pr-file-browser.md` for the measurements; this file is only about where the tree is
and what is still unproven.

## Where the tree is

| | |
|---|---|
| develop | `b3bf67a` (the merge of #51, the spectrum panel). This work goes in as its own PR against `develop` — `main` is untouched, as asked |
| new files | `include/boreaz/io/directory.h`, `src/io/directory.cpp`, `tests/test_directory.cpp` (9 cases), `docs/pr-file-browser.md` |
| touched | `src/gui/app_window.cpp` (the explorer window, F3/Up/Down/Left/Enter/Escape, errors under the field), `src/gui/viz/waterfall.cpp` (the speed bar and the leaving band), `CMakeLists.txt`, `tools/key-in-window.ps1`, `tools/key-in-window.md` |
| tests | 322 cases, 8 877 474 assertions, green. `ctest` 1/1 |
| pixels | still unseen: a GL read-back of the window is black on this machine, as in phases 10-12 |

## What was learned that the next person needs

- **`tools/key-in-window.ps1` was posting the arrow keys wrong.** `VK_DOWN` without bit 24 of
  `lParam` arrives as `GLFW_KEY_KP_2` (322), and `MapVirtualKey(..., MAPVK_VK_TO_VSC_EX)` does not
  report the `0xE0` prefix here. Fixed by setting the extension bit for the arrow/navigation keys.
  Any earlier "the app ignored the arrows" reading is suspect; re-measure before repeating it.
- **ImGui keyboard navigation and this app's own key handler were both acting on the same keys** in
  the explorer. `ImGuiWindowFlags_NoNavInputs` on that window makes Enter mean one thing.
- **`parent_folder(".")` is `"."`**, so a picker seeded with `"."` cannot be walked up out of. That is
  why `current_folder()` exists.
- **`"C:"` is not a folder to the filesystem** (it means "the current directory on C:"), so
  `list_folder` puts the slash back. Walking up to a drive root works now.
- A 4000-name folder walk costs ~388 ms and is bounded at 4000 names on purpose, on the click, off
  the audio thread (`0 dropped, 0 steals` in the same run).

## Open, on purpose

- The click path and Enter share `pick_folder_entry`; only the keyboard route is measured.
- No filter/type-ahead inside the listing — the field is the fast path to a huge folder.
- The tap's startup backlog (7680 frames before the window exists) is phase 12's, untouched.
- `7680` and the black capture are the two things that keep "it looks right" unclaimed.
