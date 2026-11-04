# key-in-window.ps1 — a keystroke into a window nobody can focus

`SendKeys` and `keybd_event` both need the target window to be the **foreground** window, and the
desktop refuses `SetForegroundWindow` from another process. Both report success anyway, which is how a
GUI test ends up proving nothing. `PostMessage` needs no focus, and GLFW's window procedure answers it.

    powershell -NoProfile -File tools/key-in-window.ps1 -vk 13     # Enter into boreaz
    powershell -NoProfile -File tools/key-in-window.ps1 -vk 112    # F1, the song path field
    powershell -NoProfile -File tools/key-in-window.ps1 -text "C:/songs/lead-simple.mid"

It takes the running process's `MainWindowHandle`, translates the virtual key to a scancode and posts
`WM_KEYDOWN` then `WM_KEYUP`. GLFW codes and Windows virtual keys are not the same number space, so
check `GLFW_KEY_*` before reading a test result: Enter is `13` here and arrives as `257` there.

What the app does in answer has to be observable on its own — boreaz prints `finished` when a loaded
file runs out, and that single line is enough to prove play, pause and resume from a log.

`-text` posts `WM_CHAR` instead of a key, because that is the path a character takes to an ImGui text
field: GLFW hands characters to ImGui itself, and they never reach the app's note queue.

A **modifier cannot be posted this way.** GLFW takes its modifier state from the operating system, not, so a posted `VK_CONTROL` arrives with `mods = 0` and the app cannot tell the
key was typed with it held. Measured, not assumed: posting left-Ctrl then `O` gave the `O` stroke with
`mods=0`. That is why every shortcut in this app is a bare key — Enter, Backspace, F1.
