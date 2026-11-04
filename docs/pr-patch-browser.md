feat: the mixer saves and loads patches, and a bad file is refused rather than obeyed

The writer landed in #38 with nothing to call it. This is the part you can click:
a `Patch` block at the bottom of the mixer — a name field, `Save`, and the
patches folder listed as one button per patch, labelled with the name from its own
header. Hovering a row shows the file behind it and the sampled instrument it
wants, so the list and the text inside the file are visibly the same thing.

**The browser is a directory listing.** `patches_dir()` finds `patches/` by the
same walk up the tree `defaults_path()` uses for `config/params.ini`, because the
two ways of running a build are from the repository root and from `build/`. There
is no database of presets and no index: the folder *is* the browser, and a file
dropped into it from Explorer shows up on `Rescan`.

**Saving writes the panel and the library together.** The instrument path in the
field above is written into the header as the patch's instrument, so one file
carries both halves of a preset — which is what phase 9's "round-trips including
instrument" asks for. Loading does the same in reverse through the one path the
app already had for adopting a library: the parameters go in via `set_params`
(the engine's ramp is what makes a patch change without a click) and the
instrument follows. The Load button and a patch load are now the same four lines
rather than two copies of them.

**A patch that names no instrument keeps the library that is playing** rather
than dropping to the synthesiser, and a patch whose library is missing says so in
the panel's own line. Going quietly to the synth would be the failure phase 4 made
a rule against: a preset that changes your instrument without telling you.

**A refused file changes nothing, including the sound.** That is the decision
from this morning, and it reverses PLAN's exit criterion, which asked for a
partial load with a warning: the parser has always taken a file as a unit or not
at all, and a half-loaded preset is some of one sound and some of another — a
sound nobody wrote and nobody can name. So `adopt_patch` loads into a copy of
what is playing and assigns it only on success. The library-level half of that is
now asserted: after a refused file, the `Params` that was handed to the loader is
still what it was.

A patch name goes through a small check because it becomes a file name: no dot,
slash or colon. A name is text typed into a box, and `..` is a save that lands
outside the patches folder.

272 cases / 8 860 425 assertions; the two panel cases call `toggle()`, so the
Patch block, the scan and the buttons all execute under the suite — but the scan
finds nothing there because there is no `patches/` in the tree yet. No click is
simulated: this repo's ImGui tests draw the panel, they do not press its buttons.
`release_check` green end to end, goldens unmoved.

Not done: the factory patches (next, written as files and checked by render), the
device config file, and autosave on exit.
