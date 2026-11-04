docs: privacy names the file and the four keys the app writes

PR #41 started writing `%APPDATA%\Boreaz\settings.txt` and `docs/PRIVACY.md` did not follow, even
though its own last line says it changes in the same pull request as what is stored.

Two things were wrong in it:

- it named none of the four things the file holds, and never mentioned a **sample library** at all;
- it claimed a stored **window size**. Nothing stores window geometry: `width` and `height` are
  compile-time defaults in `AppOptions` (`include/boreaz/gui/app_window.h`) and are not written to
  any file. That claim predates #41 and nothing had disproved it.

Now it says: one file, four names (audio output port, MIDI input port, sample library, the patch
that was open), read at every start, rewritten at the end of a session that had a window, no
parameters, no audio, no playing history. It also names a second thing the app writes that the file
did not mention — patch files you save, which go to `%APPDATA%\Boreaz\patches\` and never into the
shipped `patches/`.

Docs only. `release_check` green end to end, which matters here because the hygiene gate is the one
that cares about this file: it fails a tracked file that starts carrying a machine-absolute path or
a real config value.
