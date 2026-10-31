#!/usr/bin/env python3
"""Build the Salamander subset Boreaz ships with, from the upstream SFZ repository.

    python tools/mkinst.py --dry-run          # what would be fetched, and what it would cost
    python tools/mkinst.py                    # fetch, decode, write the instrument
    python tools/mkinst.py --layers 1,10,16   # a different velocity subset

Upstream is `sfzinstruments/SalamanderGrandPiano` at one pinned commit: 641 FLAC recordings plus the
mapping **as authored data** - `Data/notes.txt` is 16 velocity groups with their own `lovel`/`hivel`
bands, `Data/region.txt` the 30 minor-third sample regions, `Data/hammer.txt` the chromatic key-off
noise. That is why this tool assembles instead of subsetting. The published `.sfz` wraps those pieces
in `<master>` key switches (C0 natural, C#0 retuned), CC gates (`locc20/21/22` for string resonance,
hammer noise, pedal noise) and CC-modulated opcodes (`offset_oncc98`, `amp_veltrack_oncc99`,
`ampeg_release_oncc72`). None of that reaches this engine, so copying the file would put twelve
tunings and three gated layers in front of a reader that would pick one of each at random. Assembling
one flat SFZ is what makes the result predictable; writing the dropped opcodes to `provenance.txt`,
with a count each, is what makes it auditable.

`--dry-run` answers the size question before the bandwidth is spent: the upstream tree listing carries
the byte size of every file, so the fetch cost is exact and the decoded cost is a measured factor.
Integrity is the git blob hash of each file fetched, checked against the tree listing of the pinned
commit: a moved upstream is a refusal, not a surprise.

Needs ffmpeg on PATH. Writes only under `--out`.
"""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import urllib.parse
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
REPO = "sfzinstruments/SalamanderGrandPiano"
COMMIT = "3382bf9496bba2486f5ab0de55a264d1dfc38404"
API_TREE = f"https://api.github.com/repos/{REPO}/git/trees/{COMMIT}?recursive=1"
RAW = f"https://raw.githubusercontent.com/{REPO}/{COMMIT}/"

# The two trees worth building. The second <master> (retuned, switched by C#0) and the string-resonance
# and pedal blocks are not here: no <master>, no sw_*, no locc gating in this engine.
NOTE_TREE = ["Data/tune_nat.txt", "Data/notes.txt"]
HAMMER_TREE = ["Data/hammer.txt"]

# Opcodes the reader in src/sfz/ honours. Everything else is dropped and counted.
KEPT = {"sample", "lokey", "hikey", "key", "pitch_keycenter", "pitch_keyroot", "lovel", "hivel",
        "volume", "ampegain", "tune", "amp_veltrack", "ampeg_release", "trigger", "group", "count",
        "offset", "loop_start", "loop_start", "loop_end", "loop_end", "loop_mode", "pan"}

# Decoded mono 16-bit bytes per FLAC byte, measured on four upstream files: 0.61 for a 0.46 s hammer
# noise, 0.96 for a 2.6 s high note, ~0.80 for the long decays.
WAV_PER_FLAC = 0.80
WAV_PER_FLAC_SPAN = (0.61, 0.96)

# This subset rendered 0.4044 peak at 0 dB against the shipped upright's 0.6121, so it ships about
# 4 dB quiet. +6 dB on the regions lands it at 0.6941 measured, i.e. +4.7 dB of that reached the
# output: `volume=` is not a pure gain stage in this engine, and nobody has worked out why yet.
LEVEL_DB = 6.0

LICENSE_FILES = ["LICENSE", "README.md"]
ORDER = ["trigger", "lokey", "hikey", "key", "pitch_keycenter", "lovel", "hivel", "group", "tune",
         "volume", "amp_veltrack", "ampeg_release", "offset", "sample"]


def git_blob_sha1(data):
    """Git's own object id, which is what the tree listing reports for every file."""
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


