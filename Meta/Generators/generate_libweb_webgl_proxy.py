#!/usr/bin/env python3

# Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
#
# SPDX-License-Identifier: BSD-2-Clause

import sys

from pathlib import Path
from typing import TextIO

sys.path.append(str(Path(__file__).resolve().parent))

from libweb_webgl import command_name
from libweb_webgl import is_const_pointer
from libweb_webgl import is_pointer
from libweb_webgl import method_signature
from libweb_webgl import run_generator
from libweb_webgl import snake_case
from libweb_webgl import sync_reply_fields

# Generates WebGLContextProxy, WebContent's drop-in replacement for the GL seam: the
# same method signatures as the generated GLFunctions class, but recording commands into
# a WebGLCommandList (flushed to the Compositor) instead of calling GL. Value-returning
# entry points become synchronous round trips. Method parameter names match the GL
# argument names from GLFunctions.json, so the JSON's payload-size expressions are used
# verbatim.


def object_id_expression(arg: dict) -> str:
    if arg["type"] == "GLsync":
        return f"static_cast<WebGLObjectId>(reinterpret_cast<uintptr_t>({arg['name']}))"
    return arg["name"]


# Emits statements declaring `<field>_bytes` spans and a fully-initialized `command`
# whose WebGLDataSpan fields point at them in append order.
def emit_command_construction(out: TextIO, function: dict, command_type: str) -> list:
    blobs = []  # (field, bytes_variable)
    initializers = []
    for arg in function["args"]:
        field = snake_case(arg["name"])
        name = arg["name"]
        if arg.get("string"):
            out.write(f"    ReadonlyBytes {field}_bytes {{ {name}, __builtin_strlen({name}) + 1 }};\n")
            blobs.append(field)
            initializers.append((field, None))
        elif arg.get("offset"):
            initializers.append((field, f"static_cast<GLintptr>(reinterpret_cast<uintptr_t>({name}))"))
        elif arg.get("object") and not is_pointer(arg):
            initializers.append((field, object_id_expression(arg)))
        elif (arg.get("object") and is_const_pointer(arg)) or "payload" in arg:
            size = f"static_cast<size_t>({arg['payload']})"
            if arg.get("nullable"):
                out.write(f"    ReadonlyBytes {field}_bytes {{ {name}, {name} ? {size} : 0 }};\n")
                initializers.append((f"has_{field}", f"{name} != nullptr"))
            else:
                out.write(f"    ReadonlyBytes {field}_bytes {{ {name}, {size} }};\n")
            blobs.append(field)
            initializers.append((field, None))
        else:
            initializers.append((field, name))

    fields = ", ".join(f".{field} = {value}" for field, value in initializers if value is not None)
    out.write(f"    {command_type} command {{ {fields} }};\n")
    for index, field in enumerate(blobs):
        if index == 0:
            offset = "WebGLCommandList::first_inline_data_offset(sizeof(command))"
        else:
            offset = f"WebGLCommandList::next_inline_data_offset(command.{blobs[index - 1]})"
        out.write(f"    command.{field} = {{ {offset}, static_cast<u32>({field}_bytes.size()) }};\n")
    return blobs


def emit_record(out: TextIO, blobs: list) -> None:
    blob_args = "".join(f", {field}_bytes" for field in blobs)
    out.write(f"    record(command{blob_args});\n")


def emit_command_method(out: TextIO, function: dict) -> None:
    command_type = f"Commands::{command_name(function)}"
    out.write(f"{method_signature(function, 'WebGLContextProxy::')}\n{{\n")
    blobs = emit_command_construction(out, function, command_type)
    assert len(blobs) <= 1, f"{function['name']} needs more inline blobs than record() supports"
    emit_record(out, blobs)
    out.write("}\n\n")


def emit_gen_method(out: TextIO, function: dict) -> None:
    command_type = f"Commands::{command_name(function)}"
    out.write(f"{method_signature(function, 'WebGLContextProxy::')}\n{{\n")
    if function["return"] != "void":
        scalars = "".join(f", .{snake_case(a['name'])} = {a['name']}" for a in function["args"])
        out.write("    auto id = allocate_object_id();\n")
        out.write(f"    record({command_type} {{ .id = id{scalars} }});\n")
        if function["return"] == "GLsync":
            out.write("    return reinterpret_cast<GLsync>(static_cast<uintptr_t>(id));\n")
        else:
            out.write("    return id;\n")
        out.write("}\n\n")
        return

    count_name = function["args"][0]["name"]
    span_field = snake_case(function["args"][1]["name"])
    out_name = function["args"][1]["name"]
    out.write(f"""    if ({count_name} <= 0)
        return;
    static_assert(IsSame<WebGLObjectId, GLuint>);
    for (GLsizei i = 0; i < {count_name}; ++i)
        {out_name}[i] = allocate_object_id();
    ReadonlyBytes {span_field}_bytes {{ {out_name}, static_cast<size_t>({count_name}) * sizeof(WebGLObjectId) }};
    Commands::{command_name(function)} command {{ .{snake_case(count_name)} = {count_name} }};
    command.{span_field} = {{ WebGLCommandList::first_inline_data_offset(sizeof(command)), static_cast<u32>({span_field}_bytes.size()) }};
    record(command, {span_field}_bytes);
}}

""")


