#!/usr/bin/env python
"""Runs the two stress-2 build phases and prints their medians."""
import statistics
import subprocess
import sys

ROOT = "D:/Actual Project Files/Project Nirqoba/build/ab"


def run(exe_path, key, rounds=5):
    values = []
    for _ in range(rounds):
        out = subprocess.run([exe_path], capture_output=True, text=True).stdout
        lines = out.splitlines()
        for index, line in enumerate(lines):
            if line.strip() == key:
                values.append(float(lines[index + 1]))
                break
    return values


def report(label, exe_path, key):
    values = run(exe_path, key)
    if not values:
        print(f"{label}: no {key} found")
        return
    print(
        f"{label:10s} {key:10s} median {statistics.median(values):8.1f} ms"
        f"  range {min(values):.0f}..{max(values):.0f}"
    )


if __name__ == "__main__":
    tag = sys.argv[1] if len(sys.argv) > 1 else ""
    report(f"02 {tag}", f"{ROOT}/bench02.exe", "BUILD_MS")
    report(f"02 {tag}", f"{ROOT}/bench02.exe", "COLLECT_MS")
    report(f"03 {tag}", f"{ROOT}/bench03.exe", "BUILD_MS")
