#!/usr/bin/env python3
"""A/B comparison between the Python reference and the xgcore DLL output.

Both tools dump the same set of files (meta.json plus binary arrays) into a
directory. This script compares them byte-for-byte.

Usage:
  python ab_compare.py <ref_dir> <dll_dir>
"""

import argparse
import json
import os
import sys


BINARY_FILES = [
    "order.bin",
    "curve_offsets.bin",
    "positions.bin",
    "widths.bin",
    "root_points.bin",
]


def compare_file(ref_path, dll_path):
    with open(ref_path, "rb") as f:
        ref = f.read()
    with open(dll_path, "rb") as f:
        dll = f.read()
    if len(ref) != len(dll):
        return "size differs: reference %d bytes, dll %d bytes" % (
            len(ref),
            len(dll),
        )
    for i in range(0, len(ref), 4):
        if ref[i : i + 4] != dll[i : i + 4]:
            return "first mismatch at byte offset %d" % i
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ref_dir")
    parser.add_argument("dll_dir")
    args = parser.parse_args()

    failures = []

    with open(os.path.join(args.ref_dir, "meta.json")) as f:
        ref_meta = json.load(f)
    with open(os.path.join(args.dll_dir, "meta.json")) as f:
        dll_meta = json.load(f)
    if ref_meta != dll_meta:
        failures.append("meta.json differs: %r vs %r" % (ref_meta, dll_meta))

    for name in BINARY_FILES:
        ref_path = os.path.join(args.ref_dir, name)
        dll_path = os.path.join(args.dll_dir, name)
        error = compare_file(ref_path, dll_path)
        if error:
            failures.append("%s: %s" % (name, error))

    if failures:
        print("FAIL")
        for f in failures:
            print("  - " + f)
        return 1
    print("PASS: reference and DLL outputs are identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
