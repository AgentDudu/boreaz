# refactor: the mod envelope is mod_env_* everywhere, before a file froze env2_*

## Why now and not later

Phase 9 saves patches to a `key = value` file. That is the moment a parameter
name stops being internal: after it, a rename is a compatibility question with
aliases and a migration, and before it a rename is a string. #35 made the panel
say **"Mod env attack"** while the file kept writing `env2_attack_s`, on the
reasoning that the label was free and the key was not. That reasoning was right
about the cost and wrong about the deadline — phase 9 is the next phase, so the
free window is now, and shipping two vocabularies for one envelope into a file
format is how you end up with both forever.

## What moved

| layer | before | after |
|---|---|---|
| the file / the parameter table | `env2_attack_s`, `env2_decay_s`, `env2_sustain`, `env2_release_s` | `mod_env_attack_s`, `mod_env_decay_s`, `mod_env_sustain`, `mod_env_release_s` |
| `Params` fields | `env2_attack_s` … | `mod_env_attack_s` … |
| the source enum | `ModSource::kEnv2` | `ModSource::kModEnv` |
| inside `SynthVoice` | `env2_`, `mod_used_env2_`, `apply_env2_times` | `mod_env_`, `mod_used_env_`, `apply_mod_env_times` |
| the row label | "Mod env attack" | unchanged — it already said this |
| the amp envelope | `attack_s`, `decay_s`, `sustain_level`, `release_s` | unchanged — this is the one a player reads without a qualifier, and `env1_*` has never existed |

Nothing moved in the arithmetic. The rename is why `Mod env` now means one thing
in the cable list, the panel row, the `.ini`, the enum and the voice.

## The break, measured rather than assumed

The parser refuses an unknown key, so an old config does not silently lose its
settings — it stops the app:

```
boreaz: line 1: 'env2_attack_s' is not an engine parameter. Known keys are listed in --help
```

Run against a one-line `.ini` with the old key, and the same file with the new
key loads. `config/params.ini`, the fixtures, the goldens and the tooling never
mentioned the old keys, so the only files that can break are hand-written
configs, and the format that would have made them permanent is the one phase 9
has not written yet.

## Proof this is a rename, not a sound change

- **Both render-contract gates pass against unchanged hashes**: synthesiser
  `08eb2e60c42ca135` / peak 0.142584, sampled instrument
  `8d2611517da3c819` / peak 0.117014. Those are the hashes the sine-table PR
  wrote this morning, so the rename moved no bit of audio.
- 271 cases, 8 859 389 assertions, same counts as before the rename; the tests
  that name the keys (`param_spec("mod_env_attack_s")` and its label) are the
  ones the rename updated, so the new spelling is pinned by an assertion.
- `python tools/release_check.py` — every gate ran, including the app on the
  real audio device and the live parameter bus.

## Two things in the diff that are not the rename

- `synth_voice.h`: clang-format sorted the include block and wrapped
  `noise_level()`. Both were already like that at HEAD — the format gate runs on
  **changed** files, so an untouched file keeps its drift until something else
  touches it. Worth knowing about the gate: it does not keep the tree formatted,
  only the diff.
- `docs/pr-modulation-router.md` and `docs/pr-panel-cables.md` each get a
  **superseded** pointer rather than a rewrite. They are dated records of what was
  decided when they merged; rewriting them would erase the argument that the
  rename is cheaper before phase 9 than after. PLAN §7 carries the reversal.

## Not done

- No alias for the old keys. Deliberate: nothing in the tree writes them, and an
  alias table is a cost paid forever to spare a handful of hand-written configs
  one edit. If you have a `.ini` with `env2_*` in it, delete the line or rename
  it — the error tells you which.
- `env1_*` still does not exist. The amp envelope's rows keep their bare names.
  If a future phase wants a second named envelope, this naming is already the
  pattern for it.