class Upstream:
    """The pinned commit, one file at a time, verified against the tree listing of that commit."""

    def __init__(self, cache):
        self.cache = cache
        cache.mkdir(parents=True, exist_ok=True)
        self.sizes, self.oids = {}, {}
        for entry in json.loads(self._get(API_TREE, "tree.json"))["tree"]:
            if entry["type"] == "blob":
                self.sizes[entry["path"]] = entry["size"]
                self.oids[entry["path"]] = entry["sha"]

    def _get(self, url, name):
        target = self.cache / name
        if not target.exists():
            with urllib.request.urlopen(url, timeout=300) as response:
                target.write_bytes(response.read())
        return target.read_bytes()

    def checked(self, path, name):
        data = self._get(RAW + urllib.parse.quote(path), name)
        want = self.oids.get(path)
        if want and git_blob_sha1(data) != want:
            raise SystemExit(f"{path}: blob hash does not match {COMMIT[:12]}")
        return data

    def text(self, path):
        return self.checked(path, "text-" + path.replace("/", "_")).decode("utf-8")

    def binary(self, path):
        return self.checked(path, "sample-" + path.replace("/", "_"))


def include(upstream, path, defines, out):
    """Append the opcode lines of one upstream file, expanding #include and applying #define.

    Sequential with defines persisting, which is how the upstream files work: `Data/notes.txt` puts
    `#include "Data/vel_06.txt"` and `#include "Data/region.txt"` on one `<group>` line, and the only
    reason region.txt's 30 regions come out named `A0v6.flac` is that vel_06's `#define $VEL v6` is
    still in force while region.txt is expanded. The group's own opcodes are emitted before the
    includes' contents, because the header has to precede the regions it applies to.
    """
    for raw in upstream.text(path).splitlines():
        line = raw.split("//")[0].strip()
        if not line:
            continue
        own, contents = "", []
        rest = line
        while True:
            found = re.search(r'#include\s+"([^"]+)"', rest)
            if not found:
                break
            own += rest[: found.start()]
            before = len(out)
            include(upstream, found.group(1), defines, out)  # nested, so its defines are already set
            contents.extend(out[before:])
            del out[before:]
            rest = rest[found.end():]
        if own or contents:
            own = substitute((own + " " + rest).strip(), defines)
            if own:
                out.append(own)
            out.extend(contents)
            continue
        defined = re.match(r"^#define\s+\$(\w+)\s+(.*)$", line)
        if defined:
            defines[defined.group(1)] = defined.group(2).strip()
            continue
        out.append(substitute(line, defines))


def substitute(line, defines):
    for _ in range(8):
        done = re.sub(r"\$(\w+)", lambda m: defines.get(m.group(1), "$" + m.group(1)), line)
        if done == line:
            return done
        line = done
    return line


def regions_of(lines):
    """Opcode lines into regions, with <global> and <group> scope applied.

    Two rules the upstream files make necessary. (1) A new <group> starts again from <global>, not
    from the group above it - the rule this repository shipped broken once (PLAN.md phase 6b). (2) A
    group's opcodes may sit on bare lines under its header, as they do in `Data/hammer.txt`:

        <group>
        group_label=rel
        amp_veltrack=82
        volume=-37
        <region> region_label=01 key=21 sample=rel1.flac

    so a bare line belongs to whatever section is open and to every region that follows it.
    """
    regions = []
    glob, group = {}, {}
    for line in lines:
        header = re.match(r"^<(\w+)>(.*)$", line)
        if header:
            name, rest = header.group(1).lower(), dict(opcodes(header.group(2)))
            if name == "global":
                glob, group = rest, {}
            elif name == "group":
                group = {**group, **rest}
            elif name == "region":
                regions.append({**glob, **group, **rest})
            continue
        found = dict(opcodes(line))
        if found:
            group = {**group, **found}
    return regions


def bands_of(lines):
    """The velocity bands the mapping declares, in the order the file lists them."""
    bands = []
    for line in lines:
        header = re.match(r"^<group>(.*)$", line)
        if header:
            found = dict(opcodes(header.group(1)))
            band = (int(found.get("lovel", 1)), int(found.get("hivel", 127)))
            if band not in bands:
                bands.append(band)
    return bands


