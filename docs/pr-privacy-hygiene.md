# What the markdown publishes about the machine it was written on

## The ask

"Read the markdown files whether they have sensitive information/data in it." All 32 tracked
`.md`, plus the non-markdown tracked text for context, plus history. The repository is
**public** (`AgentDudu/boreaz`), 143 commits, so anything here is published, not private.

## Found

| What | Where | Stance |
|---|---|---|
| **The Windows account name** — the same name `NOTICE` carries in its copyright line | `docs/pr-folder-search.md:32` (six filter needles in a measurement table), `tools/walk-to-song.ps1` (the same list, twice: comment and code) | **A real leak, and it contradicts this repository's own promise**: `docs/PRIVACY.md` says *"No absolute machine paths, device serials, or user names are committed to the repository."* It slipped through because the gate only looks for a **whole** machine path (`[A-Za-z]:[\\/][Uu]sers[\\/]`, `/home/…`) and a credential; a name typed as a folder hop is neither |
| **Two third-party e-mail addresses** — the FreePats upright's recorders | `PLAN.md` CC0 credit line | Third-party PII in an attribution line. Upstream publishes them, but nothing about the credit needs an address |
| **A full home path in history** — a `cd` into `<home>/VSC_Projects/boreaz` | `PLAN.md:366` at `b7bddcd` (the first commit), still reachable in old blobs; gone from `HEAD` | Left as it is; see the limits |
| Hardware and installed-software fingerprint — `Realtek High Definition Audio`, `VB-Audio Virtual Cable` / `CABLE In 16 Ch` | 11 places | **Kept, deliberately.** Those names are what make "0 dropped of 1624 callbacks at 512" mean something: they tie every latency claim to a driver a reader can have or distrust |
| Illustrative paths — `D:\instruments\grand\library.txt`, `C:/songs/*.mid` | HANDOFF-phase9, pr-app-settings, pr-open-a-song, tools/key-in-window.md | Never real; they are the loader's example input |
| `AgentDudu` | HANDOFF.md | The public owner of the public repository |

## Checked and clean

Machine hostname (`DESKTOP-F2345QN`) · tokens and secrets (`ghp_`, `github_pat_`, `ghs_`,
`AKIA…`, `sk-`, `xox*`, `api_key=`/`secret`/`password` carrying a value) · MAC addresses · IPv4 ·
UNC `\\server\share` · `/home/…` · `.env` (ignored; `.env.example` values are empty and a test
enforces it) · MIDI port names · any real path to the song library · anything inside
`%APPDATA%\Boreaz\settings.txt`.

## What changed

| Piece | Why |
|---|---|
| The gate now looks for **the account name of the machine it runs on** — case-folded substring, `USERNAME`/`USER`/`LOGNAME`, skipped under three characters, and skipped for service logins (`runner`, `ContainerAdministrator`, `root`, `git`, …) because a CI login is not identity | This is the shape the leak actually had. A path regex cannot see a name used as a filter needle |
| `NOTICE` is exempt from that rule | The licence line states the copyright holder's own name on purpose. `test_hygiene.cpp` stays exempt too — it contains the patterns by necessity |
| `tools/walk-to-song.ps1` builds its hop list from `$env:USERNAME`, and throws if the variable is absent | The measurement stays re-runnable on any machine and the script stops carrying anyone's login in its source |
| `docs/pr-folder-search.md` names the hop as "the account folder" | The claim it measures does not need the name; the tool comment says where the name comes from |
| `PLAN.md` keeps the recorders' names and the two source links, drops the addresses | Credit stays verifiable (`github.com/freepats/upright-piano-KW`, `freepats.zenvoid.org`) without publishing scrapeable addresses |
| `docs/PRIVACY.md` now lists what the gate really enforces — machine-absolute path, config value with real data, credential or token, **or this machine's account name** — and states the one exception: the author's own name in `NOTICE` | The sentence was already written before the rule existed. Now it is true |

## Measured

| Claim | How | Result |
|---|---|---|
| The rule can fire | a probe file at the repository root holding one sentence with the bare account name and no path around it | `ctest` fails: `_hygiene_probe.md: this machine's own account name -> "…"` |
| The tree is clean under it | probe deleted, same command | **100 % tests passed, 1/1** |
| The new rule's own shape | new case in `test_hygiene.cpp`: a person's login is not generic, four service logins are; a body containing the account name offends in either case (folded), a body naming a different account does not, prose about `runner` does not | 327 cases, **8 877 508 assertions**, 0 failed (was 326 / 8 877 498) |
| The scrub did not break the measurement it documented | `tools/walk-to-song.ps1` re-run end to end | six hops (`Users` → the account → … → `.mid`), `"C:/…\build\drive-ab.mid": 176 events, 17.75 seconds`, **0 dropped, 0 restarts** |
| This very note was the rule's first offender | the first draft of this file named the account and gave a `cd` under the home folder, in prose about not naming them | `ctest` failed with both findings against `docs/pr-privacy-hygiene.md` — "machine-absolute path (Windows)" and "this machine's own account name". Scrubbed, and the suite is green again. A rule that only catches other people's mistakes is not a rule |
| Nothing else moved | full suite + build | 0 build errors, suite green |

