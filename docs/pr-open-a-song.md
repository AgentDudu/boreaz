# feat: a song chosen while the app runs, and the open dialog is deliberately not one

Closes the last item phase 10 named: `pause, loop, seek and an open dialog`.

## What changed

The window can now replace the song without restarting the app, by two routes:

- **Drop the file on the window.** `glfwSetDropCallback` copies the first name into one slot in `Impl`
  (GLFW's C strings die when the callback returns) and the *drawing frame* is what acts on it — the
  callback has no app to hand the name to, and a callback that opened files itself would be file I/O in
  the wrong place with the wrong lifetime.
- **Type a path.** `F1` or the `Change` button on the transport row opens a text field, prefilled with
  what is loaded, that takes the keyboard in the frame it opens. `Enter` loads what it says.

The line above the panel now says which file is loaded (`song: <path>`, or
`song: none - drop a .mid file on this window`), and a file that will not load says why in red beside
the button.

Reading the file stays the app's job. `Options::Songs::load(path, reason)` is one call, filled in by
`main.cpp`, running on the thread that pumps the window:

```cpp
LoadResult loaded = boreaz::midifile::load_file(path, rate);
if (!loaded.ok) { reason = loaded.error; return false; }   // nothing else has moved
transport.stop(engine);
song = std::move(loaded.song);
transport.load(song);
transport.play(engine);
```

So **a path that will not read leaves the session exactly as it was** — the file is parsed into a
temporary before anything that is sounding gets touched. `song_path` in `main.cpp` is now the single
name for "which file is loaded", so the startup print, the runtime print and the `finished` line cannot
report three different songs; the window is told the name with `set_song_path`, sitting beside the
existing `set_external_playback`.

`tools/key-in-window.ps1` gained `-text` (one `WM_CHAR` per character, which is the path a character
takes into an ImGui field) and **lost `-ctrl`**, for a reason measured below.

## Why there is no `GetOpenFileName`

Its modal loop runs on the thread that pumps this window, and that thread is the one feeding the event
ring. A file picker left open longer than the lookahead (half a second) lets the device run past the
samples it was handed and stutters the song — the app's one real-time promise, broken by a dialog. A
path field and a drop cost no thread at all. It is also the form this app already speaks: `--params`,
`--instrument` and the settings file are all paths as text, and a typed path can be pasted, which a
dialog makes awkward and a script cannot do at all.

## Why F1 and not Ctrl+O

Measured: `PostMessage` of left-Ctrl followed by `O` delivers the `O` stroke with `mods = 0`. GLFW takes
its modifier state from the operating system, not from the message queue, so a modifier cannot be
injected into a window that is not the foreground one — and in this app the foreground window cannot be
chosen from a script. Rather than ship a shortcut that cannot be tested, the shortcut is a bare key.
That is consistent with the two the app already has: `Enter` and `Backspace`. **Every shortcut here is
an unmodified key, because every letter is a pitch** (`m` is A, `o` is G), and an F-key costs no note.

## Measured

Live window, live device, `tools/key-in-window.ps1`:

- An app started with **no file at all**: `F1`, the path typed one character at a time, `Enter` →
  `boreaz: "…pad-chords.mid": 48 events, 6.5 seconds.` and then `boreaz: "…pad-chords.mid" finished.`
  Its two loader warnings came out too (ignored program changes, ignored sysex), so the runtime route
  reports exactly what the startup route reports.
- A **second** song, loaded after the first had run out: `lead-simple.mid` played and printed
  `finished`, then `F1` + path + `Enter` printed `"…pad-chords.mid": 48 events, 6.5 seconds.` and later
  that file's own `finished` — 0 dropped, 0 steals across both. A song adopted into a stopped transport
  has to start its clock on the file rather than on its first sounding voice (`external_playback` goes
  back to true), and the dropped-event count is reset with the song it counts.
- An app started **with** `lead-simple.mid`, then handed `C:/songs/does-not-exist.mid`:
  `boreaz: cannot open 'C:/songs/does-not-exist.mid' for reading. Is it a file, and is it there?` — the
  original song's `finished` line had already printed, nothing was swapped, and the run carried on.
- 295 cases / 8 861 666 assertions, unchanged from the previous slice; `tools/release_check.py` green;
  every golden unmoved.

## Not measured

- **The drop itself.** `glfwSetDropCallback` is wired and the frame answers it through the same
  `ask_for_song` the typed path uses, but an OLE drag out of Explorer cannot be scripted. Same status as
  the mouse from the previous slice: nothing in this phase has been clicked, including `Change` and
  `Load`.
- Whether the field is *comfortable* — width, where it sits when the window is narrow. It is usable and
  keyboard-reachable; that is not the same as tuned.
