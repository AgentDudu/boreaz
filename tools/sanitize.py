#!/usr/bin/env python3
"""Re-run the AddressSanitizer gate, so the claim in PLAN stays a gate and not a memory.

    python tools/sanitize.py                      # the compiler build/ already uses
    python tools/sanitize.py --compiler clang++   # name one; it has to be able to do ASan

It probes first: compile and run one instrumented program, then build the tree, then run the suite.
The tree it builds lives in build-san/, ignored like every other build directory, and is deliberately
separate from build/ and build-clang/ -- an instrumented build is not a measurement build, and mixing
them is how this gate went quiet once already.

Windows notes, written down so the next run does not relearn them: leak detection is off because LSan
does not work on Windows, and ASan's handling of structured exceptions disagrees with miniaudio's
frames, so that is off too. ASH_* is what the MSVC AddressSanitizer runtime reads, ASAN_* is what the
GCC and clang.gnu runtime reads; both are set because either toolchain may be the one configured.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build-san"

FLAGS = "-fsanitize=address -fno-omit-frame-pointer -g"

ASAN_ENV = {
    "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1",
    "ASH_OPTIONS": "no_leaks=true",
    "ASH_VSEH": "0",
    "ASH_NO_THREAD_SAFETY_ANALYSIS": "1",
}


def run(argv, what):
    print("+ " + " ".join(str(a) for a in argv), flush=True)
    proc = subprocess.run([str(a) for a in argv], cwd=str(ROOT))
    if proc.returncode != 0:
        sys.exit(f"FAIL: {what} exited {proc.returncode}")


def resolve_compiler(explicit):
    """The compiler to use: the one asked for, else the one the main tree is already configured with."""
    if explicit:
        return explicit
    cache = ROOT / "build" / "CMakeCache.txt"
    if cache.exists():
        m = re.search(r"^CMAKE_CXX_COMPILER:[A-Z]+=(.+)$", cache.read_text(encoding="utf-8"),
                      re.MULTILINE)
        if m:
            return m.group(1)
    sys.exit("FAIL: no compiler to probe. Pass --compiler <path>; the sanitizer build is pinned to "
             "one compiler so the build and the probe cannot drift.")


def probe(cxx):
    """Prove the compiler can build AND RUN an instrumented program before building the tree.

    Compiling with -fsanitize=address and running it are two separate abilities and a toolchain can
    have either one alone: a GCC built without libsanitizer cannot link at all, and an LLVM whose ASan
    runtime does not service this target links fine and then dies at load. Both are worse than a
    refusal, because the suite would then report a green it never measured.
    """
    work = Path(tempfile.mkdtemp(prefix="boreaz-asan-probe-"))
    src = work / "probe.cpp"
    src.write_text('#include <cstdio>\n'
                   'int main() { char* p = new char[8]; p[3] = 7; std::printf("probe %d\\n", p[3]);\n'
                   '           delete[] p; return 0; }\n', encoding="utf-8")
    exe = work / ("probe.exe" if os.name == "nt" else "probe")
    link = subprocess.run([cxx, "-fsanitize=address", "-g", str(src), "-o", str(exe)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if link.returncode != 0:
        sys.exit("FAIL: this toolchain cannot link AddressSanitizer.\n"
                 f"{link.stdout.strip()[-400:]}\n"
                 "A GCC built without libsanitizer is one such toolchain -- a stock Strawberry GCC "
                 "is. Install a toolchain whose ASan runtime is present and re-run.")
    if not exe.exists():
        sys.exit("FAIL: the probe reported success and produced no executable.")
    go = subprocess.run([str(exe)], env={**os.environ, **ASAN_ENV},
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if "probe 7" not in go.stdout:
        sys.exit("FAIL: the instrumented probe did not run. The binary carries ASan instrumentation "
                 "but its runtime is missing or does not service this target, so a suite run here "
                 f"would measure nothing.\n{go.stdout.strip()[-400:]}")
    print("probe: an instrumented program compiled and ran.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default=None, help="C++ compiler for this build")
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()

    cxx = resolve_compiler(args.compiler)
    print(f"compiler: {cxx}")
    probe(cxx)

    run(["cmake", "-S", ".", "-B", str(OUT), "-G", "Ninja",
         "-D", "CMAKE_BUILD_TYPE=Debug",
         "-D", f"CMAKE_CXX_FLAGS={FLAGS}",
         "-D", "BOREAZ_BUILD_TESTS=ON",
         "-D", f"CMAKE_CXX_COMPILER={cxx}"], "configure")
    run(["cmake", "--build", str(OUT), "-j", str(args.jobs)], "build")

    tests = OUT / "boreaz_tests.exe"
    if not tests.exists():
        tests = OUT / "boreaz_tests"
    print("+ env " + " ".join(f"{k}={v}" for k, v in sorted(ASAN_ENV.items())), flush=True)
    proc = subprocess.run([str(tests)], cwd=str(ROOT), env={**os.environ, **ASAN_ENV},
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    sys.stdout.write(proc.stdout)
    for line in proc.stdout.splitlines():
        if re.search(r"\[doctest\]: (test cases|assertions)", line):
            print("ASan suite: " + line.strip())
    if "AddressSanitizer" in proc.stdout:
        sys.exit("FAIL: AddressSanitizer reported something. That is the gate working.")
    if proc.returncode != 0:
        sys.exit(f"FAIL: the suite exited {proc.returncode} under ASan")
    print("OK: the suite is green under AddressSanitizer.")


if __name__ == "__main__":
    main()