def reband(bands, layers):
    """New contiguous lovel/hivel per kept layer: 1..127, no gaps, no overlaps.

    Cut points are the library's own hivel values nearest the quartiles, and each new band keeps the
    recording whose own band centre was nearest the new centre. Round numbers like 1-32 would be
    easier to type and further from what was recorded.
    """
    edges = []
    for i in range(1, len(layers)):
        want = 127 * i / len(layers)
        # Only boundaries that sit between the kept bands are legal cuts.
        low = max(bands[l - 1][1] for l in layers[:i])
        high = min(bands[l - 1][0] for l in layers[i:]) - 1
        legal = [edge for edge in sorted({b[1] for b in bands}) if low <= edge <= high]
        edges.append(min(legal, key=lambda edge: abs(edge - want)))
    # The lowest band starts at 0, not 1: SFZ velocity ranges are inclusive and 0 is a legal
    # velocity, so a mapping that starts at 1 leaves the sample store with keys no layer covers.
    bounds = [0] + edges + [127]
    return {
        number: (bounds[i] + (1 if i else 0), bounds[i + 1])
        for i, number in enumerate(layers)
    }


def opcodes(text):
    pairs = []
    for word in text.split():
        if "=" in word:
            key, value = word.split("=", 1)
            pairs.append((key.lower(), value))
    return pairs


def keep(region, dropped, forcing=None, level_db=0.0):
    """The opcodes this engine reads, with the rest counted for the provenance file."""
    out = {}
    for key, value in region.items():
        if key in KEPT:
            out[key] = value
        else:
            dropped[key] = dropped.get(key, 0) + 1
    for key, value in (forcing or {}).items():
        out[key] = value
    if "sample" not in out:
        raise SystemExit(f"a region asks for nothing to play: {region}")
    if "key" in out and "lokey" not in out:
        # This engine resolves a note through lokey/hikey plus pitch_keycenter; `key` alone is a
        # shorthand only the SFZ spec offers.
        out["lokey"] = out["hikey"] = out["pitch_keycenter"] = out.pop("key")
    out["sample"] = out["sample"].rsplit(".", 1)[0] + ".wav"
    if level_db:
        # The level lives here rather than in the engine default on purpose: a non-zero engine
        # default would make every other instrument too loud and would hide gain behind a mixer.
        # Written on the region, it is reviewable in the file, and it stands in for upstream's
        # `set_cc7 100` + `amplitude_oncc7 100`, which is how that library reaches its own loudness.
        out["volume"] = f"{float(out.get('volume', 0)) + level_db:g}"
    return out


def assemble(upstream, layers, releases):
    defines = {}
    for line in upstream.text("Salamander Grand Piano V3.sfz").splitlines():
        found = re.match(r"^\s*#define\s+\$(\w+)\s+(.*)$", line.split("//")[0])
        if found:
            defines[found.group(1)] = found.group(2).strip()

    dropped = {}
    note_lines = []
    for path in NOTE_TREE:
        include(upstream, path, defines, note_lines)
    regions = regions_of(note_lines)
    bands = bands_of(note_lines)
    if len(bands) != 16:
        raise SystemExit(f"expected 16 velocity groups upstream, found {len(bands)}")
    if not regions:
        raise SystemExit("the note mapping produced no regions")

    new_bands = reband(bands, layers)
    notes = []
    for number in layers:
        lo, hi = bands[number - 1]
        for region in regions:
            if (int(region.get("lovel", 1)), int(region.get("hivel", 127))) != (lo, hi):
                continue
            notes.append(keep(region, dropped, {
                "lovel": str(new_bands[number][0]), "hivel": str(new_bands[number][1])}, LEVEL_DB))

    hammers = []
    if releases == "hammer":
        lines = []
        for path in HAMMER_TREE:
            include(upstream, path, defines, lines)
        for region in regions_of(lines):
            # Upstream gates these behind `locc21=1` and puts `trigger=release` in a <global> two
            # <master> blocks away. Say it on the region, where the reader can see it.
            hammers.append(keep(region, dropped, {"trigger": "release"}, LEVEL_DB))
    return notes, hammers, bands, new_bands, dropped, len(regions)


