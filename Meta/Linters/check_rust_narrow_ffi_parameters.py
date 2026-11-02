#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

"""Keeps u8 and u16 parameters out of the functions C++ calls in Rust.

rustc takes the bits above a narrow unsigned parameter to be zero and may read them, as clang does for what it compiles.
The x86-64 ABI leaves them undefined, and GCC 16 sign-extends such an argument: a u16 of 32768 or above arrives as a
negative 32-bit value. Take a u32, which C++ widens by the value, and narrow it in Rust.

ALLOWED lists the parameters that stand, by file, each of which carries values below the sign bit, such as a property id
or an enum's discriminant. A file holding fewer than listed fails too, so that the list only shrinks.

The check is a heuristic, not a parser: it sees neither type aliases nor enums and structs passed by value.
"""

import pathlib
import re
import subprocess
import sys

FUNCTION = re.compile(r"extern\s+\"C\"\s+fn\s+(\w+)\s*(?:<[^>]*>)?\s*\(")
NARROW_TYPE = re.compile(r"^(?:(?:core|std)::ffi::)?(?:u8|u16|c_uchar|c_ushort)$")

ALLOWED = {
    "Libraries/LibGfx/Rust/src/font_catalog.rs": 3,
    "Libraries/LibGfx/Rust/src/yuv.rs": 1,
    "Libraries/LibJS/Rust/src/embedding/array_buffer.rs": 1,
    "Libraries/LibJS/Rust/src/embedding/debugger.rs": 3,
    "Libraries/LibJS/Rust/src/embedding/environment.rs": 1,
    "Libraries/LibJS/Rust/src/embedding/function.rs": 1,
    "Libraries/LibJS/Rust/src/embedding/host/host_array.rs": 1,
    "Libraries/LibJS/Rust/src/embedding/object.rs": 13,
    "Libraries/LibJS/Rust/src/embedding/typed_array.rs": 4,
    "Libraries/LibJS/Rust/src/embedding/vm.rs": 2,
    "Libraries/LibJS/Rust/src/unicode/intl.rs": 1,
    "Libraries/LibJS/Rust/src/unicode/number_format.rs": 1,
    "Libraries/LibUnicode/Rust/src/calendar.rs": 7,
    "Libraries/LibWeb/Bindings/Rust/src/html/structured_serialize/exports.rs": 1,
    "Libraries/LibWeb/Bindings/Rust/src/web_assembly/gc_object.rs": 2,
    "Libraries/LibWeb/HTML/Parser/Rust/src/lib.rs": 1,
    "Libraries/LibWeb/Rust/src/css/animated_overlay.rs": 3,
    "Libraries/LibWeb/Rust/src/css/animation.rs": 2,
    "Libraries/LibWeb/Rust/src/css/calc.rs": 7,
    "Libraries/LibWeb/Rust/src/css/color_resolution.rs": 1,
    "Libraries/LibWeb/Rust/src/css/declaration_block.rs": 1,
    "Libraries/LibWeb/Rust/src/css/descriptor_block.rs": 2,
    "Libraries/LibWeb/Rust/src/css/parser/descriptor_parser.rs": 1,
    "Libraries/LibWeb/Rust/src/css/parser/value_parser.rs": 2,
    "Libraries/LibWeb/Rust/src/css/property_metadata.rs": 4,
    "Libraries/LibWeb/Rust/src/css/selector_operations.rs": 1,
    "Libraries/LibWeb/Rust/src/css/serialize.rs": 1,
    "Libraries/LibWeb/Rust/src/css/style/engine_calls.rs": 3,
    "Libraries/LibWeb/Rust/src/css/style_compute.rs": 8,
    "Libraries/LibWeb/Rust/src/css/style_value.rs": 22,
    "Libraries/LibWeb/Rust/src/gfx_test_stubs.rs": 3,
    "Libraries/LibWeb/Rust/src/layout/layout_changes.rs": 1,
    "Libraries/LibWeb/Rust/src/layout/layout_node_arena.rs": 3,
    "Libraries/LibWeb/Rust/src/layout/script_entries.rs": 2,
    "Libraries/LibWeb/Rust/src/layout/shell_reads.rs": 1,
    "Libraries/LibWeb/Rust/src/layout/text_transform.rs": 1,
    "Libraries/LibWeb/Rust/src/layout/tree_update_marks.rs": 2,
    "Libraries/LibWeb/Rust/src/painting/ffi/main_thread_entries.rs": 3,
    "Libraries/LibWeb/Rust/src/painting/visual_animation_builder.rs": 1,
    "Libraries/LibWeb/Rust/src/render_state/clock.rs": 1,
    "Tests/LibWebView/RustCrashReport.rs": 1,
}


def split_parameters(source, start):
    """The parameters of the function whose list begins at `start`, split at commas outside any brackets."""
    parameters = []
    current = []
    depth = 1
    for character in source[start:]:
        if character in "([{<":
            depth += 1
        elif character in ")]}":
            depth -= 1
            if depth == 0:
                parameters.append("".join(current))
                return parameters
        elif character == ">" and current and current[-1] != "-":
            depth -= 1
        if character == "," and depth == 1:
            parameters.append("".join(current))
            current = []
        else:
            current.append(character)
    return parameters


def narrow_parameters(source):
    for function in FUNCTION.finditer(source):
        for parameter in split_parameters(source, function.end()):
            name, separator, parameter_type = parameter.partition(":")
            if separator and NARROW_TYPE.match(parameter_type.strip()):
                yield function.group(1), name.strip()


def main():
    files = subprocess.run(["git", "ls-files", "--", "*.rs"], capture_output=True, text=True).stdout.split()
    failures = []
    for file in files:
        found = list(narrow_parameters(pathlib.Path(file).read_text()))
        allowed = ALLOWED.get(file, 0)
        if len(found) > allowed:
            failures.extend(f"{file}: {function}({name})" for function, name in found)
        elif len(found) < allowed:
            failures.append(f"{file}: holds {len(found)} narrow parameters, lower its allowance of {allowed}")
    for failure in failures:
        print(f"narrow FFI parameter, take a u32: {failure}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
