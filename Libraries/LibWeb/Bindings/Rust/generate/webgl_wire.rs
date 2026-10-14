/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Writes the vocabulary of the WebGL command stream: the command types, the synchronous call types and the struct of
//! each, which src/web_gl/proxy/vocabulary.rs includes. The C++ side of the stream gets the same from
//! Meta/Generators/generate_libweb_webgl_commands.py.

use std::fmt::Write;

use crate::webgl_wire_model::{Vocabulary, WireStruct, WireType, rust_identifier, wire_layout_name_hash};

/// A field of the Rust struct: one of the model, or the bytes that the C ABI leaves unused, which the struct spells
/// out so that all of its bytes are initialized and it can be written to the stream as bytes.
struct RustField {
    name: String,
    rust_type: String,
    zero: String,
}

fn rust_fields(layout: &WireStruct) -> Vec<RustField> {
    let mut fields = Vec::new();
    if layout.fields.is_empty() {
        fields.push(RustField {
            name: "_empty".into(),
            rust_type: "u8".into(),
            zero: "0".into(),
        });
        return fields;
    }
    for (index, field) in layout.fields.iter().enumerate() {
        let end = field.offset + field.wire_type.size();
        let next_offset = layout.fields.get(index + 1).map_or(layout.size, |next| next.offset);
        let zero = match field.wire_type {
            WireType::Bool => "false",
            WireType::F32 => "0.0",
            WireType::DataSpan => "DataSpan::EMPTY",
            WireType::U8 | WireType::U32 | WireType::I32 | WireType::I64 | WireType::U64 => "0",
        };
        fields.push(RustField {
            name: rust_identifier(&field.spec.name),
            rust_type: field.wire_type.rust_type().into(),
            zero: zero.into(),
        });
        if next_offset > end {
            let padding = next_offset - end;
            fields.push(RustField {
                name: format!("_padding_after_{}", field.spec.name),
                rust_type: format!("[u8; {padding}]"),
                zero: format!("[0; {padding}]"),
            });
        }
    }
    fields
}

fn write_struct(out: &mut String, rust_name: &str, layout: &WireStruct) {
    let fields = rust_fields(layout);
    writeln!(out, "    /// `{}`", layout.cpp_name).unwrap();
    out.push_str("    #[derive(Clone, Copy, Debug, PartialEq)]\n    #[repr(C)]\n");
    writeln!(out, "    pub(crate) struct {rust_name} {{").unwrap();
    for field in &fields {
        writeln!(out, "        pub(crate) {}: {},", field.name, field.rust_type).unwrap();
    }
    out.push_str("    }\n");
    // NB: The assertions below are what the safety of the implementation rests on: the fields are where the C ABI
    //     puts them and fill the struct, so it has no byte that the compiler could leave uninitialized.
    out.push_str("    // SAFETY: The struct is repr(C) and made of plain values that fill it, as asserted below.\n");
    writeln!(out, "    unsafe impl WireStruct for {rust_name} {{").unwrap();
    out.push_str("        const ZEROED: Self = Self {\n");
    for field in &fields {
        writeln!(out, "            {}: {},", field.name, field.zero).unwrap();
    }
    out.push_str("        };\n    }\n");
    out.push_str("    const _: () = {\n");
    writeln!(out, "        assert!(size_of::<{rust_name}>() == {});", layout.size).unwrap();
    writeln!(
        out,
        "        assert!(align_of::<{rust_name}>() == {});",
        layout.alignment
    )
    .unwrap();
    for field in &layout.fields {
        let name = rust_identifier(&field.spec.name);
        writeln!(
            out,
            "        assert!(offset_of!({rust_name}, {name}) == {});",
            field.offset
        )
        .unwrap();
    }
    out.push_str("    };\n\n");
}

fn write_type_enum(out: &mut String, enum_name: &str, count_name: &str, description: &str, names: &[&str]) {
    writeln!(out, "/// {description}").unwrap();
    out.push_str("#[derive(Clone, Copy, Debug, PartialEq, Eq)]\n#[repr(u16)]\n");
    writeln!(out, "pub(crate) enum {enum_name} {{").unwrap();
    for name in names {
        writeln!(out, "    {name},").unwrap();
    }
    out.push_str("}\n\n");
    // NB: Only the tests of the vocabulary ask how many types there are.
    writeln!(
        out,
        "#[cfg(test)]\npub(crate) const {count_name}: u16 = {};\n",
        names.len()
    )
    .unwrap();
}

fn write_layout_words(out: &mut String, rust_path: &str, layout: &WireStruct) {
    writeln!(out, "    // {}", layout.cpp_name).unwrap();
    writeln!(
        out,
        "    {:#010x}, size_of::<{rust_path}>() as u32, align_of::<{rust_path}>() as u32, {},",
        wire_layout_name_hash(&layout.cpp_name),
        layout.fields.len()
    )
    .unwrap();
    for field in &layout.fields {
        writeln!(
            out,
            "    {:#010x}, {:#010x}, offset_of!({rust_path}, {}) as u32, size_of::<{}>() as u32,",
            wire_layout_name_hash(&field.spec.name),
            wire_layout_name_hash(&field.spec.cpp_type),
            rust_identifier(&field.spec.name),
            field.wire_type.rust_type()
        )
        .unwrap();
    }
}

