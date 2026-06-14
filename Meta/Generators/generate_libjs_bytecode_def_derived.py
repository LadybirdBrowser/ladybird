#!/usr/bin/env python3
from __future__ import annotations

import sys

from typing import List
from typing import Optional

from libjs_bytecode_def import Field
from libjs_bytecode_def import OpDef
from libjs_bytecode_def import parse_bytecode_def


def mname_to_param(name: str) -> str:
    if name.startswith("m_") and len(name) > 2:
        return name[2:]
    return name


def getter_name_for_field(field_name: str) -> str:
    return mname_to_param(field_name)


def is_operand_type(t: str) -> bool:
    t = t.strip()
    return t == "Operand" or t == "Optional<Operand>"


def is_optional_operand_type(t: str) -> bool:
    return t.strip() == "Optional<Operand>"


def is_label_type(t: str) -> bool:
    t = t.strip()
    return t == "Label" or t == "Optional<Label>"


def is_optional_label_type(t: str) -> bool:
    return t.strip() == "Optional<Label>"


def find_count_field_name(op: OpDef, array_field: Field) -> Optional[str]:
    """
    Heuristic: look for a u32/size_t field matching
      - <name>_count
      - if name ends with 's', also <name_without_s>_count
    """
    candidates = [f"{array_field.name}_count"]

    if array_field.name.endswith("s"):
        base = array_field.name[:-1]
        candidates.append(f"{base}_count")

    for cand in candidates:
        for f in op.fields:
            if f.name == cand and not f.is_array and f.type.strip() in ("u32", "size_t"):
                return cand
    return None


def get_count_field_name_or_die(op: OpDef, array_field: Field) -> str:
    name = find_count_field_name(op, array_field)
    if name is None:
        raise RuntimeError(f"No count field (u32/size_t) found for array field '{array_field.name}' in op '{op.name}'")
    return name


def generate_enum_macro(ops: List[OpDef]) -> str:
    filtered = [op for op in ops if op.name != "Instruction"]
    lines = []
    lines.append("#define ENUMERATE_BYTECODE_OPS(O) \\")
    for i, op in enumerate(filtered):
        last = i == len(filtered) - 1
        if last:
            lines.append(f"    O({op.name})")
        else:
            lines.append(f"    O({op.name}) \\")
    return "\n".join(lines)


def generate_getters(op: OpDef) -> List[str]:
    lines: List[str] = []
    for f in op.fields:
        gname = getter_name_for_field(f.name)
        if f.is_array:
            count_name = get_count_field_name_or_die(op, f)
            rettype = f"ReadonlySpan<{f.type}>"
            lines.append(
                f"    {rettype} {gname}() const {{ return ReadonlySpan<{f.type}> {{ {f.name}, {count_name} }}; }}"
            )
        else:
            lines.append(f"    auto const& {gname}() const {{ return {f.name}; }}")
    return lines


def generate_class(op: OpDef) -> str:
    if op.name == "Instruction":
        return ""

    base = op.base or "Instruction"
    lines: List[str] = []

    lines.append(f"class {op.name} final : public {base} {{")
    lines.append("public:")

    arrays: List[Field] = [f for f in op.fields if f.is_array]
    has_array = len(arrays) > 0
    has_m_length = any(f.name == "m_length" for f in op.fields)

    if has_array:
        lines.append("    static constexpr bool IsVariableLength = true;")
    if has_m_length:
        lines.append("    size_t length_impl() const { return m_length; }")
    if op.is_terminator:
        lines.append("    static constexpr bool IsTerminator = true;")

    # Map arrays -> count fields (error if missing)
    array_to_count: dict[str, str] = {}
    count_to_array_param: dict[str, str] = {}
    for af in arrays:
        count_name = get_count_field_name_or_die(op, af)
        array_to_count[af.name] = count_name
        count_to_array_param[count_name] = mname_to_param(af.name)

    count_fields = set(count_to_array_param.keys())

    # ctor params: scalars first, then spans
    ctor_params: List[str] = []
    for f in op.fields:
        if f.is_array:
            continue
        if f.type.strip() == "EnvironmentCoordinate":
            continue
        if f.name in count_fields:
            continue
        if f.name == "m_length":
            # synthesized, don't take as parameter
            continue
        ctor_params.append(f"{cpp_type_for_field(f.type)} {mname_to_param(f.name)}")

    span_param_for_array: dict[str, str] = {}
    for af in arrays:
        span_param = mname_to_param(af.name)
        span_param_for_array[af.name] = span_param

        span_elem_type = af.type

        ctor_params.append(f"ReadonlySpan<{span_elem_type}> {span_param}")

    if ctor_params:
        lines.append(f"    {op.name}({', '.join(ctor_params)})")
    else:
        lines.append(f"    {op.name}()")

    # initializer list
    init_entries: List[str] = [f"{base}(Type::{op.name})"]
    for f in op.fields:
        if f.is_array:
            continue
        if f.type.strip() == "EnvironmentCoordinate":
            continue
        if f.name in count_fields:
            span_param = count_to_array_param[f.name]
            init_entries.append(f"{f.name}({span_param}.size())")
            continue
        if f.name == "m_length":
            # choose the first array field as the one that defines the tail
            array_for_length: Optional[Field] = arrays[0] if arrays else None
            if array_for_length is not None:
                span_param = span_param_for_array.get(array_for_length.name)
                elem_t = array_for_length.type.strip()
                if span_param is not None:
                    init_entries.append(
                        "m_length(round_up_to_power_of_two("
                        "alignof(void*), sizeof(*this) + sizeof("
                        f"{elem_t}) * {span_param}.size()))"
                    )
                else:
                    init_entries.append("m_length(0)")
            else:
                init_entries.append("m_length(0)")
            continue

        init_entries.append(f"{f.name}({mname_to_param(f.name)})")

    if init_entries:
        lines.append(f"        : {init_entries[0]}")
        for entry in init_entries[1:]:
            lines.append(f"        , {entry}")
    lines.append("    {")
    # copy spans into trailing arrays
    for af in arrays:
        span_param = span_param_for_array[af.name]
        count_name = array_to_count[af.name]
        elem_t = af.type.strip()
        if elem_t == "Optional<Operand>":
            lines.append(f"        for (size_t i = 0; i < {span_param}.size(); ++i) {{")
            lines.append(f"            if ({span_param}[i].has_value())")
            lines.append(f"                {af.name}[i] = {span_param}[i].value();")
            lines.append("            else")
            lines.append(f"                {af.name}[i] = {{}};")
            lines.append("        }")
        else:
            lines.append(f"        for (size_t i = 0; i < {span_param}.size(); ++i)")
            lines.append(f"            {af.name}[i] = {span_param}[i];")
    lines.append("    }")
    lines.append("")

    getters = generate_getters(op)
    if getters:
        lines.append("")
        lines.extend(getters)

    lines.append("")
    lines.append("private:")
    for f in op.fields:
        cpp_t = cpp_type_for_field(f.type)
        if f.is_array:
            lines.append(f"    {cpp_t} {f.name}[];")
        else:
            lines.append(f"    {cpp_t} {f.name};")

    lines.append("};")
    lines.append(f"static_assert(IsTriviallyDestructible<{op.name}>);")
    return "\n".join(lines)


