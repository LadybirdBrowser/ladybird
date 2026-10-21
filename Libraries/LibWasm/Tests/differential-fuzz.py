#!/usr/bin/env python3
# Differential fuzzing of the WebAssembly engine against other ones: `wasm-tools smith` generates valid modules, to
# which this script adds, for each exported function, a probe that calls the function with zero arguments and returns
# a hash of its results (floats as their bits, references as whether they are null) and of memory 0. Each engine runs
# each probe; whether it traps and the hash it returns must agree. When the engine under test and the old engine
# disagree, wabt's interpreter (wasm-interp) decides which one is wrong.
#
#   differential-fuzz.py --new Build/release/bin/wasm --old /path/to/old/wasm --cases 500 --out /tmp/fuzz
#
# Each case that the engine under test gets wrong is kept in the output directory with what the engines returned.
# Re-run a single case with --seed. Cases are reproducible from their seed.

import argparse
import concurrent.futures
import os
import random
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from reencode import DecodeError  # noqa: E402
from reencode import Reader  # noqa: E402
from reencode import unsigned_leb  # noqa: E402

SMITH_FEATURES = [
    "--ensure-termination",
    "--export-everything",
    "true",
    "--canonicalize-nans",
    "true",
    "--max-imports",
    "0",
    "--min-funcs",
    "1",
    "--max-funcs",
    "8",
    "--max-memory32-bytes",
    str(16 * 65536),
    "--max-memories",
    "1",
    "--max-tables",
    "2",
    "--max-table-elements",
    "100",
    "--max-data-segments",
    "4",
    "--simd-enabled",
    "true",
    "--bulk-memory-enabled",
    "true",
    "--reference-types-enabled",
    "true",
    "--multi-value-enabled",
    "true",
    "--saturating-float-to-int-enabled",
    "true",
    "--sign-extension-ops-enabled",
    "true",
    "--tail-call-enabled",
    "true",
    "--relaxed-simd-enabled",
    "false",
    "--gc-enabled",
    "false",
    "--exceptions-enabled",
    "false",
    "--threads-enabled",
    "false",
    "--memory64-enabled",
    "false",
    "--extended-const-enabled",
    "true",
    "--allow-start-export",
    "false",
]

VALUE_TYPE_ZERO = {
    0x7F: bytes([0x41, 0x00]),
    0x7E: bytes([0x42, 0x00]),
    0x7D: bytes([0x43, 0, 0, 0, 0]),
    0x7C: bytes([0x44, 0, 0, 0, 0, 0, 0, 0, 0]),
    0x7B: bytes([0xFD, 0x0C]) + bytes(16),
    0x70: bytes([0xD0, 0x70]),
    0x6F: bytes([0xD0, 0x6F]),
}


def sections_of(data):
    reader = Reader(data, 8)
    sections = []
    while reader.position < reader.end:
        section_id = reader.byte()
        size = reader.unsigned(32)
        sections.append([section_id, bytearray(reader.bytes(size))])
    return sections


def vector_of(contents):
    reader = Reader(bytes(contents))
    return reader, reader.unsigned(32)