def emit_sync_method(out: TextIO, function: dict) -> None:
    call = f"SyncCalls::{command_name(function)}"
    out.write(f"{method_signature(function, 'WebGLContextProxy::')}\n{{\n")

    # Build the request (in-args only) plus its inline blobs.
    blobs = []
    initializers = []
    for arg in function["args"]:
        if arg.get("out"):
            continue
        field = snake_case(arg["name"])
        name = arg["name"]
        if arg.get("string"):
            out.write(f"    ReadonlyBytes {field}_bytes {{ {name}, __builtin_strlen({name}) + 1 }};\n")
            blobs.append(field)
        elif "payload" in arg:
            out.write(f"    ReadonlyBytes {field}_bytes {{ {name}, static_cast<size_t>({arg['payload']}) }};\n")
            blobs.append(field)
        elif arg.get("object") and not is_pointer(arg):
            initializers.append((field, object_id_expression(arg)))
        else:
            initializers.append((field, name))
    assert len(blobs) <= 1, f"{function['name']} needs more request blobs than encode_request supports"

    fields = ", ".join(f".{field} = {value}" for field, value in initializers)
    out.write(f"    {call}::Request request {{ {fields} }};\n")
    for field in blobs:
        out.write(
            f"    request.{field} = {{ WebGLCommandList::first_inline_data_offset(sizeof(request)), static_cast<u32>({field}_bytes.size()) }};\n"
        )
    blob_argument = f", {blobs[0]}_bytes" if blobs else ""
    out.write(
        f"    auto reply_bytes = send_sync_call(WebGLSyncCall::encode_request<{call}>(request{blob_argument}));\n"
    )

    has_return = function["return"] != "void"
    failure = "        return {};\n" if has_return else "        return;\n"
    out.write("    if (is_lost())\n")
    out.write(failure)
    if sync_reply_fields(function):
        out.write(f"    auto reply = WebGLSyncCall::decode_reply<{call}::Reply>(reply_bytes);\n")

    for _, field_name, arg in sync_reply_fields(function):
        if arg is None:
            continue
        name = arg["name"]
        if "payload" in arg:
            out.write(f"""    if ({name})
        WebGLCommandList::copy_data_span(reply_bytes, reply.{field_name}, {{ {name}, static_cast<size_t>({arg["payload"]}) }});
""")
        else:
            out.write(f"    if ({name})\n        *{name} = reply.{field_name};\n")

    if has_return:
        out.write("    return reply.return_value;\n")
    out.write("}\n\n")


def write_header_file(out: TextIO, functions: list) -> None:
    out.write("""#pragma once

#include <LibWeb/WebGL/WebGLCommands.h>
#include <LibWeb/WebGL/WebGLContextProxyBase.h>

namespace Web::WebGL {

// The remote-recording implementation of the GL seam: identical signatures to
// GLFunctions, so the WebGL implementation files cannot tell the difference.
class WEB_API WebGLContextProxy final : public WebGLContextProxyBase {
public:
    using WebGLContextProxyBase::WebGLContextProxyBase;

""")
    for function in functions:
        if function["category"] not in ("command", "gen", "sync"):
            continue
        out.write(f"    {method_signature(function)};\n")
    out.write("""
    // Custom-handled entry points; defined manually in WebGLContextProxyBase.cpp.
""")
    for function in functions:
        if function["category"] != "custom":
            continue
        out.write(f"    {method_signature(function)};\n")
    out.write("""};

}
""")


def write_implementation_file(out: TextIO, functions: list) -> None:
    out.write("""#include <LibWeb/WebGL/WebGLContextProxy.h>

namespace Web::WebGL {

""")
    for function in functions:
        if function["category"] == "command":
            emit_command_method(out, function)
        elif function["category"] == "gen":
            emit_gen_method(out, function)
        elif function["category"] == "sync":
            emit_sync_method(out, function)
    out.write("}\n")


if __name__ == "__main__":
    run_generator("Generate the WebGL context proxy recorder", write_header_file, write_implementation_file)