## Limits, said plainly

- **History is untouched.** `b7bddcd` still holds that `cd` line and the folder-search blob still
  holds the bare name; 143 commits and any fork or GitHub cached view keep them.
  Rewriting them would break every branch and PR reference for no real gain, because the same name
  is published on purpose one line away — the first line of `NOTICE` is the copyright holder's own
  name. The account name is not the secret here; if it were, the licence would have to change too.
- **The rule is machine-relative.** It flags the login of whoever runs the check, so it cannot catch
  a *different* person's name in a file added on someone else's machine, and it deliberately does
  not look for names under three characters, where a name matches ordinary prose.
- The walk tool still **prints** the account name on stdout while it runs (it is typing it into the
  app's filter). That output goes to the console and to untracked scratch logs, not into the tree.
- The decision to keep the device names is a decision, not a fact: it publishes what hardware and
  what virtual cable are installed on this machine.

## The code, scanned the same way

The same question asked of the 130 tracked non-markdown files (64 `.cpp`, 47 `.h`, 11 `.txt`, 6
`.py`, 4 `.mid`, 2 `.ps1`, 2 `.ini`, and the dot-files).

| Looked for | Result |
|---|---|
| e-mail addresses, a URL carrying credentials | none |
| credential shapes: `ghp_`/`gho_`/`ghs_`/`github_pat_`, `xox*`, `AKIA…`, `-----BEGIN`, a JWT `eyJ…`, `sk_live`, `api_key`/`token`/`secret`/`password` with a value next to it | none — every hit was `AllPass`/`bypass` in the reverb and `for (int pass = 0…)` |
| product keys, account ids, phone numbers, an SSN shape | none |
| hostname, account name, personal names | none outside `NOTICE` |
| GUID, SID, loopback endpoint, MAC address | none |
| machine-absolute path, `/home/…` | none. The paths in code and tests are invented: `C:/songs`, `D:\instruments\grand upright\library.txt` |
| URLs | public upstream only: the four `FetchContent` pins, the FreePats release, `api.github.com` and `raw.githubusercontent.com` in `tools/mkinst.py` — no auth header, no token, no private host |
| long base64/hex runs | the two `sha256` release pins in `tools/mkinstrument.py`: integrity pins for files anyone can download |
| what is inside the committed `.mid` fixtures | `strings` answers `MThd`, `MTrk`, and the file's own name as the track name. Nothing else is in them |
| `.env.example`, `patches/*.txt`, `config/params.ini` | every `.env` value empty (a test keeps it that way); no path, device name or user name in any shipped patch or preset value |
| whatever the machine writes — `%APPDATA%\Boreaz\settings.txt`, `imgui.ini`, `.env`, `build/`, `third_party/`, rendered WAVs | git-ignored, and none of it is in the tree |

**One thing found, and fixed.** `tests/test_app_settings.cpp` used this machine's **real audio
endpoint** as a payload: `CABLE In 16 Ch (VB-Audio Virtual Cable)` is device 0 of
`boreaz --list-devices` on this box, and `Speakers (Realtek` is the start of the real default. The
rule being broken is PLAN's, in its own words: *"No secret and no machine-specific value is ever
written in code"*, with audio/MIDI device names named as the example. A round-trip test needs the
**shape** — spaces, brackets, backslashes, a value that is everything after the first `=` — and not
the device, so the literals are now `Cable In 16 Ch (Some Virtual Cable)`,
`Auto Input Port (Some Keyboard)` and `Speakers (Some Driver)`, the last being the name
`test_gui.cpp` already used. Unchanged: 327 cases, 8 877 508 assertions, `ctest` 1/1 — the same
numbers as before the literals moved, which is the proof that nothing but the strings changed.

**Left in code on purpose:** the comments in `src/io/audio_device.cpp` and `tools/release_check.py`
that name the Realtek driver while explaining a measurement (*"a 512 frame period is late 0 at 1024
and 256 of 1665 at 512, which is the Realtek, not this code"*). That is the same provenance the
markdown keeps by decision, and removing it would make those measurements unattributable.
