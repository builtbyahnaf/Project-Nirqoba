#!/usr/bin/env python3
"""Check that neither runtime configuration can silently lose an invariant check.

The runtime is built twice from one source list (``NIRQOBA_RUNTIME_SOURCES``), so
a check can only differ between the two libraries if it sits inside a
configuration guard. This script enforces that:

1. No ``nrq_runtime_fatal`` call appears inside a guard on ``NRQ_DEBUG_ENABLED``
   (or a guard on ``NRQ_RUNTIME_DEBUG_ENABLED``) in the runtime sources. A fatal
   check inside such a guard would exist in one configuration only, which is the
   one thing the split must never do to memory-safety or heap-integrity checks.
2. ``runtime/CMakeLists.txt`` adds no per-target sources: both targets are
   created by ``nirqoba_add_runtime_target`` and inherit the same list, so the
   two libraries cannot drift apart file by file.

Run by ``scripts/build.ps1`` after a successful build, and by hand:
    python scripts/check_runtime_check_parity.py

Exits 0 when the invariant holds, 1 with a description when it does not.
"""

from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNTIME_SRC = os.path.join(ROOT, "runtime", "src")
RUNTIME_CMAKE = os.path.join(ROOT, "runtime", "CMakeLists.txt")

CONFIG_MACROS = ("NRQ_DEBUG_ENABLED", "NRQ_RUNTIME_DEBUG_ENABLED")
GUARD_RE = re.compile(
    r"^\s*#\s*(?:if|ifdef|ifndef|elif|else|endif|if\s+defined)\b")
MACRO_RE = re.compile(r"NRQ_(?:RUNTIME_)?DEBUG_ENABLED")
FATAL_RE = re.compile(r"nrq_runtime_fatal\s*\(")


def configuration_regions(lines):
    """Yield the line index ranges that are conditional on the configuration.

    Only regions whose controlling condition mentions a configuration macro are
    returned; other preprocessor use (include guards, platform guards) is
    irrelevant here. Each returned tuple is (first_line, last_line) inclusive,
    1-based, covering the guarded body and not the `#if`/`#endif` lines.
    """
    regions = []
    stack = []  # (line_no, controlled_by_configuration)
    for index, line in enumerate(lines, start=1):
        stripped = line.strip()
        if not GUARD_RE.match(line):
            continue
        if stripped.startswith("#if") or stripped.startswith("#ifdef") or \
                stripped.startswith("#ifndef"):
            enclosing = any(flag for _, flag in stack)
            stack.append((index, enclosing or bool(MACRO_RE.search(stripped))))
        elif stripped.startswith("#else") or stripped.startswith("#elif"):
            if stack:
                start, flag = stack[-1]
                if flag:
                    regions.append((start + 1, index - 1))
                stack[-1] = (index, flag)
        elif stripped.startswith("#endif"):
            if stack:
                start, flag = stack.pop()
                if flag:
                    regions.append((start + 1, index - 1))
    return [(int(lo), int(hi)) for lo, hi in regions if lo <= hi]


def check_fatals():
    problems = []
    total = 0
    for name in sorted(os.listdir(RUNTIME_SRC)):
        if not name.endswith((".cpp", ".h")):
            continue
        path = os.path.join(RUNTIME_SRC, name)
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            lines = handle.readlines()
        guarded = configuration_regions(lines)
        for number, line in enumerate(lines, start=1):
            if not FATAL_RE.search(line) or line.lstrip().startswith("//"):
                continue
            total += 1
            for low, high in guarded:
                if low <= number <= high:
                    problems.append(
                        "%s:%d: invariant check inside a configuration guard"
                        % (os.path.relpath(path, ROOT), number))
                    break
    return total, problems


def check_source_list():
    problems = []
    with open(RUNTIME_CMAKE, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    if "target_sources" in text:
        problems.append(
            "runtime/CMakeLists.txt adds per-target sources; both configurations "
            "must be built from NIRQOBA_RUNTIME_SOURCES")
    for target in ("nirqoba_runtime", "nirqoba_runtime_debug"):
        if target not in text:
            problems.append("runtime/CMakeLists.txt does not build %s" % target)
    return problems


def main():
    total, problems = check_fatals()
    problems += check_source_list()
    if problems:
        for problem in problems:
            print("check-runtime-parity: %s" % problem)
        return 1
    print("check-runtime-parity: %d invariant checks, present in both "
          "configurations" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