pub fn write_wire_vocabulary(vocabulary: &Vocabulary) -> String {
    let mut out = String::from("// Generated by build.rs from Libraries/LibCompositing/WebGL/GLFunctions.json.\n\n");
    out.push_str("use core::mem::offset_of;\n\n");
    out.push_str("use crate::web_gl::proxy::wire::DataSpan;\n\n");

    let command_names: Vec<&str> = vocabulary
        .commands
        .iter()
        .map(|entry| entry.function.command_name())
        .collect();
    write_type_enum(
        &mut out,
        "CommandType",
        "COMMAND_TYPE_COUNT",
        "What a record of the command stream is, in the order of the C++ enum `WebGLCommandType`.",
        &command_names,
    );
    let sync_call_names: Vec<&str> = vocabulary
        .sync_calls
        .iter()
        .map(|entry| entry.function.command_name())
        .collect();
    write_type_enum(
        &mut out,
        "SyncCallType",
        "SYNC_CALL_TYPE_COUNT",
        "What a synchronous call is, in the order of the C++ enum `WebGLSyncCallType`.",
        &sync_call_names,
    );
    let max_command_struct_size = vocabulary
        .commands
        .iter()
        .map(|entry| entry.layout.size)
        .max()
        .unwrap_or(0);
    out.push_str("/// The size of the largest struct of a record.\n");
    writeln!(
        out,
        "pub(crate) const MAX_COMMAND_STRUCT_SIZE: usize = {max_command_struct_size};\n"
    )
    .unwrap();

    out.push_str(
        "/// The structs of the records of the command stream, which are those of the C++ namespace `Commands`.\n",
    );
    out.push_str("pub(crate) mod commands {\n");
    out.push_str("    use core::mem::offset_of;\n\n");
    out.push_str("    use super::CommandType;\n");
    out.push_str("    use crate::web_gl::proxy::wire::{Command, DataSpan, WireStruct};\n\n");
    for entry in &vocabulary.commands {
        let name = entry.function.command_name();
        write_struct(&mut out, name, &entry.layout);
        writeln!(out, "    impl Command for {name} {{").unwrap();
        writeln!(out, "        const TYPE: CommandType = CommandType::{name};").unwrap();
        out.push_str("    }\n\n");
    }
    out.push_str("}\n\n");

    out.push_str("/// The structs of the synchronous calls, which are those of the C++ namespace `SyncCalls`.\n");
    out.push_str("pub(crate) mod sync_calls {\n");
    out.push_str("    use core::mem::offset_of;\n\n");
    out.push_str("    use super::SyncCallType;\n");
    out.push_str("    use crate::web_gl::proxy::wire::{DataSpan, SyncReply, SyncRequest, WireStruct};\n\n");
    for entry in &vocabulary.sync_calls {
        let name = entry.function.command_name();
        write_struct(&mut out, &format!("{name}Request"), &entry.request);
        write_struct(&mut out, &format!("{name}Reply"), &entry.reply);
        writeln!(out, "    impl SyncRequest for {name}Request {{").unwrap();
        writeln!(out, "        const TYPE: SyncCallType = SyncCallType::{name};").unwrap();
        writeln!(out, "        type Reply = {name}Reply;").unwrap();
        out.push_str("    }\n");
        // NB: vocabulary() rejects a reply with a field that some bit pattern is no value of.
        out.push_str("    // SAFETY: Every field of the struct is a number or made of numbers.\n");
        writeln!(out, "    unsafe impl SyncReply for {name}Reply {{}}\n").unwrap();
    }
    out.push_str("}\n\n");

    out.push_str(
        "/// The layout of every struct above as this compiler laid it out, in the form of the table that the C++ side\n\
         /// reports: per struct the hash of its C++ name, its size, its alignment and the number of its fields, then\n\
         /// per field the hash of its name, the hash of the name of its C++ type, its offset and its size.\n",
    );
    out.push_str("#[allow(clippy::unreadable_literal, reason = \"the hashes are not read\")]\n");
    out.push_str("pub(crate) static WIRE_STRUCT_LAYOUTS: &[u32] = &[\n");
    for entry in &vocabulary.commands {
        write_layout_words(
            &mut out,
            &format!("commands::{}", entry.function.command_name()),
            &entry.layout,
        );
    }
    for entry in &vocabulary.sync_calls {
        let name = entry.function.command_name();
        write_layout_words(&mut out, &format!("sync_calls::{name}Request"), &entry.request);
        write_layout_words(&mut out, &format!("sync_calls::{name}Reply"), &entry.reply);
    }
    out.push_str("];\n");
    out
}
