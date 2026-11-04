# Boreaz

A piano for Windows: **88 keys, A0 to C8** — the whole compass of a real one. Play it with the letter
row of your typing keyboard, a real MIDI keyboard, or a `.mid` file. The sound is made by the program
itself, or it can play a recorded concert piano instead. A window shows the keys, the notes moving,
and a slider for every sound control.

No installer, no account, nothing uploaded. Free for non-commercial use.

## Build

Needs Windows 10 or 11, CMake 3.25 or newer, Ninja, and GCC or Clang. Dependencies are fetched at
pinned versions on the first configure, so that first configure wants internet.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Run

Sound comes out of the default output with nothing configured.

```bash
build/boreaz --gui                     # the window: click the keys, or type z s x d c v
build/boreaz                           # no window, wait for a MIDI keyboard
build/boreaz song.mid                  # play a file, and stop when it ends
build/boreaz-render song.mid out.wav   # make a WAV from a file, no sound card needed
build/boreaz --list-devices            # what outputs this machine has
build/boreaz --help                    # every option, and every sound control
```

## License

Boreaz's own source is under the **PolyForm Noncommercial License 1.0.0**: free to use, study, change
and share for non-commercial purposes; selling it or using it commercially needs a separate agreement
with the author. See `LICENSE`.

The sampled pianos and any bundled third-party code keep their own terms, listed in `NOTICE`, and
neither is covered by the non-commercial clause.
