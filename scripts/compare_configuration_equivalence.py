#!/usr/bin/env python3
"""Compare the two runtime configurations on the same programs.

The development configuration may be slower and may emit observation hooks; it
must not collect differently. This script builds every chosen integration program
twice - once for `nirqoba_runtime`, once for `nirqoba_runtime_debug` - runs both,
and compares exit status, stdout and stderr byte for byte.

Programs whose own output is not reproducible are skipped rather than reported as
a difference: a case is skipped when running the *production* binary twice already
disagrees with itself, because then a mismatch cannot be attributed to the
configuration.

Usage:
    python scripts/compare_configuration_equivalence.py \
        --compiler build/msvc-dev/bin/nirqoba.exe [--filter gc_*] [--verbose]

Exit status is 0 when every stable program agrees across configurations, 1 when
one does not, 2 when the script could not run (missing compiler, no cases).
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
POSITIVE = os.path.join(ROOT, "tests", "integration", "positive")
RUN_TIMEOUT_SECONDS = 120


def cases(pattern):
    found = []
    for name in sorted(os.listdir(POSITIVE)):
        if not fnmatch.fnmatch(name, pattern):
            continue
        directory = os.path.join(POSITIVE, name)
        source = os.path.join(directory, "main.nrqba")
        if os.path.isfile(source):
            found.append((name, directory, source))
    return found


def build(compiler, source, output, workdir, debug):
    command = [compiler, "build", source, "-o", output]
    if debug:
        command.append("--debug-enabled")
    try:
        result = subprocess.run(
            command, cwd=workdir, capture_output=True, text=True,
            timeout=RUN_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        return None, "compilation timed out"
    if result.returncode != 0:
        return None, "compilation failed: %s" % (result.stderr or result.stdout)
    return output, None


def run(executable, workdir):
    try:
        result = subprocess.run(
            [executable], cwd=workdir, capture_output=True, text=True,
            timeout=RUN_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        return None
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--filter", default="*")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    compiler = os.path.abspath(args.compiler)
    if not os.path.isfile(compiler):
        print("no compiler at %s" % compiler)
        return 2

    selected = cases(args.filter)
    if not selected:
        print("no integration programs matched %r" % args.filter)
        return 2

    agreed = 0
    skipped = []
    failed = []
    for name, directory, source in selected:
        workdir = tempfile.mkdtemp(prefix="nirqoba-equivalence-")
        try:
            production, error = build(
                compiler, source, os.path.join(workdir, "production.exe"),
                workdir, debug=False)
            if error:
                # The comparison needs a program that runs. A case the normal
                # harness only type-checks (or one needing libs the plain link
                # line does not carry) cannot evidence anything either way, so it
                # is skipped with the reason rather than counted as a difference.
                skipped.append((name, "production build: " + error.strip()))
                continue
            first = run(production, directory)
            second = run(production, directory)
            if first is None or second is None:
                skipped.append((name, "did not finish"))
                continue
            if (first.returncode, first.stdout, first.stderr) != \
                    (second.returncode, second.stdout, second.stderr):
                skipped.append((name, "output is not reproducible"))
                continue

            development, error = build(
                compiler, source, os.path.join(workdir, "development.exe"),
                workdir, debug=True)
            if error:
                failed.append((name, error))
                continue
            observed = run(development, directory)
            if observed is None:
                failed.append((name, "development build did not finish"))
                continue
            if (observed.returncode, observed.stdout, observed.stderr) != \
                    (first.returncode, first.stdout, first.stderr):
                failed.append((name, "behaviour differs between configurations"))
                if args.verbose:
                    print("  production : rc=%d %r" %
                          (first.returncode, first.stdout))
                    print("  development: rc=%d %r" %
                          (observed.returncode, observed.stdout))
                continue
            agreed += 1
        finally:
            shutil.rmtree(workdir, ignore_errors=True)

    for name, reason in skipped:
        print("skipped %s: %s" % (name, reason))
    for name, reason in failed:
        print("FAILED %s: %s" % (name, reason))
    print("configuration equivalence: %d agreed, %d skipped, %d failed"
          % (agreed, len(skipped), len(failed)))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