def cost(notes, hammers, upstream, bands, new_bands, layers):
    names = [region["sample"] for region in notes + hammers]
    flac = sum(upstream.sizes["Samples/" + name.replace(".wav", ".flac")] for name in names)
    low, high = WAV_PER_FLAC_SPAN
    print(f"upstream   {REPO} @ {COMMIT[:12]}")
    print(f"layers     " + ", ".join(
        f"v{n} {bands[n-1][0]}-{bands[n-1][1]} -> {new_bands[n][0]}-{new_bands[n][1]}"
        for n in layers))
    print(f"regions    {len(notes)} notes + {len(hammers)} key-off = {len(names)} samples, "
          f"{len(set(names))} distinct")
    print(f"fetch      {flac / 1e6:.1f} MB FLAC (upstream as authored)")
    print(f"output     about {flac * WAV_PER_FLAC / 1e6:.0f} MB mono 16-bit 44.1 kHz WAV "
          f"({flac * low / 1e6:.0f}-{flac * high / 1e6:.0f} MB; factor measured on four files)")
    return names, flac


def sfz_text(notes, hammers, layers, bands, new_bands):
    header = f"""; Salamander Grand Piano, subset for Boreaz.
;
// Assembled from {REPO} @ {COMMIT[:12]} by tools/mkinst.py - not a copy of the upstream file.
// Kept: the natural tuning, {len(layers)} of its 16 velocity groups ({", ".join("v" + str(n) for n in layers)}),
// all 88 chromatic hammer-noise releases. Dropped: the retuned variant, string resonance, pedal noise,
// every *_oncc opcode, and the library's CC7 gain semantics. See NOTICE.txt and provenance.txt.
;
// Velocity bands were re-cut so the kept layers cover 1-127 with no gaps:
""" + "".join(
        f";   v{n}: was {bands[n-1][0]}-{bands[n-1][1]}, now {new_bands[n][0]}-{new_bands[n][1]}\n"
        for n in layers) + ";\n<global>\n// no defaults: every region below carries what it needs\n"
    body = "".join(
        "<region> " + " ".join(f"{key}={region[key]}"
                               for key in ORDER + sorted(set(region) - set(ORDER))
                               if key in region) + "\n"
        for region in notes + hammers)
    return header + "\n" + body


NOTICE = f"""Salamander Grand Piano - subset used by Boreaz
=============================================

Upstream : {REPO} (Alexander Holm), commit {COMMIT}
Licence  : Creative Commons Attribution 3.0 Unported. See LICENSE and the upstream README.
Built by : tools/mkinst.py, from the upstream SFZ and its Data/*.txt mapping.

What this directory is
----------------------
A subset, not the library. {0} velocity layers of the natural tuning, the 30 sample regions
they cover, and all 88 chromatic key-off hammer-noise recordings, decoded from the upstream
48 kHz / 24-bit stereo FLAC to 44.1 kHz / 16-bit mono WAV (downmixed, ffmpeg).

What is not here, and why
-------------------------
The retuned variant (selected by C#0 upstream), string-resonance and pedal-noise layers (gated by
CC 20/21/22 upstream), and every `*_oncc` opcode: this engine reads no key switches, no CC gating, no
per-CC modulation. `provenance.txt` lists each dropped opcode with a count.

One semantic difference worth knowing: upstream reaches its default loudness through
`set_cc7 100` plus `amplitude_oncc7 100`, a curve that is almost linear across the upper half. This
engine has no `set_cc7`, so its CC7 (a plain linear fader, and the file's overall gain) has to be
lifted to about the same perceived level. Same intent, different numbers: 1.0 here is upstream's
~0.55 at CC7 100.
"""