def add_probes(data):
    """Add, for each exported function, an exported `__probe_<n>` that calls it with zero arguments, drops its
    results, and returns a hash of memory 0. Returns the new module and the names of the functions probed."""
    sections = sections_of(data)
    by_id = {section_id: contents for section_id, contents in sections}
    if 2 in by_id and vector_of(by_id[2])[1] > 0:
        raise DecodeError("imports are not probed")
    types = []
    reader, count = vector_of(by_id.get(1, b"\0"))
    for _ in range(count):
        if reader.byte() != 0x60:
            raise DecodeError("only function types are probed")
        parameters = [reader.byte() for _ in range(reader.unsigned(32))]
        results = [reader.byte() for _ in range(reader.unsigned(32))]
        if any(t not in VALUE_TYPE_ZERO for t in parameters + results):
            raise DecodeError("unprobed value type")
        types.append((parameters, results))
    reader, count = vector_of(by_id.get(3, b"\0"))
    functions = [reader.unsigned(32) for _ in range(count)]
    reader, count = vector_of(by_id.get(7, b"\0"))
    exports = []
    for _ in range(count):
        name = reader.bytes(reader.unsigned(32)).decode()
        kind = reader.byte()
        index = reader.unsigned(32)
        if kind == 0:
            exports.append((name, index))
    has_memory = 5 in by_id and vector_of(by_id[5])[1] > 0

    probe_type = len(types)
    new_types = bytearray(by_id.get(1, b"\0"))
    reader, count = vector_of(new_types)
    new_types = bytearray(unsigned_leb(count + 1)) + new_types[reader.position :] + bytes([0x60, 0x00, 0x01, 0x7E])

    # Locals: $i (i32), $hash (i64), $vector (v128).
    fold = bytes([0x20, 0x01, 0x42, 0x1F, 0x7E, 0x7C, 0x21, 0x01])
    folds = {
        0x7F: bytes([0xAD]) + fold,
        0x7E: fold,
        0x7D: bytes([0xBC, 0xAD]) + fold,
        0x7C: bytes([0xBD]) + fold,
        0x7B: bytes([0x21, 0x02, 0x20, 0x02, 0xFD, 0x1D, 0x00]) + fold + bytes([0x20, 0x02, 0xFD, 0x1D, 0x01]) + fold,
        0x70: bytes([0xD1, 0xAD]) + fold,
        0x6F: bytes([0xD1, 0xAD]) + fold,
    }
    bodies = []
    for _, function_index in exports:
        parameters, results = types[functions[function_index]]
        code = bytearray()
        for parameter in parameters:
            code += VALUE_TYPE_ZERO[parameter]
        code += bytes([0x10]) + unsigned_leb(function_index)
        # The last result is on top.
        for result in reversed(results):
            code += folds[result]
        if has_memory:
            # block loop
            #   i >= memory.size << 16 -> exit
            #   hash = hash * 31 + i64.load(i); i += 8
            # end end
            code += bytes([0x02, 0x40, 0x03, 0x40])
            code += bytes([0x20, 0x00, 0x3F, 0x00, 0x41, 0x10, 0x74, 0x4F, 0x0D, 0x01])
            code += bytes([0x20, 0x00, 0x29, 0x03, 0x00]) + fold
            code += bytes([0x20, 0x00, 0x41, 0x08, 0x6A, 0x21, 0x00, 0x0C, 0x00])
            code += bytes([0x0B, 0x0B])
        code += bytes([0x20, 0x01, 0x0B])
        body = bytes([0x03, 0x01, 0x7F, 0x01, 0x7E, 0x01, 0x7B]) + bytes(code)
        bodies.append(unsigned_leb(len(body)) + body)

    reader, count = vector_of(by_id.get(3, b"\0"))
    new_functions = bytearray(unsigned_leb(count + len(exports))) + by_id.get(3, b"\0")[reader.position :]
    new_functions += unsigned_leb(probe_type) * len(exports)
    reader, count = vector_of(by_id.get(10, b"\0"))
    new_code = bytearray(unsigned_leb(count + len(exports))) + by_id.get(10, b"\0")[reader.position :]
    for body in bodies:
        new_code += body
    reader, count = vector_of(by_id.get(7, b"\0"))
    # Every function gets a plain name as well, since smith's names may not survive a command line.
    new_exports = bytearray(unsigned_leb(count + 2 * len(exports))) + by_id.get(7, b"\0")[reader.position :]
    for probe, (_, function_index) in enumerate(exports):
        name = f"__function_{probe}".encode()
        new_exports += unsigned_leb(len(name)) + name + bytes([0x00]) + unsigned_leb(function_index)
        name = f"__probe_{probe}".encode()
        new_exports += unsigned_leb(len(name)) + name + bytes([0x00]) + unsigned_leb(len(functions) + probe)

    replaced = {1: new_types, 3: new_functions, 7: new_exports, 10: new_code}
    present = {section_id for section_id, _ in sections}
    order = [1, 2, 3, 4, 5, 13, 6, 7, 8, 9, 12, 10, 11]
    out = bytearray(data[:8])
    for section_id in order:
        if section_id in replaced:
            contents = replaced[section_id]
        elif section_id in present:
            contents = by_id[section_id]
        else:
            continue
        out += bytes([section_id]) + unsigned_leb(len(contents)) + contents
    return bytes(out), [name for name, _ in exports]


