#!/usr/bin/env python3
"""Build main out of develop: the same tree, with the working notes left behind.

    python tools/promote_main.py --dry-run      # what would be dropped, nothing written
    python tools/promote_main.py --orphan       # first promotion: a root commit, no parent
    python tools/promote_main.py                # later promotions: parent = the previous main

main is not a merge of develop, and that is the whole point. A merge would bring the notes back:
develop adds and changes them constantly, and a merge only ever asks "what changed on both sides".
So main gets a commit whose tree is develop's tree minus every path AGENT_NOTES matches, with the
previous main as its parent. The tree is written with git plumbing, in a throwaway index: nothing
is checked out and the working tree is never touched.

The tool then proves the only difference is the notes: `git diff --name-only <new main> <develop>`
has to be exactly the paths it dropped. If a source file moved between the two, that is a bug in
here and it says so instead of pushing.
"""

import argparse
import fnmatch
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Working notes for whoever drives this repository next: the plan, the phase handoffs, and the
# per-PR notes. docs/PRIVACY.md is NOT in here — it is written for a person using the app.
AGENT_NOTES = ["PLAN.md", "HANDOFF*", "docs/HANDOFF*", "docs/pr-*.md"]


def git(*args, stdin=None):
    proc = subprocess.run(["git", *args], cwd=str(ROOT), input=stdin, capture_output=True,
                          text=True)
    if proc.returncode != 0:
        sys.exit(f"FAIL: git {' '.join(args)}: {proc.stderr.strip()}")
    return proc.stdout.strip()


def dropped(paths):
    return [p for p in paths if any(fnmatch.fnmatch(p, pat) for pat in AGENT_NOTES)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", default="develop", help="the branch to promote (default develop)")
    ap.add_argument("--target", default="main", help="the branch to write (default main)")
    ap.add_argument("--orphan", action="store_true",
                    help="write a root commit, so the notes never appear in this branch's history")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    source = git("rev-parse", args.source)
    paths = git("ls-tree", "-r", "-z", "--name-only", source).split("\0")
    paths = [p for p in paths if p]
    gone = dropped(paths)
    if not gone:
        sys.exit("FAIL: nothing matched the note patterns. Refusing to promote an unfiltered tree.")

    print(f"source: {args.source} {source[:9]}  ({len(paths)} files)")
    print(f"dropping {len(gone)} working-note files:")
    for p in gone:
        print("  - " + p)
    if args.dry_run:
        return

    with tempfile.TemporaryDirectory(prefix="boreaz-promote-") as tmp:
        env = {**os.environ, "GIT_INDEX_FILE": str(Path(tmp) / "index")}
        git_env(env, "read-tree", f"{source}^{{tree}}")
        for chunk in [gone[i:i + 100] for i in range(0, len(gone), 100)]:
            git_env(env, "rm", "-r", "-f", "--cached", "--", *chunk)
        tree = git_env(env, "write-tree")

    parents = []
    if not args.orphan:
        tip = subprocess.run(["git", "rev-parse", "--verify", "-q", args.target], cwd=str(ROOT),
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        if tip.returncode == 0:
            parents = ["-p", tip.stdout.strip()]
    commit_args = ["commit-tree", tree] + parents + [
        "-m", f"promote {args.source} {source[:9]}: the shipped tree, without the working notes"]
    commit = git(*commit_args)

    check = git("diff", "--name-only", commit, source).split("\n")
    if sorted(x for x in check if x) != sorted(gone):
        sys.exit("FAIL: the promoted tree differs from the source by more than the notes.\n"
                 + "\n".join(sorted(set(check) - set(gone))))

    git("update-ref", f"refs/heads/{args.target}", commit)
    print(f"\nwrote {args.target} {commit[:9]}  (tree {tree[:9]}, "
          f"{'root commit' if not parents else 'child of ' + parents[1][:9]})")
    print(f"nothing else differs from {args.source}: {len(gone)} note files are the whole gap.")
    print(f"push with: git push {'--force-with-lease ' if args.orphan else ''}"
          f"origin {args.target}")


def git_env(env, *args):
    proc = subprocess.run(["git", *args], cwd=str(ROOT), env=env, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"FAIL: git {' '.join(args[:2])}: {proc.stderr.strip()}")
    return proc.stdout.strip()


if __name__ == "__main__":
    main()
