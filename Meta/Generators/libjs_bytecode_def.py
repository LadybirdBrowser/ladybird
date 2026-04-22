#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass
from dataclasses import field
from typing import List
from typing import Optional


@dataclass
class Field:
    name: str
    type: str
    is_array: bool = False


@dataclass
class OpDef:
    name: str
    base: str
    fields: List[Field] = field(default_factory=list)
    is_terminator: bool = False  # @terminator
    is_nothrow: bool = False  # @nothrow


def parse_bytecode_def(path: str) -> List[OpDef]:
    with open(path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    ops: List[OpDef] = []
    current: Optional[OpDef] = None
    in_op = False

    for raw_line in lines:
        stripped = raw_line.strip()
        if not stripped:
            continue
        if stripped.startswith("//") or stripped.startswith("#"):
            continue

        if stripped.startswith("op "):
            if in_op:
                raise RuntimeError("Nested op blocks are not allowed")
            in_op = True

            rest = stripped[len("op ") :].strip()
            if "<" in rest:
                name_part, base_part = rest.split("<", 1)
                name = name_part.strip()
                base = base_part.strip()
            else:
                name = rest.strip()
                base = "Instruction"

            current = OpDef(name=name, base=base)
            continue

        if stripped == "endop":
            if not in_op or current is None:
                raise RuntimeError("endop without corresponding op")
            ops.append(current)
            current = None
            in_op = False
            continue

        if not in_op:
            continue

        if stripped.startswith("@"):
            if stripped == "@terminator":
                current.is_terminator = True
            elif stripped == "@nothrow":
                current.is_nothrow = True
            continue

        if ":" not in stripped:
            raise RuntimeError(f"Malformed field line: {stripped!r}")
        lhs, rhs = stripped.split(":", 1)
        field_name = lhs.strip()
        field_type = rhs.strip()
        is_array = False
        if field_type.endswith("[]"):
            is_array = True
            field_type = field_type[:-2].strip()
        current.fields.append(Field(name=field_name, type=field_type, is_array=is_array))

    if in_op or current is not None:
        raise RuntimeError("Unclosed op block at end of file")

    return ops


__all__ = [
    "Field",
    "OpDef",
    "parse_bytecode_def",
]
