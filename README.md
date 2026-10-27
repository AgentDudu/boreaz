# Boreaz

A lightweight audio/MIDI synthesizer for Windows: **88 keys, A0 to C8** — the full compass
of a real piano — with an on-screen keyboard, a switchable visualiser (transport strip or
note waterfall), `.mid` playback, and a sampled concert grand.

C++20, no DAW required. Runs standalone, takes a real MIDI keyboard, and can also render a
MIDI file to WAV from the command line.

## Status

Early. The repo currently holds the development plan and the build scaffold, so there is no
sound yet. `PLAN.md` is the contract for what is being built and in which order; phases 0-12
are listed there with exit criteria.

```
boreaz --version      # works today
```

## Requirements

- Windows 10 or 11
- CMake 3.25 or newer, and GCC or Clang (this project is built with `ninja`)
- Internet access on first configure: dependencies are fetched at pinned versions

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/boreaz --version
```

Sound, device selection and MIDI input arrive with the later phases; until then
`--list` and audio flags are not implemented.

## Configuration

Nothing is hardcoded and nothing is uploaded. Machine-specific values come from the
environment, optionally backed by a local `.env` file:

```bash
cp .env.example .env     # .env is git-ignored; every key in the example is empty
```

See `docs/PRIVACY.md`.

## License

Boreaz's own source is licensed under the **PolyForm Noncommercial License 1.0.0** — free to
use, study, change and share for non-commercial purposes; selling it or using it commercially
needs a separate agreement with the author. See `LICENSE`.

Bundled third-party code and the sampled piano keep their own terms, listed in `NOTICE`.
The piano (Salamander Grand Piano, CC-BY-3.0) is *not* covered by the non-commercial clause:
it stays freely reusable under CC-BY.
