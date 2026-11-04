# feat: the controls a hand uses, in the window shell and not in the mixer

## What changed

`AppWindow::draw` now puts a row of the player's own controls between the view switcher and the panel:
**Play/Pause** (one button, one label), **Stop**, a **Loop** checkbox, and a position bar that reads
`m:ss / m:ss` and **seeks to wherever it is clicked**. The row is drawn whenever the app has a song and
independently of which panel is open, so the waterfall and the spectrum of phases 11 and 12 cannot hide
it. It is *not* drawn when there is no song, because a play button with nothing to play is a lie.

The window owns no song and no clock. `Options::Transport` is nine calls — `active`, `playing`,
`looped`, `position`, `end`, `toggle`, `stop`, `toggle_loop`, `seek(fraction)` — that `main.cpp` fills
in with the app's `Transport` and `Engine`, and every one runs on the thread that pumps the window,
which is the thread the pump loop is already on. So none of them is a lock, and none reaches the audio
thread except through `Engine::push_event` / the engine's own flush request.

`boreaz::midifile::Transport` grew the answers, so the arithmetic of "what does this control do" lives
in one place and not in a widget:

- `toggle(engine)` — running becomes pause; pause or finished becomes running; finished *at the end*
  means from the top. The button and `Enter` both call this and nothing decides twice.
- `seek_fraction(engine, 0.0..1.0)` — clamped at both ends, because a pointer that slid off the bar is
  a normal event and not an error to report.
- `end_sample()` — so a panel can print the song's length without reaching into `Song`.

`Enter` and numpad `Enter` are the shortcut for `toggle`. Not Space (Space is the sustain pedal) and not
a letter (`m` is A, `r` is F#, so any letter shortcut would sound as well as command). Backspace keeps
what phase 5 gave it: the **strip's** clock, which is a different clock from the song's playhead.

## Why

Phase 10 named pause, loop and seek, and a player who cannot see where they are in a song cannot seek
mentally either. The row went into the window shell rather than the mixer because the mixer is opened to
build a sound and closed to play one — a play button behind a window is a play button that does not get
pressed. And it went above the panel because the strip is the one thing on screen that has to survive
every panel change phase 11 and 12 make.

The bar is **click-to-seek and not a drag**. A widget that follows the playhead and is also the thing
being dragged fights the player every frame: the frame writes the position the drag is trying to set.
One click, one seek, is the smallest thing that answers "jump to that part", and it cannot disagree
with itself.

## Bug this found

**Typing into a text field played a chord.** Every letter of a patch name typed into the library field
or the Save-as field is a note on the computer keyboard, so `piano` was a five-note cluster on top of
the word. The guard was already there in shape but asked the wrong question: `ImGui::GetIO()
.WantCaptureKeyboard` also goes true for reasons unrelated to any field (Dear ImGui's keyboard
navigation, `ConfigNavCaptureKeyboard`), which in this app would have muted the piano. It asks
`WantTextInput` now — the one flag that means a field is being edited. Strokes are still *drained*
while a field has the keyboard, so closing the field does not release a queue of held letters as a
chord into the next frame.

Also removed, not added: `Transport` had both `looped()` and `looping()`. `looping()` stays.

## Evidence

**A live window, a live device, a real file.** A window nobody can watch is still observable, because
`main.cpp` prints `finished` when the file runs out. Keys are delivered with
`PostMessage(hwnd, WM_KEYDOWN|WM_KEYUP, VK_RETURN, ...)` on the process's `MainWindowHandle`;
`SetForegroundWindow` + `keybd_event` and `WScript.AppActivate` + `SendKeys` both *reported success and
delivered nothing*, because the desktop refuses foreground from another process — three earlier runs of
"the shortcut does not work" were three runs of testing nothing.

| experiment | what was posted | what the log shows | reading |
|---|---|---|---|
| restart | one Enter, after a 2.958 s file had already ended | `finished` once before the key, **twice** after | the key put the song back at the top and it ran to the end again |
| pause | one Enter at 1.2 s of that same file | **no `finished` at 5.7 s** | impossible unless the pause took: an unpaused 2.96 s file ends by ~3 s |
| resume | a second Enter | `finished` **once** | the song carried on from where it stopped and ended |

That is the keyboard route proven end to end: GLFW → the window's stroke queue → `toggle` → the
transport → the audio thread. Proof the keystroke arrived at all was a temporary `fprintf` in the
stroke loop printing `key=257 action=1` (`GLFW_KEY_ENTER`), removed before merge.

**Tests.** 295 cases, 8 861 666 assertions, 0 failed. `tests/test_transport.cpp` is eleven cases: the
two new ones are *"one button, and what one press means is decided in one place"* (the play/pause/
finished-at-the-end table) and *"a slider dragged past either end still lands inside the song"*.
`tests/test_gui.cpp` adds *"Enter is the transport, and a key that commands cannot also be a note"* —
Enter, keypad Enter, backspace and the arrows map to no note, so a command key cannot sound.

**Audio path unmoved.** Headless with a song, same day as the routing measurements: 278 callbacks,
0 restarts, 0 dropped, 0 steals, **1.16 %** audio-thread CPU — inside the band the transport already
measured (0.82 %-1.25 %), and the `late` count on this box drifts by tens of percent between hours, so
only the A/B already recorded in the phase row is read as a comparison. `tools/release_check.py`: every
gate green, every golden byte-identical.

## Not proven by this PR

- **The mouse.** Nobody has clicked Play, the Loop checkbox, or the bar. The click handler is five lines
  over `GetItemRectMin/Max` and `GetMousePos`, and no test in this repository can generate a pointer
  inside a window.
- The row on a machine with **no audio device at all** — phase 7's open criterion, unchanged here.
- Loop *auditioned*: the checkbox flips and the transport wraps (measured in phase 10's transport cases),
  but nobody has sat and listened to a looped song repeat from the GUI.
- The **open-a-file dialog**, which is the last item phase 10 named and is not in this branch.

## How to review

1. `include/boreaz/midifile/transport.h` + `src/midifile/transport.cpp` — `toggle`, `seek_fraction`,
   `end_sample`. Read the two new cases in `tests/test_transport.cpp` beside them.
2. `include/boreaz/gui/app_window.h` — `Options::Transport`, and the comment about which thread the
   calls run on. Nothing in the header owns state.
3. `src/gui/app_window.cpp` — the row block, then the `WantTextInput` guard, then the `Enter` branch.
   The block is the only place in the GUI that writes to the app rather than reading from it, and it
   writes by calling the same functions the keyboard calls.
4. `src/app/main.cpp` — nine lambdas. Check that each captures by reference something that outlives the
   window, and that none of them touches the audio thread directly.