def write_out(args, notes, hammers, upstream, names, bands, new_bands, layers, dropped, regions):
    out = args.out
    if out.exists() and any(out.iterdir()) and not args.force:
        raise SystemExit(f"{out} is not empty; pass --force to overwrite it")
    out.mkdir(parents=True, exist_ok=True)
    for name in sorted(set(names)):
        source = upstream.binary("Samples/" + name.replace(".wav", ".flac"))
        target = out / name
        temporary = out / (name + ".flac")
        temporary.write_bytes(source)
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(temporary),
                        "-ac", "1", "-ar", "44100", "-c:a", "pcm_s16le", str(target)], check=True)
        temporary.unlink()
    (out / "salamander.sfz").write_text(sfz_text(notes, hammers, layers, bands, new_bands),
                                        encoding="utf-8")
    (out / "NOTICE.txt").write_text(
        NOTICE.format(len(layers)) + f"\nWrote {len(set(names))} samples, "
        f"{len(notes)} note regions and {len(hammers)} key-off regions.\n", encoding="utf-8")
    for name in LICENSE_FILES:
        (out / name).write_text(upstream.text(name), encoding="utf-8")
    provenance = [
        "provenance of this instrument",
        "=============================",
        f"command    : {' '.join(sys.argv)}",
        f"upstream   : {REPO} @ {COMMIT}",
        f"assembled  : {regions} note regions upstream, {len(layers)} velocity layers kept "
        f"({', '.join(str(n) for n in layers)})",
        f"releases   : {'88 chromatic hammer noise' if hammers else 'none'}",
        "not built  : the retuned tuning (<master> + sw_*), string resonance (locc20),",
        "             pedal noise (locc22), the cc7 amplitude curve (<curve>, unused upstream)",
        "rebuilt    : lovel/hivel re-cut to cover 1-127 with no gaps; trigger=release moved onto",
        "             each key-off region; key= expanded to lokey/hikey/pitch_keycenter",
        "open       : the notes carry no group and the key-off noise is group 3, so with group_count",
        "             2 the hammer never steals the note it follows - upstream numbers it that way",
        "             (you hear mechanism and string). If the ear wants the note cut instead, give",
        "             the note regions group=1: that is the one number that turns the steal on.",
        "",
        "opcodes dropped because this engine does not read them, with counts:",
    ] + [f"  {key:<32} {count}" for key, count in sorted(dropped.items(), key=lambda kv: (-kv[1], kv[0]))]
    (out / "provenance.txt").write_text("\n".join(provenance) + "\n", encoding="utf-8")
    written = sum(target.stat().st_size for target in out.glob("*.wav"))
    print(f"output     {len(set(names))} WAVs written, {written / 1e6:.1f} MB")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=REPO_ROOT / "third_party" / "instruments" /
                        "salamander")
    parser.add_argument("--layers", default="1,6,10,14",
                        help="upstream velocity groups to keep, 1 is the softest")
    parser.add_argument("--releases", choices=["hammer", "none"], default="hammer")
    parser.add_argument("--cache", type=Path, default=REPO_ROOT / "build" / "mkinst-cache")
    parser.add_argument("--dry-run", action="store_true", help="print the cost, write nothing")
    parser.add_argument("--force", action="store_true", help="overwrite an existing --out")
    args = parser.parse_args(argv)

    layers = [int(part) for part in args.layers.split(",") if part.strip()]
    if not layers or sorted(set(layers)) != sorted(layers) or min(layers) < 1 or max(layers) > 16:
        raise SystemExit("--layers wants distinct numbers from 1 to 16")

    upstream = Upstream(args.cache)
    notes, hammers, bands, new_bands, dropped, regions = assemble(upstream, layers, args.releases)
    names, _flac = cost(notes, hammers, upstream, bands, new_bands, layers)
    if dropped:
        print("dropped    " + ", ".join(f"{key} {count}" for key, count in
                                        sorted(dropped.items(), key=lambda kv: -kv[1])[:8]))
    if args.dry_run:
        print(f"target     {args.out} (not written: --dry-run)")
        return 0
    write_out(args, notes, hammers, upstream, names, bands, new_bands, layers, dropped, regions)
    print(f"target     {args.out}")
    print(f"check      build/boreaz-sfz-probe.exe {args.out / 'salamander.sfz'}")
    print(f"listen     python tools/listen.py --instrument {args.out / 'salamander.sfz'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
