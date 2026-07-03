#!/usr/bin/env python3

# Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
#
# SPDX-License-Identifier: BSD-2-Clause

import re
import sys

from io import StringIO
from pathlib import Path
from typing import TextIO

sys.path.append(str(Path(__file__).resolve().parent))

from libweb_webgl import command_name
from libweb_webgl import deref_type
from libweb_webgl import is_const_pointer
from libweb_webgl import is_pointer
from libweb_webgl import is_wire_command
from libweb_webgl import is_wire_sync
from libweb_webgl import method_name
from libweb_webgl import run_generator
from libweb_webgl import snake_case
from libweb_webgl import sync_reply_fields

# Generates the Compositor-side replayer for the WebGL command stream: one
# replay_webgl_command() overload per command. The stream crosses a process boundary, so
# every payload span is asserted against the size the command's own fields imply before
# anything reaches GL; object ids are translated through WebGLObjectMap. GL-level
# validation stays in ANGLE (the host context runs with EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE),
# exactly as it did when WebGL lived in WebContent.


def element_type(pointer_type: str):
    base = pointer_type.replace("const", "").replace("*", "").strip()
    return None if base in ("void", "GLchar") else base


def rewrite_size_expression(expression: str, function: dict, holder: str = "command") -> str:
    # Wire fields are GLsizei/GLint (32-bit) and arrive unvalidated from WebContent. Widen
    # each to i64 before it participates in the size product so that an attacker-chosen
    # count cannot overflow the multiplication (which is signed-overflow UB, and on a
    # wraparound to a small value would let a mismatched payload pass the size check).
    for arg_name in sorted((a["name"] for a in function["args"]), key=len, reverse=True):
        expression = re.sub(
            rf"\b{re.escape(arg_name)}\b", f"static_cast<i64>({holder}.{snake_case(arg_name)})", expression
        )
    return expression


def emit_payload_resolution(lines: list, function: dict, arg: dict) -> str:
    field = snake_case(arg["name"])
    expression = rewrite_size_expression(arg["payload"], function)
    size_check = f"static_cast<i64>({field}_bytes.size()) != static_cast<i64>({expression})"
    if arg.get("nullable"):
        lines.append(f"    ReadonlyBytes {field}_bytes;")
        lines.append(f"    if (command.has_{field}) {{")
        lines.append(f"        if (command.{field}.size != 0)")
        lines.append(f"            {field}_bytes = WebGLCommandList::resolve_data_span(payload, command.{field});")
        lines.append(f"        VERIFY(!({size_check}));")
        lines.append("    } else {")
        lines.append(f"        VERIFY(command.{field}.size == 0);")
        lines.append("    }")
    else:
        lines.append(f"    auto {field}_bytes = WebGLCommandList::resolve_data_span(payload, command.{field});")
        lines.append(f"    VERIFY(!({size_check}));")

    typed = element_type(arg["type"])
    if typed:
        if arg.get("nullable"):
            lines.append(f"    Span<{typed} const> {field};")
            lines.append(f"    if (command.has_{field} && command.{field}.size != 0)")
            lines.append(f"        {field} = WebGLCommandList::resolve_typed_span<{typed}>(payload, command.{field});")
        else:
            lines.append(f"    auto {field} = WebGLCommandList::resolve_typed_span<{typed}>(payload, command.{field});")
        data_expression = f"{field}.data()"
    else:
        data_expression = f"{field}_bytes.data()"
    if arg.get("nullable"):
        return f"command.has_{field} ? {data_expression} : nullptr"
    return data_expression


def emit_command_body(out: TextIO, function: dict) -> bool:
    lines: list = []
    call_args: list = []
    payload_used = False
    deletes = function.get("deletes_objects", False)

    for arg in function["args"]:
        field = snake_case(arg["name"])
        if arg.get("string"):
            payload_used = True
            lines.append(f"    auto {field}_bytes = WebGLCommandList::resolve_string_span(payload, command.{field});")
            call_args.append(f"reinterpret_cast<GLchar const*>({field}_bytes.data())")
        elif arg.get("offset"):
            call_args.append(f"reinterpret_cast<void const*>(static_cast<uintptr_t>(command.{field}))")
        elif arg.get("object") and not is_pointer(arg):
            if arg["type"] == "GLsync":
                lookup = "take_sync" if deletes else "lookup_sync"
                lines.append(f"    auto {field} = objects.{lookup}(command.{field});")
            elif arg.get("zero_means_default"):
                # The JSON names the OpenGLContext getter that supplies the host-side
                # object for client id 0.
                default_getter = arg["zero_means_default"]
                assert default_getter in (
                    "default_framebuffer",
                    "default_renderbuffer",
                ), f"unknown zero_means_default getter {default_getter!r} on {function['name']}.{arg['name']}"
                lines.append(
                    f"    GLuint {field} = command.{field} ? objects.lookup(command.{field}) : gl.{default_getter}();"
                )
            else:
                lookup = "take" if deletes else "lookup"
                lines.append(f"    auto {field} = objects.{lookup}(command.{field});")
            call_args.append(field)
        elif arg.get("object") and is_const_pointer(arg):
            payload_used = True
            data_expression = emit_payload_resolution(lines, function, arg)
            assert data_expression == f"{field}.data()", "object arrays are typed WebGLObjectId spans"
            lines.append(f"    Vector<GLuint> {field}_names;")
            lines.append(f"    {field}_names.ensure_capacity({field}.size());")
            lines.append(f"    for (auto id : {field})")
            lines.append(f"        {field}_names.unchecked_append(objects.{'take' if deletes else 'lookup'}(id));")
            call_args.append(f"{field}_names.data()")
        elif "payload" in arg:
            payload_used = True
            call_args.append(emit_payload_resolution(lines, function, arg))
        else:
            call_args.append(f"command.{field}")

    lines.append(f"    gl.{method_name(function)}({', '.join(call_args)});")
    out.write("\n".join(lines) + "\n")
    return payload_used


