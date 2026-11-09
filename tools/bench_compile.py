#!/usr/bin/env python3
"""Author: dhjs0000

Repeatable M3 compile-speed benchmark.

Generates a single-function .wir with N dependent integer adds and measures
whippet -S (GNU as text) and/or whippet -c (O1 + internal COFF assembler).

Example:
    python tools/bench_compile.py --whippet build/whippet.exe --instructions 20000
"""
import argparse
import os
import statistics
import subprocess
import sys
import tempfile
import time


def generate(path, count):
    lines = [
        "func @bench(i32 %x) -> i32 {",
        "entry:",
        "  %v0 = add i32 %x, 1",
    ]
    for i in range(1, count):
        lines.append(f"  %v{i} = add i32 %v{i - 1}, 1")
    lines.append(f"  ret i32 %v{count - 1}")
    lines.append("}")
    with open(path, "w", encoding="ascii", newline="\n") as stream:
        stream.write("\n".join(lines) + "\n")
    return count


def measure(command):
    start = time.perf_counter()
    result = subprocess.run(command, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE)
    elapsed = time.perf_counter() - start
    if result.returncode != 0:
        sys.stderr.write(result.stderr.decode("utf-8", "replace"))
        raise SystemExit(f"command failed ({result.returncode}): {command}")
    return elapsed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--whippet", default=os.path.join("build", "whippet.exe"))
    parser.add_argument("--instructions", type=int, default=20000)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--workdir", default=os.path.join("build", "bench"))
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    source = os.path.join(args.workdir, "bench.wir")
    text_output = os.path.join(args.workdir, "bench.s")
    object_output = os.path.join(args.workdir, "bench.obj")
    count = generate(source, args.instructions)

    cases = [
        ("asm (-S)", [args.whippet, "-S", "-o", text_output, source],
         text_output),
        ("obj (-c, O1+COFF)", [args.whippet, "-c", "-o", object_output, source],
         object_output),
    ]
    print(f"whippet: {args.whippet}")
    print(f"instructions: {count}, repeat: {args.repeat}")
    for label, command, output in cases:
        samples = [measure(command) for _ in range(args.repeat)]
        best = min(samples)
        median = statistics.median(samples)
        size = os.path.getsize(output) if os.path.exists(output) else 0
        print(f"{label:20s} best {best * 1000:7.1f} ms  "
              f"median {median * 1000:7.1f} ms  "
              f"{count / best:8.0f} lines/s  {size} bytes")

    if not args.keep:
        for path in (source, text_output, object_output):
            try:
                os.remove(path)
            except OSError:
                pass


if __name__ == "__main__":
    main()
