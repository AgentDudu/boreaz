# The notice names both pianos, and two claims stop pretending

Branch: `fix/notice-and-gate-truth`. Merged into `develop`.

## What was wrong

Three things this repository said that the tree could not back up. All three were found by asking a
question of the file rather than of the compiler, which is why none of the gates had ever caught them.

1. **`NOTICE` described the wrong instrument.** It listed **Salamander Grand Piano v3** (CC-BY-3.0,
   modified subset) under "Instrument data (built by tools/mkinst…)" — while `README` tells a stranger
   to run `tools/mkinstrument.py`, which fetches **Upright Piano KW** from the FreePats release
   `v1.0.0` (hash-pinned zip), and that is the instrument `tools/release_check.py` and
   `tools/listen.py` actually play. Salamander is real — `tools/mkinst.py` builds it and writes a
   generated `NOTICE.txt` into `third_party/instruments/salamander/` — but the notice never mentioned
   the asset the documented path installs. Checked against the primary source in the tree, not against
   a memory: `third_party/instruments/upright-piano-KW/LICENSE` is **CC0 1.0 Universal**, and its
   `README.md` says "Published under the terms of Creative Commons CC0 public domain dedication".
   CC0 needs no attribution, so the added entry is a courtesy; the part that matters is that the terms
   travel with the audio, which they do because `mkinstrument` copies the upstream `LICENSE` and
   `README.md` next to the samples it decodes. No e-mail address from the upstream credit block is
   copied into this repository.
2. **"Sanitizer clean" was a claim with no recipe.** `PLAN` described an ASan-green run (233 cases,
   7 934 953 assertions) and named `build-clang` as "the ASan tree". Both `build-clang` and
   `build-asan` on disk carry **zero `-fsanitize` occurrences** in their compile commands. Measured on
   this machine: the primary GCC (MinGW-w64 UCRT 13.2 as shipped by Strawberry) cannot link it
   (`ld.exe: cannot find -lasan`), and LLVM 23.1.2 links an instrumented binary that dies at load
   (`ldd`: `clang_rt.asan_dynamic-x86_64.dll => not found`). So the sentence in `PLAN` was history
   being quoted as a current measurement, and the four clang-cl fixes it wrapped are the only part of
   it that still holds.
3. **Two rows promised a check that does not exist.** The asset table and the risk table both said the
   attribution is enforced by "CI fails a release whose instrument has no matching attribution", and
   both counted "credit in the about box" as a channel. This tree has **no hosted pipeline at all**,
   and the GUI has **no About panel**. What does carry the obligation today is the `NOTICE.txt` that
   `mkinst` writes into the artifact. Both rows now say that, and the about-box row is marked unbuilt
   instead of implied.

## What is in here

- `NOTICE`: both instruments, the right tool named for each, and the closing note pluralised
  (CC-BY-3.0 for Salamander, CC0 for the upright, neither covered by the PolyForm clause).
- `tools/sanitize.py`: the tracked sanitizer gate. It **probes first** — compile and then *run* one
  instrumented program — because linking ASan and running it are separate abilities and a toolchain
  can have either one alone. A GCC without libsanitizer fails the first half; this LLVM fails the
  second, and both print the reason instead of reporting a green the run never measured. The suite run
  itself keeps the flags and environment the historical run used (`ASH_VSEH=0`, no leak detection on
  Windows, and the `ASAN_*` names alongside `ASH_*` because either runtime may be the linked one), and
  builds into `build-san/` so an instrumented tree can never be confused with a measurement tree.
- `tools/promote_main.py`: builds `main` out of `develop`'s tree minus 29 working-note files
  (`PLAN.md`, `HANDOFF*`, `docs/HANDOFF*`, `docs/pr-*.md`), by plumbing: a throwaway index,
  `read-tree` → `rm --cached` → `write-tree` → `commit-tree`, no checkout and no working-tree churn.
  `main` is **promoted, never merged** — a merge would put the notes straight back, because develop
  edits them constantly. After writing, it checks its own work: `git diff --name-only <new main>
  develop` has to equal exactly the files it dropped, or it refuses to move the ref. That check is also
  the proof that promotion changes nothing a build depends on: the whole gap between the branches is
  markdown.
- `README`: the licence paragraph now names both instruments with their real terms, and a **Branches**
  section says what lives where — source, tools, tests, `README`, `LICENSE`, `NOTICE`, `config`,
  `patches` and `docs/PRIVACY.md` on `main`; the plan, the handoffs and the per-PR notes on `develop`.
  Source comments that cite `PLAN.md §n` therefore point at `develop`, and that is written down rather
  than left as a surprise.
- `PLAN`: the two asset rows and the toolchain bullet corrected, and the gate command block gains
  `python tools/sanitize.py`.

## Gate, run after these files were committed

Nothing C++ changed: `--golden`, both render paths, the release-envelope window table, the live
parameter bus, the keyboard layout, the SFZ render and `--list-devices` all ran green
(`OK: every gate ran`), and `ctest` reports **100 % tests passed, 0 failed out of 1**. Source files
cited `PLAN.md` in comments only — no build step, test or tool opens a planning document — which is
what made leaving them off `main` safe to do at all.

## What this does not fix

- The sanitizer numbers in `PLAN` are still the old ones, now labelled as history. Getting a real run
  means installing a toolchain that has an ASan runtime, then `python tools/sanitize.py`.
- No About panel, so a running app still shows the attribution nowhere on screen.
- Still no hosted CI, so "every gate ran" still means "on this machine, by hand".
