/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Writes the vocabulary of the WebGL command stream: the command types, the synchronous call types and the struct of
//! each, which src/webgl/vocabulary.rs includes.

use std::fmt::Write;

use crate::webgl_wire_model::{Vocabulary, WireStruct, WireType, rust_identifier};

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
    writeln!(out, "    pub struct {rust_name} {{").unwrap();
    for field in &fields {
        writeln!(out, "        pub {}: {},", field.name, field.rust_type).unwrap();
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
    out.push_str("        };\n");
    let bool_field_offsets: Vec<String> = layout
        .fields
        .iter()
        .filter(|field| field.wire_type == WireType::Bool)
        .map(|field| field.offset.to_string())
        .collect();
    if !bool_field_offsets.is_empty() {
        writeln!(
            out,
            "        const BOOL_FIELD_OFFSETS: &'static [usize] = &[{}];",
            bool_field_offsets.join(", ")
        )
        .unwrap();
    }
    out.push_str("    }\n");
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
    writeln!(out, "pub enum {enum_name} {{").unwrap();
    for name in names {
        writeln!(out, "    {name},").unwrap();
    }
    out.push_str("}\n\n");
    writeln!(out, "impl {enum_name} {{").unwrap();
    out.push_str("    pub fn from_number(number: u16) -> Option<Self> {\n        Some(match number {\n");
    for (number, name) in names.iter().enumerate() {
        writeln!(out, "            {number} => Self::{name},").unwrap();
    }
    out.push_str("            _ => return None,\n        })\n    }\n}\n\n");
    // NB: Only the tests of the vocabulary ask how many types there are.
    writeln!(out, "#[cfg(test)]\npub const {count_name}: u16 = {};\n", names.len()).unwrap();
}

pub fn write_wire_vocabulary(vocabulary: &Vocabulary) -> String {
    let mut out = String::from("// Generated by build.rs from Libraries/LibCompositing/WebGL/GLFunctions.json.\n\n");

    let command_names: Vec<&str> = vocabulary
        .commands
        .iter()
        .map(|entry| entry.function.command_name())
        .collect();
    write_type_enum(
        &mut out,
        "CommandType",
        "COMMAND_TYPE_COUNT",
        "What a record of the command stream is.",
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
        "What a synchronous call is.",
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
        "pub const MAX_COMMAND_STRUCT_SIZE: usize = {max_command_struct_size};\n"
    )
    .unwrap();

    out.push_str("/// The structs of the records of the command stream.\n");
    out.push_str("pub mod commands {\n");
    out.push_str("    use core::mem::offset_of;\n\n");
    out.push_str("    use super::CommandType;\n");
    out.push_str("    use crate::webgl::wire::{Command, DataSpan, WireStruct};\n\n");
    for entry in &vocabulary.commands {
        let name = entry.function.command_name();
        write_struct(&mut out, name, &entry.layout);
        writeln!(out, "    impl Command for {name} {{").unwrap();
        writeln!(out, "        const TYPE: CommandType = CommandType::{name};").unwrap();
        out.push_str("    }\n\n");
    }
    out.push_str("}\n\n");

    out.push_str("/// The structs of the synchronous calls.\n");
    out.push_str("pub mod sync_calls {\n");
    out.push_str("    use core::mem::offset_of;\n\n");
    out.push_str("    use super::SyncCallType;\n");
    out.push_str("    use crate::webgl::wire::{DataSpan, SyncReply, SyncRequest, WireStruct};\n\n");
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

    out
}
