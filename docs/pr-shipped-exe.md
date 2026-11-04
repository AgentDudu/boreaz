# The exe a player can be sent: what it needs beside it, and what it says when that is missing

## The ask

"Is there a way for our app to be an exe?" It already was one — so the question that mattered was
what the exe needs *next to it*, and what happens on a machine where that thing is not where the
process happened to start. Decided with the person asking: **a zip of `boreaz.exe` plus its files**,
**console stays** (it is where every GUI claim in this project is measured from),
**Windows 10/11 only** (the UCRT and `D3DCOMPILER_47.dll` the import table asks for are already in
`System32` there). Everything else — installer, signing, icon, older Windows — stayed parked.

## What was already true, measured before touching anything

```
$ objdump -p build/boreaz.exe | grep "DLL Name"
d3d11.dll  D3DCOMPILER_47.dll  GDI32  KERNEL32  SHELL32  USER32  WINMM  api-ms-win-crt-*
```

No `libstdc++-6.dll`, no `libwinpthread-1.dll`, and none sitting in `build/`: `CMakeLists.txt` has
linked `-static -static-libgcc` into every executable since the two-MinGW-on-this-PATH problem made
it a load failure at `0xc0000139`. No Visual C++ redistributable is involved. 6 290 036 bytes.

## The hole, measured first

Bare exe in an empty folder, working directory also empty, `%APPDATA%` pointed at a fresh folder:

```text
stderr  boreaz: cannot open the parameter file 'config/params.ini'
exit    1
```

`defaults_path()` only ever asked the **working directory** and the three folders above it. Nothing
asked the folder the program stands in, and a shortcut, an "Open with", or a double-click from a
file manager sets that to somewhere else entirely. Copy `config/params.ini` beside the exe and the
same start works — which is why the hole went unnoticed: every run in every phase so far has been
from a folder inside the repository.

## What changed

| Piece | Where | Why |
|---|---|---|
| `config::executable_folder()` | `src/config/params_file.cpp` | `GetModuleFileNameW` (wide: a release can live under a name the active code page cannot spell), growing buffer, empty when the API refuses. The folder that travels with the exe |
| `defaults_path()` asks it, last | same | The repository walk stays first so a build tree keeps answering with the repository's own defaults; the exe's folder is what makes a release started from anywhere answer with its own |
| …and says the absolute name when the file is missing | same | The failure line is the only thing a player gets. `'config/params.ini'` points at a folder they were never in; the name now printed is the folder to put the file in |
| `patches_dir()` asks it too | same | A zip that ships factory patches beside `boreaz.exe` lists them. `user_patches_dir()` (`%APPDATA%\Boreaz\patches`) is untouched, so the shipped folder is still never written to |
| Version resource | `cmake/version.rc.in` → `configure_file` | File properties used to say nothing. Now: `FileVersion 0.1.0`, `ProductName boreaz`, `FileDescription`, `OriginalFilename boreaz.exe`, `LegalCopyright MIT` — generated from the same `project(boreaz VERSION …)` that the check at the top of `CMakeLists.txt` holds `include/boreaz/version.h` to, so the number cannot be said two different ways |
| `--version` | `src/app/main.cpp` | `boreaz 0.1.0`, one line, nothing else started — no device, no window, no settings read. The file properties answer the same question, but not from a script |
| `-s` on the Release app only | `CMakeLists.txt` | 6 290 036 → **3 972 096 bytes**. What comes off is the symbol table; the tests and the tools keep theirs, because that is where a stack trace is asked for |

## Measured

| Claim | How | Result |
|---|---|---|
| A release layout, started from a folder with no repository in it, comes up | `%TEMP%\boreaz-release\{boreaz.exe, config\params.ini, patches\}`, working directory `%USERPROFILE%\Documents`, fresh `%APPDATA%` | `boreaz: Speakers (Realtek High Definition Audio), 48000 Hz, 512 frame period, window. Ctrl+C stops.` — the run that used to exit 1 |
| Each of the three paths resolves beside the exe, not beside the shortcut | `build/_scratch/dirprobe.cpp` linked against `boreaz_config`, run from `Documents` | `exe folder: …\boreaz-release`, `defaults: …\boreaz-release\config\params.ini`, `patches: …\boreaz-release\patches` |
| A release with its `config` renamed away says where to put it | same, with `config` moved aside | exit 1 and `boreaz: cannot open the parameter file '%TEMP%\boreaz-release\config\params.ini'` — the absolute path in the release folder, not the relative name a player cannot act on |
| The file properties name the build | `(Get-Item build\boreaz.exe).VersionInfo` | `FileVersion 0.1.0` · `ProductVersion 0.1.0` · `ProductName boreaz` · `OriginalFilename boreaz.exe` · `LegalCopyright MIT` |
| `--version` answers and stops | `./build/boreaz.exe --version` | `boreaz 0.1.0`, exit 0 |
| Stripping did not break the image | `--version` and `--list-devices` on the `-s` build | version line, then the real device list (Realtek, VB-Audio cable, the default marked) |
| The shipped layout is a rule, not a habit | new case in `tests/test_app_settings.cpp`: writes `config/params.ini` beside the *test binary*, moves the working directory to the temp folder, reads it back | the path found is the one beside the exe, absolute, and the value that loads is the one that file says (`master_gain_db = -3`, where the built-in default is `-12`) |
| Nothing else moved | full suite | **326 cases, 8 877 498 assertions, 0 failed** (was 325 / 8 877 474) |

The shipped zip, then: `boreaz.exe` 3 972 096 B, `config/params.ini` 2 795 B, `patches/` 36 KB —
about 4 MB, one double-click, no installer, no administrator, nothing written outside
`%APPDATA%\Boreaz`.

## Not proven here

- **A machine that has never had MSYS on it.** This box has one Windows install and a MinGW in its
  `PATH`, so "no redistributable needed" rests on the import table above, which names only OS
  DLLs — strong evidence, not the same as a clean VM. Windows 10/11 only, as decided: the UCRT
  API-set DLLs and `D3DCOMPILER_47.dll` are part of those systems; Windows 8.1 wants KB2999226 and
  Windows 7 wants the old DirectX redistributable, and neither has been tried.
- **SmartScreen and antivirus.** The exe is unsigned, so a download from GitHub will say "unknown
  publisher". Not tested, and signing was left out of scope by choice rather than by accident.
- **The patch browser looking at the shipped `patches/`.** `patches_dir()` resolving beside the exe
  is measured by the probe; the browser itself has never been clicked, in this or any earlier phase.
- **A stack trace on the shipped exe.** `-s` takes the function names with the symbol table. A Release
  crash report will have addresses; run the Debug build, or the tests, if names are what is needed.
- No CI job produces the artifact. `CI release` stays where PLAN.md parked it, in the list of things
  to do when asked.
