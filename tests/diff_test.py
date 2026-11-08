#!/usr/bin/env python3
# Author: dhjs0000
#
# M1 differential test: generate random integer Whippet IR functions and an
# equivalent C reference, compile the IR through whippet, compile both with
# clang, and compare results for a fixed set of edge-case inputs.
#
# The generated C uses unsigned casts for wrapping add/sub/mul/neg/shl and
# explicit unsigned shifts, so its behaviour is defined and matches Whippet's
# modular integer semantics.

import argparse
import pathlib
import random
import subprocess
import sys

BINARY_OPS = {
    "add": "(int32_t)((uint32_t){a} + (uint32_t){b})",
    "sub": "(int32_t)((uint32_t){a} - (uint32_t){b})",
    "mul": "(int32_t)((uint32_t){a} * (uint32_t){b})",
    "and": "(int32_t)({a} & {b})",
    "or": "(int32_t)({a} | {b})",
    "xor": "(int32_t)({a} ^ {b})",
}

ICMP_OPS = {
    "eq": "{a} == {b}",
    "ne": "{a} != {b}",
    "slt": "{a} < {b}",
    "sle": "{a} <= {b}",
    "sgt": "{a} > {b}",
    "sge": "{a} >= {b}",
    "ult": "(uint32_t){a} < (uint32_t){b}",
    "ule": "(uint32_t){a} <= (uint32_t){b}",
    "ugt": "(uint32_t){a} > (uint32_t){b}",
    "uge": "(uint32_t){a} >= (uint32_t){b}",
}


def pick_operand(rng, values):
    if rng.random() < 0.68:
        return rng.choice(values)
    constant = rng.randint(-100, 100)
    return (str(constant), str(constant))


def generate_function(index, rng, op_count):
    values = [("%a", "a"), ("%b", "b"), ("%c", "c")]
    ir_lines = []
    c_lines = []

    for i in range(op_count):
        name = f"%v{i}"
        cname = f"v{i}"
        choice = rng.random()

        if choice < 0.50:
            op = rng.choice(list(BINARY_OPS))
            left = pick_operand(rng, values)
            right = pick_operand(rng, values)
            ir_lines.append(f"  {name} = {op} i32 {left[0]}, {right[0]}")
            expression = BINARY_OPS[op].format(a=left[1], b=right[1])
            c_lines.append(f"    int32_t {cname} = {expression};")

        elif choice < 0.65:
            op, mnemonic = rng.choice(
                [("sdiv", "/"), ("srem", "%"), ("udiv", "/"), ("urem", "%")]
            )
            left = pick_operand(rng, values)
            divisor = rng.choice([2, 3, 5, 7])
            ir_lines.append(f"  {name} = {op} i32 {left[0]}, {divisor}")
            if op in ("udiv", "urem"):
                c_expr = f"(int32_t)((uint32_t){left[1]} {mnemonic} {divisor}u)"
            else:
                c_expr = f"(int32_t)({left[1]} {mnemonic} {divisor})"
            c_lines.append(f"    int32_t {cname} = {c_expr};")

        elif choice < 0.78:
            op, c_template = rng.choice(
                [
                    ("shl", "(int32_t)((uint32_t){a} << {k})"),
                    ("lshr", "(int32_t)((uint32_t){a} >> {k})"),
                    ("ashr", "(int32_t)({a} >> {k})"),
                ]
            )
            left = pick_operand(rng, values)
            amount = rng.randint(0, 7)
            ir_lines.append(f"  {name} = {op} i32 {left[0]}, {amount}")
            c_lines.append(
                f"    int32_t {cname} = "
                + c_template.format(a=left[1], k=amount)
                + ";"
            )

        elif choice < 0.86:
            op = rng.choice(["neg", "not"])
            left = pick_operand(rng, values)
            ir_lines.append(f"  {name} = {op} i32 {left[0]}")
            if op == "neg":
                c_lines.append(
                    f"    int32_t {cname} = (int32_t)(0u - (uint32_t){left[1]});"
                )
            else:
                c_lines.append(f"    int32_t {cname} = (int32_t)(~{left[1]});")

        elif choice < 0.94:
            predicate = rng.choice(list(ICMP_OPS))
            left = pick_operand(rng, values)
            right = pick_operand(rng, values)
            ir_lines.append(
                f"  {name} = icmp {predicate} i32 {left[0]}, {right[0]}"
            )
            expression = ICMP_OPS[predicate].format(a=left[1], b=right[1])
            c_lines.append(f"    int32_t {cname} = ({expression});")

        else:
            condition = rng.choice(values)
            left = pick_operand(rng, values)
            right = pick_operand(rng, values)
            ir_lines.append(
                f"  {name} = select i32 {condition[0]}, i32 {left[0]}, "
                f"i32 {right[0]}"
            )
            c_lines.append(
                f"    int32_t {cname} = {condition[1]} ? {left[1]} : {right[1]};"
            )

        values.append((name, cname))

    result = values[-1][1]
    ir = (
        f"func @f_{index}(i32 %a, i32 %b, i32 %c) -> i32 {{\n"
        "entry:\n"
        + "\n".join(ir_lines)
        + f"\n  ret i32 {values[-1][0]}\n"
        "}\n"
    )
    c = (
        f"static int32_t ref_{index}(int32_t a, int32_t b, int32_t c) {{\n"
        + "\n".join(c_lines)
        + f"\n    return {result};\n"
        "}\n"
    )
    return ir, c


