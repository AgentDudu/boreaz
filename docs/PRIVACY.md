# Privacy

Boreaz collects nothing.

- **No network I/O.** The application opens no sockets and makes no requests. Dependencies
  are fetched at build time, never at run time.
- **No telemetry, analytics, crash reporting, or usage tracking.** There is no code path that
  sends anything anywhere.
- **No accounts, no sign-in, no identifiers.**

## What is stored, and where

- **Configuration and device preferences** (chosen audio output, chosen MIDI input, last
  patch, window size) are written to the per-user config directory
  `%APPDATA%\Boreaz\`. This exists so the app can reopen the devices you picked. It is local,
  it is yours, and deleting it only resets your preferences.
- **`.env`** holds environment overrides in the project directory. It is git-ignored and is
  never read by anything except the local process. `.env.example` is committed with empty
  values and contains no machine data.
- **Rendered audio** only exists where you tell the renderer to write it.

## What we deliberately do not do

- No audio, MIDI input, or performance data leaves the process. MIDI and audio are never
  logged, buffered to disk, or uploaded.
- No absolute machine paths, device serials, or user names are committed to the repository.
  A test in `tests/test_hygiene.cpp` fails the build if a tracked file starts to contain a
  machine-absolute path or a config value with real data in it.

If any of the above changes, this file changes first, in the same pull request.
