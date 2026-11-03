# A cable is chosen from a list, not typed as a number

## What landed

Phase 8.4: the panel's half of the modulation router. 8.1 shipped the matrix as 32 sliders, so wiring a
patch meant knowing that `mod1_source = 2` is LFO 1 — the `.ini` was easy and the window was not.

Four things, all in the panel and the table, none in the engine:

- **The word lists live beside the enums** (`mod_router.h`): `kModSourceNames`, `kModDestNames`, each
  array sized by its own `kCount`, so an enumerator added without a word stops compiling rather than
  printing `nullptr` in a combo.
- **`param_choices(array_kind, &count)`** in the parameter table hands them to the panel. Asked by array
  kind rather than stored per row, because 32 rows would otherwise be 32 copies of the same two lists.
- **The two ends of every slot are combos.** A unipolar flag was a 0–1 slider and is a checkbox: any
  integer row whose range is exactly 0..1 is drawn as a switch, so the next boolean gets one for free.
- **A find box** filters on key *or* label and prints `N of 63`. 63 rows in a 340 px column is not a
  table a hand can search, and a filter that hides rows has to say how many it hid — otherwise a
  parameter the player cannot find looks like a parameter that does not exist.

The row labels drop three abbreviations (`M1 source`, `M1 destination`, `M1 unipolar`), and the
modulation envelope reads **"Mod env attack"** beside the cable list that says "Mod env", while the amp
envelope keeps the bare **"Attack"** a player already reads.

270 tests (1 new case, 72 assertions), the flagship golden unmoved (`lead-simple.mid` still
`peak=0.306908`), `release_check` green with every gate running, including the app on a real audio device.

## What the measurement says

Nothing to measure: no engine file changed, so the sound cannot have moved, and the golden says so.

| | |
|---|---|
| `lead-simple.mid` golden peak, before / after | **0.306908 / 0.306908** |
| rows the checkbox rule catches today | the 8 `mod*_unipolar` rows, and nothing else (`Transpose` is −24..24, `Octave` −4..4, `Bend range` is a float) |
| cable rows the panel now draws as a list | **16** (2 per slot × 8), asserted against `2 * kModSlots` |

## The naming decision, settled rather than worked around

**"Mod env" is a label. The keys stay `env2_*`.**

Inside this engine the envelope a player hears as "the second envelope" is the amp one, so a window that
puts `env2_attack_s` ("Env2 attack") under `Attack` is a defect the panel would otherwise ship — and the
panel is where the name becomes the player's problem. Only the label moves, for one reason: renaming the
keys was free today and it is not free after phase 9 puts them into saved patch files, where a rename
stops being a string and becomes a compatibility question. `env1_*` still has no row at all, which is the
same ambiguity nobody has needed to resolve yet. PLAN §7 now carries the decision so it does not get
re-litigated in the panel again.

## What surprised me during implementation

**Four of the six destinations are named for the row they move, and that is a test, not a style choice.**
"Cutoff", "Osc 2", "Osc 3" and "Hammer noise" are the labels of `filter_cutoff_ratio`, `osc2_level`,
`osc3_level` and `noise_level`, and the new case asserts the equality — so a list that drifts from the
slider it moves fails a build instead of becoming a third vocabulary. Pitch and pan have no row to match
(a note's pitch and its position are not parameters), so they are named for the thing itself.

**Every name is pinned to its enumerator rather than to its position.** Reordering `ModDest` is a thing
somebody adding a destination will do, and a panel that then calls velocity "Wheel" is a defect no engine
test would ever notice.

## What it breaks / what has to change later

- **Nothing at the file level.** Keys, ranges, defaults and the `.ini` format are untouched, so every
  patch file written since 8.1 still loads, including the four `.ini` files the router PR documented.
- The combo writes through the same `param_write` as a file does, so a list and a patch file cannot
  disagree about what a legal value is.
- The panel is still **one list of 63 rows**. The eight slots would read better as a grouped section, and
  that belongs with phase 11's switchable panel rather than here.
- The find box is a plain ASCII-folded substring filter, not fuzzy, and it filters labels only.

## One thing to know before reviewing the next file you touch

The panel's tests drive Dear ImGui with no renderer and inspect **nothing about the widgets** — they
prove the queue and the instrument rules. So the combo, the checkbox and the filter all *execute* under
the suite (all 63 rows are drawn every frame, and a bad branch would crash it), but nothing in the suite
knows whether a cable row was drawn as a combo or a slider. What the new case pins is the data under the
widget: the lists, the names, the defaults, and the clamp. Widget type is asserted by a human looking at
the window, and I have not looked at it yet.

## Review focus

1. **`param_choices(array_kind, &count)`** (`param_table.h` / `param_table.cpp`) — is asking by array
   kind the right shape, or should the list be a field on the row? The argument for asking: 32 rows
   sharing two lists. The argument against: the panel now has a third way to ask what a row is
   (`param_is_integer`, `param_choices`, and the range itself).
2. **The 0..1 checkbox rule** in `params_panel.cpp`. It is deliberately a rule about ranges rather than a
   per-row flag, which means a future integer row that happens to span 0..1 becomes a checkbox by
   accident. Grep says nothing does today; say so if you would rather the row said.
3. **The clamp assertion in the new test** — `mod1_source = 40` from a patch file has to land inside the
   list before the panel can hand ImGui a word it has not got. Asserted against the list length rather
   than the row's range, so the two cannot drift apart silently.
4. The labels themselves ("Mod env", "M1 destination") — they ship in the window and nowhere else.

## What is measured rather than asserted

- **The golden, measured:** `peak=0.306908` before and after, so the claim "nothing in the engine moved"
  is not taken on trust.
- **The clamp, asserted:** −1, 6, 40 and 1.5e9 through `param_write` all land inside `0..kCount-1`.
- **The names, asserted** against both their enumerators and the labels of the rows they move.
- **Not asserted:** that ImGui draws a combo rather than a slider, the tooltip text, or the find box's
  `N of 63`. Those are the window, and the window has not been looked at.