def make_inputs(rng):
    inputs = [
        (0, 0, 0),
        (1, 2, 3),
        (-1, 2, -3),
        (2147483647, 1, -1),
        (-2147483648, -2, 7),
        (12345, -6789, 42),
    ]
    for _ in range(8):
        inputs.append(
            (
                rng.randint(-1000000000, 1000000000),
                rng.randint(-1000000000, 1000000000),
                rng.randint(-1000000000, 1000000000),
            )
        )
    return inputs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--whippet", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--workdir", required=True)
    parser.add_argument("--functions", type=int, default=40)
    parser.add_argument("--ops", type=int, default=12)
    parser.add_argument("--seed", type=int, default=20261108)
    args = parser.parse_args()

    rng = random.Random(args.seed)
    workdir = pathlib.Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    wir_path = workdir / "diff.wir"
    c_path = workdir / "diff_reference.c"
    asm_path = workdir / "diff.s"
    exe_path = workdir / "diff.exe"

    ir_functions = []
    c_functions = []
    for i in range(args.functions):
        ir, c = generate_function(i, rng, args.ops)
        ir_functions.append(ir)
        c_functions.append(c)

    inputs = make_inputs(rng)
    prototypes = "\n".join(
        f"extern int32_t f_{i}(int32_t, int32_t, int32_t);"
        for i in range(args.functions)
    )
    checks = []
    for i in range(args.functions):
        for a, b, c in inputs:
            checks.append(
                f"    if (f_{i}({a}, {b}, {c}) != "
                f"ref_{i}({a}, {b}, {c})) {{\n"
                f'        printf("mismatch f_{i}(%d,%d,%d)\\n", {a}, {b}, {c});\n'
                "        return 1;\n"
                "    }"
            )

    driver = (
        "#include <stdint.h>\n"
        "#include <stdio.h>\n\n"
        + prototypes
        + "\n\n"
        + "\n".join(c_functions)
        + "\n\nint main(void) {\n"
        + "\n".join(checks)
        + "\n    return 0;\n}\n"
    )

    wir_path.write_text("\n".join(ir_functions), encoding="utf-8")
    c_path.write_text(driver, encoding="utf-8")

    codegen = subprocess.run(
        [args.whippet, "-S", "-o", str(asm_path), str(wir_path)],
        capture_output=True,
        text=True,
    )
    if codegen.returncode != 0:
        print(codegen.stdout)
        print(codegen.stderr, file=sys.stderr)
        return 1

    compile_result = subprocess.run(
        [args.clang, "-fwrapv", str(c_path), str(asm_path), "-o", str(exe_path)],
        capture_output=True,
        text=True,
    )
    if compile_result.returncode != 0:
        print(compile_result.stdout)
        print(compile_result.stderr, file=sys.stderr)
        return 1

    run = subprocess.run([str(exe_path)], capture_output=True, text=True)
    if run.returncode != 0:
        print(run.stdout)
        print(run.stderr, file=sys.stderr)
        return 1

    print(
        f"ok differential: {args.functions} functions x "
        f"{len(inputs)} inputs"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