def emit_gen_body(out: TextIO, function: dict) -> bool:
    if function["return"] != "void":
        scalar_args = ", ".join(f"command.{snake_case(a['name'])}" for a in function["args"])
        add = "add_sync" if function["return"] == "GLsync" else "add"
        out.write(f"    TRY(objects.{add}(command.id, gl.{method_name(function)}({scalar_args})));\n")
        return False

    # glGen*(GLsizei n, GLuint* out) shape: the span carries the client-allocated ids.
    count_field = snake_case(function["args"][0]["name"])
    span_field = snake_case(function["args"][1]["name"])
    out.write(f"""    auto ids = WebGLCommandList::resolve_typed_span<WebGLObjectId>(payload, command.{span_field});
    VERIFY(static_cast<i64>(ids.size()) == static_cast<i64>(command.{count_field}));
    for (auto id : ids) {{
        GLuint name = 0;
        gl.{method_name(function)}(1, &name);
        TRY(objects.add(id, name));
    }}
""")
    return True


def signature(function: dict, payload_used: bool) -> str:
    uses_command = function["category"] == "gen" or function["args"]
    uses_objects = function["category"] == "gen" or any(a.get("object") for a in function["args"])
    command = "const& command" if uses_command else "const&"
    objects = "WebGLObjectMap& objects" if uses_objects else "WebGLObjectMap&"
    payload = "ReadonlyBytes payload" if payload_used else "ReadonlyBytes"
    return (
        f"ErrorOr<void> replay_webgl_command(OpenGLContext& gl, {objects}, "
        f"Web::WebGL::Commands::{command_name(function)} {command}, {payload})"
    )


def emit_sync_body(out: TextIO, function: dict) -> tuple:
    lines: list = []
    call_args: list = []
    out_blobs: list = []  # field names of Vector-backed reply blobs, in arg order
    payload_used = False
    objects_used = False

    for arg in function["args"]:
        field = snake_case(arg["name"])
        # An arg-level "host_override" in GLFunctions.json replaces the wire value with a
        # host-chosen constant (e.g. a page must never be able to block the compositor).
        override = arg.get("host_override")
        if override is not None:
            call_args.append(override)
        elif arg.get("out"):
            if "payload" in arg:
                element = deref_type(arg["type"])
                expression = rewrite_size_expression(arg["payload"], function, "request")
                lines.append(f"    auto {field}_byte_size = static_cast<size_t>({expression});")
                lines.append(f"    Vector<{element}> {field};")
                lines.append(f"    {field}.resize({field}_byte_size / sizeof({element}));")
                call_args.append(f"{field}.data()")
                out_blobs.append((field, element))
            else:
                lines.append(f"    {deref_type(arg['type'])} {field} {{}};")
                call_args.append(f"&{field}")
        elif arg.get("object") and not is_pointer(arg):
            objects_used = True
            lookup = "lookup_sync" if arg["type"] == "GLsync" else "lookup"
            lines.append(f"    auto {field} = objects.{lookup}(request.{field});")
            call_args.append(field)
        elif arg.get("string"):
            payload_used = True
            lines.append(f"    auto {field}_bytes = WebGLCommandList::resolve_string_span(payload, request.{field});")
            call_args.append(f"reinterpret_cast<GLchar const*>({field}_bytes.data())")
        elif "payload" in arg:
            payload_used = True
            element = deref_type(arg["type"].replace("const", "").strip())
            expression = rewrite_size_expression(arg["payload"], function, "request")
            lines.append(
                f"    auto {field} = WebGLCommandList::resolve_typed_span<{element}>(payload, request.{field});"
            )
            lines.append(
                f"    VERIFY(static_cast<i64>({field}.size() * sizeof({element})) == static_cast<i64>({expression}));"
            )
            call_args.append(f"{field}.data()")
        else:
            call_args.append(f"request.{field}")

    invocation = f"gl.{method_name(function)}({', '.join(call_args)});"
    if function["return"] != "void":
        invocation = "auto return_value = " + invocation
    lines.append(f"    {invocation}")

    # Assemble the reply: span fields point at the Vector blobs, laid out in order.
    reply_type = f"SyncCalls::{command_name(function)}::Reply"
    blob_spans = []
    for index, (field, element) in enumerate(out_blobs):
        if index == 0:
            offset = f"WebGLCommandList::first_inline_data_offset(sizeof({reply_type}))"
        else:
            offset = f"WebGLCommandList::next_inline_data_offset({out_blobs[index - 1][0]}_span)"
        lines.append(
            f"    WebGLDataSpan {field}_span {{ {offset}, static_cast<u32>({field}.size() * sizeof({element})) }};"
        )
        blob_spans.append(field)

    initializers = []
    for _, field_name, arg in sync_reply_fields(function):
        if arg is not None and "payload" in arg:
            initializers.append(f".{field_name} = {field_name}_span")
        else:
            initializers.append(f".{field_name} = {field_name}")
    lines.append(f"    {reply_type} reply {{ {', '.join(initializers)} }};")

    blob_arguments = "".join(
        f", ReadonlyBytes {{ {field}.data(), {field}.size() * sizeof({element}) }}" for field, element in out_blobs
    )
    lines.append(f"    return WebGLSyncCall::encode_reply(reply{blob_arguments});")

    out.write("\n".join(lines) + "\n")
    return payload_used, objects_used


