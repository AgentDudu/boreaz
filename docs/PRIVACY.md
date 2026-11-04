# Privacy

Boreaz collects nothing.

- **No network I/O.** The application opens no sockets and makes no requests. Dependencies
  are fetched at build time, never at run time.
- **No telemetry, analytics, crash reporting, or usage tracking.** There is no code path that
  sends anything anywhere.
- **No accounts, no sign-in, no identifiers.**
- **One deliberate download, made by a human running a command.** `tools/mkinstrument.py` fetches
  the CC0 instrument from its own release page, prints the URL it is fetching, and refuses the file
  if its sha256 is not the one written in the tool. It writes to `third_party/`, which is ignored, and
  the application never does any of this: it reads a directory of WAVs and could not fetch one if it
  wanted to.

## What is stored, and where

- **What the machine was set up with** — the audio output port, the MIDI input port, the sample
  library, and the patch that was open when the window last closed — is one file,
  `%APPDATA%\Boreaz\settings.txt`, read at every start and rewritten at the end of a session that
  had a window. Four names, no parameters, no audio and no playing history. It exists so the app
  reopens the devices you picked instead of the first ones in the list. It is local, it is yours,
  and deleting it only costs you your preferences.
- **Patch files you save** go to `%APPDATA%\Boreaz\patches\`, one `key = value` text file per
  patch, named by you. The `patches/` folder inside the repository holds the factory voices and is
  never written to, so a save cannot overwrite a sound that shipped.
- **A rendered file** only exists where you tell the renderer to write one. Nothing is recorded
  from a live session.
- **`.env`** holds environment overrides in the project directory. It is git-ignored and is
  never read by anything except the local process. `.env.example` is committed with empty
  values and contains no machine data.

## What we deliberately do not do

- No audio, MIDI input, or performance data leaves the process. MIDI and audio are never
  logged, buffered to disk, or uploaded.
- No absolute machine paths, device serials, or user names are committed to the repository.
  A test in `tests/test_hygiene.cpp` fails the build if a tracked file starts to contain a
  machine-absolute path or a config value with real data in it.

If any of the above changes, this file changes first, in the same pull request.
