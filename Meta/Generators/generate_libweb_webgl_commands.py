#!/usr/bin/env python3

# Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
#
# SPDX-License-Identifier: BSD-2-Clause

import sys

from pathlib import Path
from typing import TextIO

sys.path.append(str(Path(__file__).resolve().parent))

from libweb_webgl import command_name
from libweb_webgl import command_stream_entries
from libweb_webgl import command_struct_fields
from libweb_webgl import is_wire_command
from libweb_webgl import is_wire_sync
from libweb_webgl import run_generator
from libweb_webgl import sync_call_entries
from libweb_webgl import sync_reply_fields
from libweb_webgl import sync_request_fields

# Generates the WebGL command stream vocabulary from GLFunctions.json: one opcode and one
# trivially-copyable struct per "command"/"gen" entry. The recorder (WebContent) and the
# replayer (Compositor) are generated from the same source of truth, so the wire format
# cannot drift between the two sides.

# The command and sync-call vocabularies share their X-macro shape; everything but the
# structs themselves is emitted by the helpers below.


def write_type_enum(out: TextIO, macro: str, enum_name: str, count_name: str, names: list) -> None:
    out.write(f"#define {macro}(V) \\\n")
    for name in names:
        out.write(f"    V({name}) \\\n")
    out.write(f"""
enum class {enum_name} : u16 {{
#define __ENUMERATE(name) name,
    {macro}(__ENUMERATE)
#undef __ENUMERATE
}};

inline constexpr u16 {count_name} = {len(names)};

WEB_API StringView to_string({enum_name});
""")


def write_visit_function(out: TextIO, macro: str, enum_name: str, function_name: str, namespace: str) -> None:
    out.write(f"""
template<typename Callback>
decltype(auto) {function_name}({enum_name} type, Callback&& callback)
{{
    switch (type) {{
#define __ENUMERATE(name) \\
    case {enum_name}::name: \\
        return callback.template operator()<{namespace}::name>();
        {macro}(__ENUMERATE)
#undef __ENUMERATE
    }}
    VERIFY_NOT_REACHED();
}}
""")


def write_static_asserts(out: TextIO, macro: str, assert_body: str) -> None:
    out.write(f"""
#define __ENUMERATE(name) {assert_body}
{macro}(__ENUMERATE)
#undef __ENUMERATE
""")


def write_to_string_implementation(out: TextIO, macro: str, enum_name: str) -> None:
    out.write(f"""
StringView to_string({enum_name} type)
{{
    switch (type) {{
#define __ENUMERATE(name) \\
    case {enum_name}::name: \\
        return #name##sv;
        {macro}(__ENUMERATE)
#undef __ENUMERATE
    }}
    VERIFY_NOT_REACHED();
}}
""")


def write_header_file(out: TextIO, functions: list) -> None:
    commands = command_stream_entries(functions)
    sync_calls = sync_call_entries(functions)

    out.write("""#pragma once

#include <AK/Assertions.h>
#include <AK/StdLibExtras.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebGL/GLFunctions.h>

namespace Web::WebGL {

""")
    write_type_enum(
        out,
        "ENUMERATE_WEBGL_COMMANDS",
        "WebGLCommandType",
        "webgl_command_type_count",
        [command_name(f) for f in commands],
    )
    out.write("\nnamespace Commands {\n")
    for f in commands:
        name = command_name(f)
        out.write(f"\nstruct {name} {{\n")
        out.write(f"    static constexpr auto command_type = WebGLCommandType::{name};\n")
        if is_wire_command(f):
            for field in f["wire_command"]:
                out.write(f"    {field['type']} {field['name']} {{}};\n")
        else:
            for cpp_type, field_name, _ in command_struct_fields(f):
                out.write(f"    {cpp_type} {field_name} {{}};\n")
        out.write("};\n")
    out.write("\n}\n")
    write_visit_function(out, "ENUMERATE_WEBGL_COMMANDS", "WebGLCommandType", "visit_webgl_command_type", "Commands")
    write_static_asserts(out, "ENUMERATE_WEBGL_COMMANDS", "static_assert(IsTriviallyCopyable<Commands::name>);")

    out.write("\n")
    write_type_enum(
        out,
        "ENUMERATE_WEBGL_SYNC_CALLS",
        "WebGLSyncCallType",
        "webgl_sync_call_type_count",
        [command_name(f) for f in sync_calls],
    )
    out.write("\nnamespace SyncCalls {\n")
    for f in sync_calls:
        name = command_name(f)
        if is_wire_sync(f):
            request_fields = [(field["type"], field["name"]) for field in f["wire_request"]]
            reply_fields = [(field["type"], field["name"]) for field in f["wire_reply"]]
        else:
            request_fields = [(cpp_type, field_name) for cpp_type, field_name, _ in sync_request_fields(f)]
            reply_fields = [(cpp_type, field_name) for cpp_type, field_name, _ in sync_reply_fields(f)]
        out.write(f"\nstruct {name} {{\n")
        out.write(f"    static constexpr auto call_type = WebGLSyncCallType::{name};\n")
        out.write("    struct Request {\n")
        for cpp_type, field_name in request_fields:
            out.write(f"        {cpp_type} {field_name} {{}};\n")
        out.write("    };\n")
        out.write("    struct Reply {\n")
        for cpp_type, field_name in reply_fields:
            out.write(f"        {cpp_type} {field_name} {{}};\n")
        out.write("    };\n")
        out.write("};\n")
    out.write("\n}\n")
    write_visit_function(
        out, "ENUMERATE_WEBGL_SYNC_CALLS", "WebGLSyncCallType", "visit_webgl_sync_call_type", "SyncCalls"
    )
    write_static_asserts(
        out,
        "ENUMERATE_WEBGL_SYNC_CALLS",
        "static_assert(IsTriviallyCopyable<SyncCalls::name::Request>); static_assert(IsTriviallyCopyable<SyncCalls::name::Reply>);",
    )
    out.write("\n}\n")


def write_implementation_file(out: TextIO, functions: list) -> None:
    out.write("""#include <LibWeb/WebGL/WebGLCommands.h>

namespace Web::WebGL {
""")
    write_to_string_implementation(out, "ENUMERATE_WEBGL_COMMANDS", "WebGLCommandType")
    write_to_string_implementation(out, "ENUMERATE_WEBGL_SYNC_CALLS", "WebGLSyncCallType")
    out.write("\n}\n")


if __name__ == "__main__":
    run_generator("Generate WebGL command stream vocabulary", write_header_file, write_implementation_file)
