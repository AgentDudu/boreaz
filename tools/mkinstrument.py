#!/usr/bin/env python3
"""Fetch and decode the CC0 starter instrument that --instrument plays.

    python tools/mkinstrument.py                      # full build, 32 MB, into third_party/
    python tools/mkinstrument.py --build compact      # the 2.2 MB build, same 66 regions
    python tools/mkinstrument.py --out D:/pianos/kw   # somewhere else
    python tools/mkinstrument.py --verify             # check what is on disk, write nothing

Why this script exists at all: the release ships **FLAC and OGG**, not WAV. PLAN.md said the
starter asset was an "SFZ+WAV build", and reading the release assets says otherwise, so the asset
needs one offline decode step before Boreaz can play it. That step belongs here rather than in the
app because the app's sample decode is deliberately "none at runtime" (PLAN, Technical decisions):
no decoder, no seek, no page fault between a key press and a sound.

The zip is hash-pinned, so the instrument Boreaz played is the instrument this file says it is, and
a changed upstream file is a failure here instead of a mystery on somebody's piano. Nothing is
written into the repository: the samples land in third_party/, which is ignored, and the CC0 LICENSE
travels with them.

Decoding needs ffmpeg on PATH (or `flac` for the FLAC build). This is a setup tool, run once, so a
developer dependency is cheap in a way a runtime dependency never is.
"""

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_OUT = REPO / "third_party" / "instruments" / "upright-piano-KW"
RELEASE = "https://github.com/freepats/upright-piano-KW/releases/download/v1.0.0/"

# Pinned against the release page on 2026-10-31. `regions` and `samples` are what --verify counts:
# an instrument that quietly lost a region is a piano with a silent patch of the keyboard.
BUILDS = {
    "full": {
        "zip": "UprightPianoKW-20220221.zip",
        "sha256": "3d602dcff94bcf9fb9b2d78df8c689814c895fd2776ee104a95b73c44bdc7e18",
        "suffix": ".flac",
        "mb": 32.4,
    },
    "compact": {
        "zip": "UprightPianoKW-20220221-compact.zip",
        "sha256": "1f6ba45d6a51dbcaa1931c5154d8304a06ad67e23999c93948eda42dc0d0ef90",
        "suffix": ".ogg",
        "mb": 2.2,
    },
}
SFZ = "UprightPianoKW-20220221.sfz"
REGIONS = 66
KEPT = ("LICENSE", "README.md")


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(build, out):
    """Return the local path of the pinned zip, downloading it if it is missing or wrong."""
    spec = BUILDS[build]
    cached = out / spec["zip"]
    if cached.exists() and sha256_of(cached) == spec["sha256"]:
        return cached
    url = RELEASE + spec["zip"]
    target = out / (spec["zip"] + ".download")
    print(f"fetching {url} ({spec['mb']} MB, under a minute on a normal link)")
    subprocess.run(["curl", "-sSL", "--fail", "--retry", "2", "-o", str(target), url], check=True)
    got = sha256_of(target)
    if got != spec["sha256"]:
        target.unlink(missing_ok=True)
        sys.exit(
            f"{spec['zip']}: sha256 is {got}\n"
            f"  expected {spec['sha256']}\n"
            "  the upstream release changed. Re-measure it, decide the new file is the one you "
            "want, and move the pin here: playing different audio than the plan describes is "
            "worse than a download that stops."
        )
    cached.unlink(missing_ok=True)
    target.rename(cached)
    return cached


def encoder(suffix):
    """The command that turns one encoded sample into PCM WAV. ffmpeg does both builds; `flac`
    is accepted for the FLAC build because some machines have that and not ffmpeg."""
    if shutil.which("ffmpeg"):
        return None
    if suffix == ".flac" and shutil.which("flac"):
        return "flac"
    sys.exit(
        "decoding needs ffmpeg on PATH"
        + (" (or `flac`)" if suffix == ".flac" else "")
        + ". The app itself needs neither: this is the one offline step that turns the release's "
        "FLAC/OGG into the PCM WAV Boreaz reads."
    )


def decode_one(src, dst, suffix):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if shutil.which("ffmpeg"):
        cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(src),
               "-c:a", "pcm_s16le", str(dst)]
    else:
        cmd = ["flac", "-d", "-f", "-s", "-o", str(dst), str(src)]
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode != 0:
        sys.exit(f"decoding {src.name} failed: {(done.stderr or done.stdout).strip()[:400]}")


def rewritten(text, suffix):
    """The mapping with its sample names pointing at the WAVs this tool wrote. Everything else -
    the loop points, the velocity groups, the tuning reference - is left exactly as recorded,
    because a generator that edits a mapping is a generator that lies about the instrument."""
    return re.sub(r"sample=(\S+)" + re.escape(suffix), r"sample=\1.wav", text)