def generate_op_namespace_body(ops: List[OpDef]) -> str:
    lines: List[str] = []
    lines.append("namespace JS::Bytecode::Op {")
    lines.append("")
    for op in ops:
        cls = generate_class(op)
        if cls:
            lines.append(cls)
            lines.append("")
    lines.append("} // namespace JS::Bytecode::Op")
    return "\n".join(lines)


CACHE_INDEX_TYPES = {
    "PropertyLookupCacheIndex",
    "GlobalVariableCacheIndex",
    "EnvironmentCoordinateCacheIndex",
    "TemplateObjectCacheIndex",
    "ObjectShapeCacheIndex",
    "ObjectPropertyIteratorCacheIndex",
}


def is_cache_index_type(t: str) -> bool:
    return t.strip() in CACHE_INDEX_TYPES


def cpp_type_for_field(t: str) -> str:
    """Return the C++ storage type for a Bytecode.def field type."""
    if is_cache_index_type(t):
        return "u32"
    return t


def generate_op_cpp_body(ops: List[OpDef]) -> str:
    lines: List[str] = []
    lines.append("#include <LibJS/Bytecode/Op.h>")
    lines.append("")
    return "\n".join(lines)


def generate_opcodes_h(ops: List[OpDef]) -> str:
    macro = generate_enum_macro(ops)
    lines: List[str] = []
    lines.append("#pragma once")
    lines.append("")
    lines.append(macro)
    lines.append("")
    return "\n".join(lines)


def generate_op_h(ops: List[OpDef]) -> str:
    includes = """#pragma once

#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <LibJS/Bytecode/Builtins.h>
#include <LibJS/Bytecode/IdentifierTable.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/Label.h>
#include <LibJS/Bytecode/Operand.h>
#include <LibJS/Bytecode/PutKind.h>
#include <LibJS/Bytecode/RegexTable.h>
#include <LibJS/Bytecode/Register.h>
#include <LibJS/Bytecode/StringTable.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Value.h>
"""
    body = generate_op_namespace_body(ops)
    return includes + "\n" + body


def usage(prog: str) -> None:
    print(
        f"Usage: {prog} -c path/to/Op.cpp -h path/to/Op.h -x path/to/OpCodes.h -i path/to/Bytecode.def",
        file=sys.stderr,
    )


def main(argv: List[str]) -> None:
    c_path = None
    h_path = None
    x_path = None
    def_path = None

    i = 1
    while i < len(argv):
        arg = argv[i]
        if arg == "-c" and i + 1 < len(argv):
            c_path = argv[i + 1]
            i += 2
        elif arg == "-h" and i + 1 < len(argv):
            h_path = argv[i + 1]
            i += 2
        elif arg == "-x" and i + 1 < len(argv):
            x_path = argv[i + 1]
            i += 2
        elif arg == "-i" and i + 1 < len(argv):
            def_path = argv[i + 1]
            i += 2
        else:
            usage(argv[0])
            sys.exit(1)

    if not (c_path and h_path and x_path and def_path):
        usage(argv[0])
        sys.exit(1)

    ops = parse_bytecode_def(def_path)

    op_h = generate_op_h(ops)
    op_cpp = generate_op_cpp_body(ops)
    opcodes_h = generate_opcodes_h(ops)

    with open(h_path, "w", encoding="utf-8") as f:
        f.write(op_h)

    with open(c_path, "w", encoding="utf-8") as f:
        f.write(op_cpp)

    with open(x_path, "w", encoding="utf-8") as f:
        f.write(opcodes_h)


if __name__ == "__main__":
    main(sys.argv)
