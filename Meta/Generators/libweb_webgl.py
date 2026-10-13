# Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
#
# SPDX-License-Identifier: BSD-2-Clause

# Shared model and CLI driver for the WebGL generators: loads GLFunctions.json and
# derives the command-struct shape each annotated GL function serializes to.

import argparse
import json
import sys

from dataclasses import dataclass
from pathlib import Path
from typing import List
from typing import Tuple

sys.path.append(str(Path(__file__).resolve().parent.parent))

from Utils.utils import title_case_to_snake_case as snake_case


def load_functions(path: str) -> list:
    with open(path, "r", encoding="utf-8") as input_file:
        return json.load(input_file)


# All WebGL generators take the same -h/-c/-j arguments and emit one header and one
# implementation file from the loaded function list.
def run_generator(description: str, write_header_file, write_implementation_file) -> None:
    parser = argparse.ArgumentParser(description=description, add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--header", required=True, help="Path to the header file to generate")
    parser.add_argument("-c", "--implementation", required=True, help="Path to the implementation file to generate")
    parser.add_argument("-j", "--json", required=True, help="Path to the JSON file to read from")
    args = parser.parse_args()

    functions = load_functions(args.json)

    with open(args.header, "w", encoding="utf-8") as output_file:
        write_header_file(output_file, functions)

    with open(args.implementation, "w", encoding="utf-8") as output_file:
        write_implementation_file(output_file, functions)


def command_name(function: dict) -> str:
    if function["category"].startswith("builtin"):
        return function["name"]
    assert function["name"].startswith("gl")
    return function["name"][2:]


def method_name(function: dict) -> str:
    return snake_case(command_name(function))


# The GLFunctions and WebGLContextProxy classes must stay signature-identical (the WebGL
# implementation cannot tell them apart), so both generators emit from this one helper.
def method_signature(function: dict, qualifier: str = "") -> str:
    args = ", ".join(f"{arg['type']} {arg['name']}" for arg in function["args"])
    return f"{function['return']} {qualifier}{method_name(function)}({args})"


# Entries carried by the command stream: regular commands and object creation (struct
# shapes derived from the GL signature) plus wire-specified ops (custom-handled GL
# functions and builtins, whose struct shapes are spelled out in the JSON).
def is_wire_command(function: dict) -> bool:
    return "wire_command" in function


def is_wire_sync(function: dict) -> bool:
    return "wire_request" in function


def command_stream_entries(functions: list) -> list:
    return [f for f in functions if f["category"] in ("command", "gen") or is_wire_command(f)]


def sync_call_entries(functions: list) -> list:
    return [f for f in functions if f["category"] == "sync" or is_wire_sync(f)]


def is_pointer(arg: dict) -> bool:
    return arg["type"].endswith("*")


def is_const_pointer(arg: dict) -> bool:
    return is_pointer(arg) and "const" in arg["type"]


def deref_type(pointer_type: str) -> str:
    return pointer_type.replace("*", "").strip()


# Returns the ordered (cpp_type, field_name, arg_or_none) triples of a synchronous
# call's request struct: every non-out argument, with object ids, strings, and input
# payloads in their wire representations.
def sync_request_fields(function: dict) -> list:
    fields = []
    for arg in function["args"]:
        if arg.get("out"):
            continue
        field_name = snake_case(arg["name"])
        if arg.get("object") and not is_pointer(arg):
            fields.append(("WebGLObjectId", field_name, arg))
        elif arg.get("string") or "payload" in arg:
            fields.append(("WebGLDataSpan", field_name, arg))
        else:
            assert not is_pointer(arg), f"unhandled pointer arg {function['name']}.{arg['name']}"
            fields.append((arg["type"], field_name, arg))
    return fields


# Returns the ordered (cpp_type, field_name, arg_or_none) triples of a synchronous
# call's reply struct: the return value, scalar outs by value, buffer outs as spans
# into the reply's inline data.
def sync_reply_fields(function: dict) -> list:
    fields = []
    if function["return"] != "void":
        fields.append((function["return"], "return_value", None))
    for arg in function["args"]:
        if not arg.get("out"):
            continue
        field_name = snake_case(arg["name"])
        if "payload" in arg:
            fields.append(("WebGLDataSpan", field_name, arg))
        else:
            fields.append((deref_type(arg["type"]), field_name, arg))
    return fields


# Returns the ordered (cpp_type, field_name, arg_or_none) triples of the
# trivially-copyable struct a command/gen function serializes to.
def command_struct_fields(function: dict) -> list:
    fields = []
    if function["category"] == "gen" and function["return"] != "void":
        fields.append(("WebGLObjectId", "id", None))
    for arg in function["args"]:
        field_name = snake_case(arg["name"])
        if function["category"] == "gen" and is_pointer(arg) and not is_const_pointer(arg):
            fields.append(("WebGLDataSpan", field_name, arg))  # client-allocated ids
        elif arg.get("object") and not is_pointer(arg):
            fields.append(("WebGLObjectId", field_name, arg))
        elif arg.get("object") and is_const_pointer(arg):
            fields.append(("WebGLDataSpan", field_name, arg))  # array of client ids
        elif arg.get("string"):
            fields.append(("WebGLDataSpan", field_name, arg))  # NUL-terminated bytes
        elif arg.get("offset"):
            fields.append(("GLintptr", field_name, arg))
        elif "payload" in arg:
            if arg.get("nullable"):
                fields.append(("bool", "has_" + field_name, arg))
            fields.append(("WebGLDataSpan", field_name, arg))
        else:
            assert not is_pointer(arg), f"unhandled pointer arg {function['name']}.{arg['name']}"
            fields.append((arg["type"], field_name, arg))
    return fields


# Returns the ordered (cpp_type, field_name) pairs of the struct that a command stream entry serializes to.
def command_wire_fields(function: dict) -> list:
    if is_wire_command(function):
        return [(field["type"], field["name"]) for field in function["wire_command"]]
    return [(cpp_type, field_name) for cpp_type, field_name, _ in command_struct_fields(function)]


# Returns the ordered (cpp_type, field_name) pairs of the request struct of a synchronous call.
def sync_request_wire_fields(function: dict) -> list:
    if is_wire_sync(function):
        return [(field["type"], field["name"]) for field in function["wire_request"]]
    return [(cpp_type, field_name) for cpp_type, field_name, _ in sync_request_fields(function)]


# Returns the ordered (cpp_type, field_name) pairs of the reply struct of a synchronous call.
def sync_reply_wire_fields(function: dict) -> list:
    if is_wire_sync(function):
        return [(field["type"], field["name"]) for field in function["wire_reply"]]
    return [(cpp_type, field_name) for cpp_type, field_name, _ in sync_reply_fields(function)]


# The (size, alignment) in bytes of every type that a field of a wire struct can have. Both sides of the command
# stream are 64-bit, and the generated structs assert that the compiler lays them out as computed from this table.
WIRE_TYPE_LAYOUTS = {
    "GLenum": (4, 4),
    "GLuint": (4, 4),
    "GLbitfield": (4, 4),
    "GLint": (4, 4),
    "GLsizei": (4, 4),
    "GLfloat": (4, 4),
    "GLboolean": (1, 1),
    "GLintptr": (8, 8),
    "GLsizeiptr": (8, 8),
    "GLuint64": (8, 8),
    "WebGLObjectId": (4, 4),
    "WebGLDataSpan": (8, 4),
    "bool": (1, 1),
    "int": (4, 4),
    "u32": (4, 4),
}


@dataclass
class WireField:
    cpp_type: str
    name: str
    offset: int
    size: int


@dataclass
class WireStruct:
    # The name of the struct inside the namespace Compositing::WebGL.
    name: str
    fields: List[WireField]
    size: int
    alignment: int


def wire_struct(name: str, fields: List[Tuple[str, str]]) -> WireStruct:
    placed_fields = []
    end_of_fields = 0
    alignment = 1
    for cpp_type, field_name in fields:
        if cpp_type not in WIRE_TYPE_LAYOUTS:
            raise ValueError(f"{name}.{field_name} has the type {cpp_type}, which has no entry in WIRE_TYPE_LAYOUTS")
        field_size, field_alignment = WIRE_TYPE_LAYOUTS[cpp_type]
        offset = -(-end_of_fields // field_alignment) * field_alignment
        placed_fields.append(WireField(cpp_type, field_name, offset, field_size))
        end_of_fields = offset + field_size
        alignment = max(alignment, field_alignment)
    # NB: A struct without fields has the size 1 in C++.
    size = max(1, -(-end_of_fields // alignment) * alignment)
    return WireStruct(name, placed_fields, size, alignment)


# Every struct that crosses the command stream, in the order of their definition in WebGLCommands.h.
def wire_structs(functions: list) -> List[WireStruct]:
    structs = []
    for function in command_stream_entries(functions):
        structs.append(wire_struct(f"Commands::{command_name(function)}", command_wire_fields(function)))
    for function in sync_call_entries(functions):
        name = command_name(function)
        structs.append(wire_struct(f"SyncCalls::{name}::Request", sync_request_wire_fields(function)))
        structs.append(wire_struct(f"SyncCalls::{name}::Reply", sync_reply_wire_fields(function)))
    return structs


# The 32-bit FNV-1a hash that stands for the name of a struct, a field or a type in the table of layout words.
def wire_layout_name_hash(name: str) -> int:
    hash_value = 0x811C9DC5
    for byte in name.encode("utf-8"):
        hash_value = ((hash_value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return hash_value