def build(out, build, force):
    spec = BUILDS[build]
    out.mkdir(parents=True, exist_ok=True)
    archive = zipfile.ZipFile(fetch(build, out))
    text = archive.read(SFZ).decode("utf-8", "replace")
    encoder(spec["suffix"])  # fail before writing anything if the machine cannot decode

    written = out / SFZ
    written.write_text(rewritten(text, spec["suffix"]), encoding="utf-8", newline="\n")
    for name in KEPT:
        (out / name).write_bytes(archive.read(name))

    entries = [n for n in archive.namelist() if n.endswith(spec["suffix"])]
    done = 0
    for name in entries:
        dst = out / Path(name).parent / (Path(name).stem + ".wav")
        if dst.exists() and not force:
            continue
        decode_one_from(archive, name, dst, spec["suffix"])
        done += 1
    mb = sum(p.stat().st_size for p in (out / "samples").glob("*.wav")) / 1e6
    regions = written.read_text(encoding="utf-8").count("<region>")
    if regions != REGIONS:
        sys.exit(f"{written}: {regions} regions, expected {REGIONS}")
    print(f"{len(entries)} samples decoded ({done} this run), {mb:.0f} MB of PCM WAV, {regions} regions")
    print("play it:")
    print(f"  ./build/boreaz --gui --instrument {written.as_posix()}")
    print(f"  ./build/boreaz-sfz-probe {written.as_posix()}")


def decode_one_from(archive, name, dst, suffix):
    """Decode one member. ffmpeg and flac both read a file rather than a pipe, so the member is
    unpacked to a sibling temporary name and removed afterwards: 66 temp files for a minute is
    cheaper than a decoder that has to be handed a stream."""
    temp = dst.parent / (dst.stem + suffix)
    dst.parent.mkdir(parents=True, exist_ok=True)
    temp.write_bytes(archive.read(name))
    decode_one(temp, dst, suffix)
    temp.unlink(missing_ok=True)


def verify(out, build):
    """What --verify proves without touching the network: that the directory on disk is a decoded
    instrument of the shape the release has, that every sample the mapping names is a PCM WAV, and
    that the license came along. The bytes of the zip are what the pin proves, and that needs the
    zip; run the tool without --verify to check that."""
    spec = BUILDS[build]
    failures = []
    written = out / SFZ
    if not written.exists():
        failures.append(f"{written} is not there; run the tool without --verify")
        report(failures)
        return
    text = written.read_text(encoding="utf-8", errors="replace")
    regions = text.count("<region>")
    if regions != REGIONS:
        failures.append(f"{written}: {regions} regions, expected {REGIONS}")
    if spec["suffix"] in text:
        failures.append(f"{written} still names {spec['suffix']} files: it was not rewritten")
    named = re.findall(r"sample=(\S+)", text)
    if len(named) != REGIONS:
        failures.append(f"{len(named)} sample= lines, expected {REGIONS}")
    for name in named:
        path = out / Path(name)
        if not path.exists():
            failures.append(f"{path} is named by the mapping and is not there")
            continue
        head = path.read_bytes()[:12]
        # 'RIFF' .... 'WAVE', and format tag 1 at offset 20: PCM, which is all the app can read.
        if head[:4] != b"RIFF" or head[8:12] != b"WAVE":
            failures.append(f"{path} is not a WAVE file")
        elif path.stat().st_size < 44 or path.read_bytes()[20:22] != b"\x01\x00":
            failures.append(f"{path} is not PCM (format tag 1), so the app could not play it")
    license_text = out / "LICENSE"
    if not license_text.exists() or "Creative Commons" not in license_text.read_text(
        encoding="utf-8", errors="replace"
    ):
        failures.append(f"{license_text} is missing or is not the CC0 dedication")
    mb = sum(p.stat().st_size for p in (out / "samples").glob("*.wav")) / 1e6
    print(f"{len(named)} samples, {regions} regions, {mb:.0f} MB of PCM WAV at {out}")
    report(failures)


def report(failures):
    for line in failures:
        print("  " + line)
    if failures:
        sys.exit(1)
    print("ok")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", choices=sorted(BUILDS), default="full")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--force", action="store_true", help="decode again over what is there")
    parser.add_argument("--verify", action="store_true", help="check the directory, write nothing")
    args = parser.parse_args()
    if args.verify:
        verify(args.out, args.build)
    else:
        build(args.out, args.build, args.force)


if __name__ == "__main__":
    main()
