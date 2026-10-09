#!/usr/bin/env python3
"""Render every parameter at both ends of its range and fail if any made no
difference.

**This is the only thing in the repo that catches a dead control.** A GLSL
uniform whose name does not match the C++ is silently ignored --
`glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented no-op
-- so a slider can be stone dead while everything compiles, links, loads and
renders.

Each control is swept where it can act. Half of fiche's controls steer the
operator, and an operator's choice shows in the picture only once it has
been acted on, so every render is 200 frames (3.3 s at 60 fps) of a card that
pans 3 px a frame, and the operator's controls are swept in a context where
the hand is busy: Searching, a short dwell, crash zooms on. Shutter acts only
on a moving carriage, so it is swept with a slow hand that is nearly always
mid-pan; Rows shows only on the whole card. The View controls
(Position, Focus) belong to Manual, Interval to Filmed, the Title to a view
of the header, Aperture to a picture out of focus. An option parameter reads
back 0..1 whatever its count (the fleet's trap), so options are set here by
element index. A control whose ends differ by less than `--floor` (mean 8-bit
difference per channel) is reported as barely alive: vectrix's lesson.

Every render is 320x180 (CI's raster).

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose] [--jobs N] [--bare]

`--bare` drops every per-control context (keeping only the base), the
addendum's honesty check: what goes dead there is the list of controls that
only act with another one set, which AGENTS.md records.
"""

import argparse
import concurrent.futures
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

BASE = []

BUSY = ["Browse=2", "Dwell=0.05", "Crash Zoom=1"]
MANUAL = ["Operator=1"]

# name -> (low setting, high setting, context settings)
SWEEP = {
    "Operator": ("0", "1", []),
    "Browse": ("0", "2", ["Dwell=0.05", "Crash Zoom=1"]),
    "Dwell": ("0", "1", ["Browse=2", "Crash Zoom=1"]),
    "Sync": ("0", "3", BUSY),
    "Hand Speed": ("0", "1", BUSY),
    "Accuracy": ("0", "1", BUSY),
    "Crash Zoom": ("0", "1", ["Browse=2", "Dwell=0.05"]),
    "Focus Skill": ("0", "1", BUSY + ["Flatness=1"]),
    "Carriage Play": ("0", "1", BUSY),
    "Jump": ("0", "1", ["Dwell=1"]),
    "Zoom": ("0", "1", []),
    "Position X": ("0.3", "0.7", MANUAL),
    "Position Y": ("0.3", "0.7", MANUAL),
    "Focus": ("0.5", "1", MANUAL),
    "Layout": ("0", "1", MANUAL + ["Zoom=0"]),
    "Columns": ("4", "14", []),
    "Rows": ("2", "7", MANUAL + ["Zoom=0"]),
    "Gutter": ("0", "1", MANUAL + ["Zoom=0.3"]),
    "Content": ("0", "1", []),
    "Interval": ("0", "1", ["Content=1"]),
    "Film": ("0", "2", []),
    "Flatness": ("0", "1", MANUAL + ["Zoom=0.3", "Aperture=0"]),
    "Dust": ("0", "1", MANUAL + ["Zoom=0.85"]),
    "Scratches": ("0", "1", MANUAL + ["Zoom=0.3"]),
    "Title": ("A", "MICROFICHE", MANUAL + ["Zoom=0", "Position Y=0"]),
    "Aperture": ("0", "1", MANUAL + ["Focus=0.8"]),
    "Parfocal": ("0", "1", BUSY),
    "Shutter": ("0", "1", ["Browse=2", "Dwell=0", "Hand Speed=0", "Crash Zoom=0", "Flatness=0"]),
    "Hotspot": ("0", "1", []),
    "Lamp": ("0", "1", []),
    "Screen Grain": ("0", "1", []),
    "Room Light": ("0", "1", []),
    "Seed": ("1", "2", []),
    "Mix": ("0", "1", []),
}

# The About block: a text line and browser buttons, with no pixel to sweep.
SKIP = {"About", "Project page", "Source on GitHub", "Support the work", "User guide"}


def read_png(path):
    """Enough of PNG for mftest's own writer: 8-bit RGBA, filter 0 rows."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, width, height, idat = 8, 0, 0, b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for y in range(height):
        start = y * (stride + 1)
        out += raw[start + 1:start + 1 + stride]
    return bytes(out)


def difference(a, b):
    if len(a) != len(b):
        return 255.0
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def render(mftest, out, settings):
    args = [str(mftest), "--out", str(out), "--size", "320x180", "--frames", "200", "--moving"]
    for setting in settings:
        args += ["--set", setting]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"mftest failed: {' '.join(args)}\n{result.stderr.strip()}")
    return read_png(out)


def parameters(mftest):
    result = subprocess.run([str(mftest), "--list"], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"mftest --list failed: {result.stderr.strip()}")
    names = []
    for line in result.stdout.splitlines()[1:]:
        parts = re.split(r"\s{2,}", line.strip())
        if len(parts) >= 3 and parts[0].isdigit():
            names.append(parts[1].strip())
    return names


def sweep_one(mftest, scratch, name, bare):
    low, high, context = SWEEP[name]
    context = BASE + ([] if bare else context)
    tag = f"p{abs(hash(name))}"
    before = render(mftest, scratch / f"{tag}-a.png", context + [f"{name}={low}"])
    after = render(mftest, scratch / f"{tag}-b.png", context + [f"{name}={high}"])
    return name, difference(before, after)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--bare", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--floor", type=float, default=0.05,
                        help="mean 8-bit difference below which a control is 'barely alive'")
    args = parser.parse_args()

    build = args.build if args.build.is_absolute() else REPO / args.build
    mftest = build / "mftest"
    if not mftest.exists():
        print(f"{mftest} not found", file=sys.stderr)
        return 1

    declared = parameters(mftest)
    unknown = [n for n in declared if n not in SWEEP and n not in SKIP]
    if unknown:
        # A new parameter with no sweep is a hole, not a pass.
        print(f"no sweep defined for: {', '.join(unknown)}", file=sys.stderr)
        return 1
    unused = [n for n in SWEEP if n not in declared]
    if unused:
        print(f"the sweep names parameters the plugin does not have: {', '.join(unused)}", file=sys.stderr)
        return 1

    dead, weak = [], []
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(lambda n: sweep_one(mftest, scratch, n, args.bare), [n for n in declared if n in SWEEP]))
    for name, delta in results:
        if delta == 0.0:
            dead.append(name)
            print(f"  DEAD {name:16s} both ends identical")
        elif delta < args.floor:
            weak.append(name)
            print(f"  WEAK {name:16s} mean delta {delta:.4f}")
        elif args.verbose:
            print(f"  ok   {name:16s} mean delta {delta:.3f}")

    print(f"{len(results)} parameters swept{' (bare: base context only)' if args.bare else ''}, "
          f"{len(dead)} dead, {len(weak)} barely alive")
    if dead or weak:
        if not args.bare:
            print("\nA parameter that changes nothing is usually a uniform name that does not match\n"
                  "the C++, or a setting nothing reads. Both are silent everywhere else.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