def sync_signature(function: dict, payload_used: bool, objects_used: bool) -> str:
    name = command_name(function)
    request = "const& request" if function["args"] else "const&"
    objects = "WebGLObjectMap& objects" if objects_used else "WebGLObjectMap&"
    payload = "ReadonlyBytes payload" if payload_used else "ReadonlyBytes"
    return (
        f"static ByteBuffer handle_one(OpenGLContext& gl, {objects}, SyncCalls::{name}::Request {request}, {payload})"
    )


def write_header_file(out: TextIO, functions: list) -> None:
    out.write("""#pragma once

#include <AK/Error.h>
#include <Compositor/WebGLObjectMap.h>
#include <Compositor/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLCommandList.h>

namespace Compositor {
""")
    for function in functions:
        if function["category"] not in ("command", "gen"):
            continue
        out.write(
            f"ErrorOr<void> replay_webgl_command(OpenGLContext&, WebGLObjectMap&, "
            f"Web::WebGL::Commands::{command_name(function)} const&, ReadonlyBytes);\n"
        )
    out.write("""
// Wire-specified ops; defined manually in HostWebGLContext.cpp. Builtin commands carry
// host-level semantics (presenting, resizing) and are dispatched by the host itself
// rather than through replay_webgl_command.
""")
    for function in functions:
        if function["category"] == "custom" and is_wire_command(function):
            out.write(
                f"ErrorOr<void> replay_webgl_command(OpenGLContext&, WebGLObjectMap&, "
                f"Web::WebGL::Commands::{command_name(function)} const&, ReadonlyBytes);\n"
            )
    for function in functions:
        if is_wire_sync(function):
            out.write(
                f"ErrorOr<ByteBuffer> handle_one(OpenGLContext&, WebGLObjectMap&, "
                f"Web::WebGL::SyncCalls::{command_name(function)}::Request const&, ReadonlyBytes);\n"
            )
    out.write("""
ErrorOr<ByteBuffer> handle_webgl_sync_call(OpenGLContext&, WebGLObjectMap&, ReadonlyBytes request);

}
""")


def write_implementation_file(out: TextIO, functions: list) -> None:
    out.write("""#include <AK/Assertions.h>
#include <AK/Vector.h>
#include <Compositor/WebGLCommandReplayer.h>

namespace Compositor {

using namespace Web::WebGL;

""")
    for function in functions:
        if function["category"] not in ("command", "gen"):
            continue
        body = StringIO()
        if function["category"] == "gen":
            payload_used = emit_gen_body(body, function)
        else:
            payload_used = emit_command_body(body, function)
        out.write(f"{signature(function, payload_used)}\n{{\n")
        out.write(body.getvalue())
        out.write("    return {};\n}\n\n")

    for function in functions:
        if function["category"] != "sync":
            continue
        body = StringIO()
        payload_used, objects_used = emit_sync_body(body, function)
        out.write(f"{sync_signature(function, payload_used, objects_used)}\n{{\n")
        out.write(body.getvalue())
        out.write("}\n\n")

    out.write("""ErrorOr<ByteBuffer> handle_webgl_sync_call(OpenGLContext& gl, WebGLObjectMap& objects, ReadonlyBytes request)
{
    return WebGLSyncCall::dispatch_request(request, [&]<typename Call>(typename Call::Request const& call_request, ReadonlyBytes payload) -> ErrorOr<ByteBuffer> {
        return handle_one(gl, objects, call_request, payload);
    });
}

}
""")


if __name__ == "__main__":
    run_generator("Generate the Compositor WebGL command replayer", write_header_file, write_implementation_file)