def outcome_of(process):
    if process.returncode < 0:
        return f"crash {process.returncode}"
    output = process.stdout + process.stderr
    if "trapped" in output or ") => error" in output:
        return "trap"
    if "failed" in output.lower() or "error:" in output:
        return "failed"
    match = re.search(r"(?:  -> |=> i64:)(-?[0-9]+)", output)
    if not match:
        return "failed"
    # Engines print the hash signed or unsigned.
    return f"returned {int(match[1]) % (1 << 64)}"


def run(binary, module, function):
    try:
        process = subprocess.run([binary, "-e", function, module], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return "timeout"
    return outcome_of(process)


def run_wabt(module, function):
    try:
        process = subprocess.run(
            ["wasm-interp", "--enable-all", module, "-r", function], capture_output=True, text=True, timeout=60
        )
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return "unavailable"
    return outcome_of(process)


def run_case(seed, arguments):
    random.seed(seed)
    entropy = bytes(random.getrandbits(8) for _ in range(arguments.entropy))
    smith = subprocess.run(
        [arguments.wasm_tools, "smith", *SMITH_FEATURES, *arguments.smith_argument],
        input=entropy,
        capture_output=True,
        timeout=60,
    )
    if smith.returncode != 0:
        return seed, "skipped", "smith failed"
    try:
        module, names = add_probes(smith.stdout)
    except (DecodeError, IndexError, UnicodeDecodeError) as error:
        return seed, "skipped", str(error)
    path = os.path.join(arguments.out, f"case-{seed}.wasm")
    with open(path, "wb") as file:
        file.write(module)
    differences = []
    verdict = "same"
    for probe in range(len(names)):
        function = f"__probe_{probe}"
        new = run(arguments.new, path, function)
        old = run(arguments.old, path, function)
        if new == old:
            continue
        wabt = run_wabt(path, function)
        if old == "failed" and wabt == "failed" and new != "failed":
            # Neither reference engine can run the module, which is valid (smith only makes valid ones).
            if verdict == "same":
                verdict = "references-fail"
            continue
        if wabt == new:
            # The old engine is wrong, or cannot run the module (smith only makes valid ones).
            if verdict == "same":
                verdict = "old-wrong"
            continue
        verdict = "different"
        differences.append(f"{function}: new {new!r}, old {old!r}, wabt {wabt!r}")
    if differences:
        with open(os.path.join(arguments.out, f"case-{seed}.txt"), "w") as file:
            file.write("\n".join(differences) + "\n")
        return seed, "different", "; ".join(differences[:3])
    os.remove(path)
    return seed, verdict, f"{len(names)} functions"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--new", required=True)
    parser.add_argument("--old", required=True)
    parser.add_argument("--wasm-tools", default=os.path.expanduser("~/.cargo/bin/wasm-tools"))
    parser.add_argument("--cases", type=int, default=200)
    parser.add_argument("--first-seed", type=int, default=0)
    parser.add_argument("--seed", type=int)
    parser.add_argument("--entropy", type=int, default=4096)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--out", required=True)
    parser.add_argument(
        "--smith-argument", action="append", default=[], help="More arguments for smith, like --min-uleb-size=5"
    )
    arguments = parser.parse_args()
    os.makedirs(arguments.out, exist_ok=True)
    seeds = (
        [arguments.seed]
        if arguments.seed is not None
        else range(arguments.first_seed, arguments.first_seed + arguments.cases)
    )
    counts = {"same": 0, "different": 0, "skipped": 0, "old-wrong": 0, "references-fail": 0}
    with concurrent.futures.ThreadPoolExecutor(max_workers=arguments.jobs) as pool:
        for seed, verdict, detail in pool.map(lambda seed: run_case(seed, arguments), seeds):
            counts[verdict] += 1
            if verdict == "different":
                print(f"seed {seed}: {verdict}: {detail}", flush=True)
    print(
        f"{counts['same']} same, {counts['different']} different, {counts['skipped']} skipped, "
        f"{counts['old-wrong']} where only the old engine is wrong (by wabt), "
        f"{counts['references-fail']} that neither the old engine nor wabt can run"
    )


if __name__ == "__main__":
    main()
