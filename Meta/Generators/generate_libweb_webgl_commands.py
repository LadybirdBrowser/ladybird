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
from libweb_webgl import command_wire_fields
from libweb_webgl import run_generator
from libweb_webgl import sync_call_entries
from libweb_webgl import sync_reply_wire_fields
from libweb_webgl import sync_request_wire_fields
from libweb_webgl import wire_layout_name_hash
from libweb_webgl import wire_structs

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

COMPOSITING_API StringView to_string({enum_name});
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
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/WebGL/GLFunctions.h>

namespace Compositing::WebGL {

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
        for cpp_type, field_name in command_wire_fields(f):
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
        out.write(f"\nstruct {name} {{\n")
        out.write(f"    static constexpr auto call_type = WebGLSyncCallType::{name};\n")
        out.write("    struct Request {\n")
        for cpp_type, field_name in sync_request_wire_fields(f):
            out.write(f"        {cpp_type} {field_name} {{}};\n")
        out.write("    };\n")
        out.write("    struct Reply {\n")
        for cpp_type, field_name in sync_reply_wire_fields(f):
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
    out.write("""
// The layout of every struct that crosses the command stream, as the compiler of this side laid it out, for a recorder
// that is not compiled from this header to compare its own against. A struct is the words (hash of its name, size,
// alignment, number of fields) followed by the words (hash of its name, hash of the name of its type, offset, size)
// of each of its fields.
COMPOSITING_API ReadonlySpan<u32> webgl_wire_struct_layouts();

}

extern "C" COMPOSITING_API u32 const* compositing_webgl_wire_struct_layouts(size_t* out_word_count);
""")


def write_wire_struct_layout_assertions(out: TextIO, functions: list) -> None:
    out.write("""
// The recorder and the replayer exchange these structs as bytes, and they can be compiled by different compilers, or
// from definitions in different languages. So the layout of each is not left to what a compiler happens to do: it is
// computed from the size and the alignment of the types of the fields, and has to be what the compiler does.
""")
    for struct in wire_structs(functions):
        out.write(f"static_assert(sizeof({struct.name}) == {struct.size});\n")
        out.write(f"static_assert(alignof({struct.name}) == {struct.alignment});\n")
        for field in struct.fields:
            out.write(f"static_assert(offsetof({struct.name}, {field.name}) == {field.offset});\n")
            out.write(f"static_assert(sizeof({struct.name}::{field.name}) == {field.size});\n")


def write_wire_struct_layouts(out: TextIO, functions: list) -> None:
    out.write("\nstatic constexpr u32 s_wire_struct_layouts[] = {\n")
    for struct in wire_structs(functions):
        struct_name_hash = wire_layout_name_hash(struct.name)
        out.write(f"    // {struct.name}\n")
        out.write(
            f"    {struct_name_hash:#010x}u, sizeof({struct.name}), alignof({struct.name}), {len(struct.fields)},\n"
        )
        for field in struct.fields:
            field_name_hash = wire_layout_name_hash(field.name)
            type_name_hash = wire_layout_name_hash(field.cpp_type)
            out.write(
                f"    {field_name_hash:#010x}u, {type_name_hash:#010x}u, "
                f"offsetof({struct.name}, {field.name}), sizeof({struct.name}::{field.name}),\n"
            )
    out.write("""};

ReadonlySpan<u32> webgl_wire_struct_layouts()
{
    return s_wire_struct_layouts;
}
""")


def write_implementation_file(out: TextIO, functions: list) -> None:
    out.write("""#include <AK/Span.h>
#include <LibCompositing/WebGL/WebGLCommands.h>
#include <stddef.h>

namespace Compositing::WebGL {
""")
    write_to_string_implementation(out, "ENUMERATE_WEBGL_COMMANDS", "WebGLCommandType")
    write_to_string_implementation(out, "ENUMERATE_WEBGL_SYNC_CALLS", "WebGLSyncCallType")
    write_wire_struct_layout_assertions(out, functions)
    write_wire_struct_layouts(out, functions)
    out.write("""
}

u32 const* compositing_webgl_wire_struct_layouts(size_t* out_word_count)
{
    auto layouts = Compositing::WebGL::webgl_wire_struct_layouts();
    *out_word_count = layouts.size();
    return layouts.data();
}
""")


if __name__ == "__main__":
    run_generator("Generate WebGL command stream vocabulary", write_header_file, write_implementation_file)
