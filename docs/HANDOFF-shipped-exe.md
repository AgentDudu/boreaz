# HANDOFF — the exe that ships in a zip (2026-11-04)

Not a PLAN phase. It is a question from the person who asked for the folder browser: "is there a way
for our app to be an exe?" PLAN stops at 12; `CI release` is one of the items PLAN parks after it.
Read `docs/pr-shipped-exe.md` for the measurements; this file is about where the tree is and what is
still unproven.

## Where the tree is

| | |
|---|---|
| develop | `a4d6f66` (the merge of #53, the folder search). This went in as its own PR against `develop` — `main` is untouched, as asked |
| new files | `cmake/version.rc.in`, `docs/pr-shipped-exe.md`, this file |
| touched | `CMakeLists.txt` (the `.rc` into `boreaz`, `-s` on the Release app), `include/boreaz/config/params_file.h` + `src/config/params_file.cpp` (`executable_folder()`, the two path lookups), `src/app/main.cpp` (`--version`), `tests/test_app_settings.cpp` (one case), `README.md` (a Release section) |
| the artifact | `build/boreaz.exe`, **3 972 096 bytes** (was 6 290 036), imports only OS DLLs, `FileVersion 0.1.0` in its properties |
| tests | 326 cases, 8 877 498 assertions, green. `ctest` 1/1 |
| version | still `0.1.0` everywhere — `project(boreaz VERSION …)` and `include/boreaz/version.h` are checked against each other by the build, so a bump is one line in each and nothing else |

## What was learned that the next person needs

- **The exe was already portable; the failure was a path, not a link.** `objdump -p` names only OS
  DLLs, and `-static -static-libgcc` has been in `CMakeLists.txt` since the two-MinGW-runtimes-on-PATH
  problem. What broke a release was `defaults_path()` asking the **working directory** and nothing
  else: `boreaz: cannot open the parameter file 'config/params.ini'`, exit 1.
- **Thirteen phases of measurement hid it, because every run was from inside the repository.** A
  claim about "the app" measured from a folder in the source tree says nothing about the folder a
  double-click gives it. Start at least one run from a copied layout with a working directory the
  repository is not near.
- **`here + "/config/params.ini"` and `std::filesystem`'s own spelling of the same file are not the
  same string** — one has a `/` in the middle of a path of `\`s. Comparing them in a test fails until
  the function returns `resolved_path(candidate)`, which is what the failure message wants printed
  anyway.
- `windres` is on this box (Strawberry's binutils 2.42) and CMake used it with no
  `enable_language(RC)`: putting the generated `.rc` in the target's sources was enough. The resource
  costs 5 106 bytes.
- **`-s` was measured before it was wired** by stripping a *copy* of the existing exe — one command,
  2.32 MB, no rebuild. Then the flag, then `--version` and `--list-devices` on the result to prove the
  stripped image still runs. A stripped Release exe has no function names in a stack trace; the tests
  and the tools keep symbols for that reason.
- **Do not `git stash push -a` in this repository.** The build tree is untracked, so `-a` swept
  `build/` into the stash and left the tree with no executables. It came back with `git stash pop`;
  use `git add <the files that are yours>` instead, which is also how the recurring clang-format
  churn from the watcher is kept out of a commit.
- Scratch layouts live in temp folders and are safe to delete: `%TEMP%\boreaz-release`,
  `%TEMP%\boreaz-fresh`, and (from a mistyped path) `%LOCALAPPDATA%\Local\Temp\boreaz-release`.

## Open, on purpose

- **No CI job builds the artifact.** `CI release` stays in PLAN's parked list.
- **Unsigned.** A download will raise SmartScreen's "unknown publisher". Signing is a purchase, not a
  patch, and nobody has tested what antivirus makes of a stripped static MinGW exe.
- **Never run on a machine that has never had MinGW on it.** One Windows install here. The import
  table is the evidence, a clean VM would be the proof. Windows 10/11 only by decision; 8.1 wants
  KB2999226, Windows 7 wants the old DirectX redistributable.
- **The patch browser has still never been clicked.** `patches_dir()` beside the exe is proven by a
  probe binary, not by the UI listing them.
- No icon in the resource, no installer, no `--diagnostics` build. `README.md` still says "18 sliders"
  where the panel has 63 — unrelated to this change and left alone.
