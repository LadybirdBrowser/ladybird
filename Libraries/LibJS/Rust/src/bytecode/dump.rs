/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::executable::{Executable, append_double_formatted_like_ak, append_value_formatted_like_ak};
use super::generator::ExceptionHandler;
use super::instruction::dump_instruction_from_bytes;
use super::instruction::instruction_is_terminator_from_opcode;
use super::instruction::instruction_length_from_bytes;
use super::instruction::instruction_name_from_opcode;
use super::instruction::visit_labels_from_bytes;
use super::native_disassembler::disassemble;
use super::operand::Operand;
use super::validator::read_u32;
use crate::bytecode::encoded_value::EncodedValue;
use crate::bytecode::encoded_value::EncodedValueKind;
use crate::interpreter::run::should_dump_interpreter_assembly;
use crate::layout::value::Value;
use crate::utf16::Utf16View;

/// Where the native code of an opcode's handler lives, which flapc emits with the interpreter.
#[repr(C)]
struct InterpreterHandlerRange {
    hot_start: *const u8,
    hot_end: *const u8,
    cold_start: *const u8,
    cold_end: *const u8,
}

unsafe extern "C" {
    safe static js_interpreter_handler_ranges: [InterpreterHandlerRange; 256];
}

impl InterpreterHandlerRange {
    fn hot_code(&'static self) -> &'static [u8] {
        // SAFETY: flapc delimits the handler's hot code with these, and the interpreter is mapped for as long as the
        // process runs.
        unsafe { native_code_between(self.hot_start, self.hot_end) }
    }

    fn cold_code(&'static self) -> &'static [u8] {
        // SAFETY: flapc delimits the handler's cold code with these, and the interpreter is mapped for as long as the
        // process runs.
        unsafe { native_code_between(self.cold_start, self.cold_end) }
    }
}

/// # Safety
///
/// start and end must delimit native code that stays mapped for as long as the process runs.
unsafe fn native_code_between(start: *const u8, end: *const u8) -> &'static [u8] {
    let start_address = start as usize;
    let end_address = end as usize;
    assert!(end_address >= start_address, "native code range must be ordered");
    if start_address == end_address {
        return &[];
    }
    // SAFETY: The caller guarantees the range.
    unsafe { ak::slice_from_raw_parts(start, end_address - start_address) }
}

struct ColdCodeRange {
    code: &'static [u8],
    owners: Vec<&'static str>,
}

pub struct BytecodeDumper<'a> {
    output: &'a mut Vec<u8>,
    executable: &'a Executable,
    basic_block_start_offsets: &'a [u32],
    first_piece: bool,
}

fn append_native_code(dumper: &mut BytecodeDumper<'_>, code: &[u8]) {
    if code.is_empty() {
        return;
    }

    let instructions = disassemble(code, code.as_ptr() as u64);
    if instructions.is_empty() {
        dumper.append("          <native disassembly unavailable>\n");
        return;
    }
    for instruction in instructions {
        let instruction_bytes = &code[instruction.offset..instruction.offset + instruction.length];
        dumper.append("          ");
        dumper.append(&format!("{:016x}  ", instruction.address));
        for byte in instruction_bytes {
            dumper.append(&format!("{byte:02x} "));
        }
        for _ in instruction_bytes.len()..15 {
            dumper.append("   ");
        }
        dumper.append(" ");
        dumper.append(&instruction.text);
        dumper.append("\n");
    }
}

fn record_cold_code_range(ranges: &mut Vec<ColdCodeRange>, code: &'static [u8], owner: &'static str) {
    if code.is_empty() {
        return;
    }
    if let Some(range) = ranges.iter_mut().find(|range| std::ptr::eq(range.code, code)) {
        if !range.owners.contains(&owner) {
            range.owners.push(owner);
        }
        return;
    }
    ranges.push(ColdCodeRange {
        code,
        owners: vec![owner],
    });
}

fn append_cold_code_ranges(dumper: &mut BytecodeDumper<'_>, ranges: &[ColdCodeRange]) {
    if ranges.is_empty() {
        return;
    }

    dumper.append("\n\x1b[36;1mCold native paths\x1b[0m:\n");
    for range in ranges {
        dumper.append("\n  \x1b[36m");
        dumper.append(&range.owners.join(", "));
        dumper.append("\x1b[0m:\n");
        append_native_code(dumper, range.code);
    }
}

impl BytecodeDumper<'_> {
    pub fn append(&mut self, text: &str) {
        self.output.extend_from_slice(text.as_bytes());
    }

    pub fn begin_instruction(&mut self, name: &str) {
        self.first_piece = true;
        self.append(name);
    }

    pub fn append_piece(&mut self, append_piece: impl FnOnce(&mut Self)) {
        if self.first_piece {
            self.append(" ");
            self.first_piece = false;
        } else {
            self.append(", ");
        }
        append_piece(self);
    }

    pub fn append_operand(&mut self, name: &str, operand: Operand) {
        if !name.is_empty() {
            self.append("\x1b[32m");
            self.append(name);
            self.append("\x1b[0m:");
        }

        let raw = operand.raw();
        if raw < self.executable.number_of_registers {
            if raw == 2 {
                self.append("\x1b[33mthis\x1b[0m");
            } else {
                self.append("\x1b[33mreg");
                self.append(&raw.to_string());
                self.append("\x1b[0m");
            }
        } else if raw < self.executable.registers_and_locals_count() {
            let index = raw - self.executable.local_index_base();
            self.append("\x1b[34m");
            Utf16View::of_fly_string(&self.executable.local_variable_names[index as usize])
                .append_as_wtf8_to(self.output);
            self.append("~");
            self.append(&index.to_string());
            self.append("\x1b[0m");
        } else if raw < self.executable.argument_index_base() {
            let index = raw - self.executable.registers_and_locals_count();
            self.append_value(self.executable.constants()[index as usize].0);
        } else {
            let index = raw - self.executable.argument_index_base();
            self.append("\x1b[34marg");
            self.append(&index.to_string());
            self.append("\x1b[0m");
        }
    }

    fn append_value(&mut self, encoded: u64) {
        match EncodedValue::from_encoded(encoded).kind() {
            EncodedValueKind::Empty => self.append("<Empty>"),
            EncodedValueKind::Boolean(value) => {
                self.append("Bool(");
                self.append(if value { "true" } else { "false" });
                self.append(")");
            }
            EncodedValueKind::Int32(value) => {
                self.append("Int32(");
                self.append(&value.to_string());
                self.append(")");
            }
            EncodedValueKind::Double(value) => {
                self.append("Double(");
                self.append_double(value);
                self.append(")");
            }
            EncodedValueKind::BigInt => {
                self.append("BigInt(");
                self.append_bigint(encoded);
                self.append(")");
            }
            EncodedValueKind::String => {
                self.append("String(\"");
                self.append_string_value(encoded);
                self.append("\")");
            }
            EncodedValueKind::Undefined => self.append("Undefined"),
            EncodedValueKind::Null => self.append("Null"),
            EncodedValueKind::Other => {
                self.append("Value(");
                self.append_value_fallback(encoded);
                self.append(")");
            }
        }
    }

    fn append_value_without_side_effects(&mut self, encoded: u64) {
        match EncodedValue::from_encoded(encoded).kind() {
            EncodedValueKind::Empty => self.append("<empty>"),
            EncodedValueKind::Boolean(value) => self.append(if value { "true" } else { "false" }),
            EncodedValueKind::Int32(value) => self.append(&value.to_string()),
            EncodedValueKind::Double(value) => self.append_double(value),
            EncodedValueKind::BigInt => self.append_bigint(encoded),
            EncodedValueKind::String => self.append_string_value(encoded),
            EncodedValueKind::Undefined => self.append("undefined"),
            EncodedValueKind::Null => self.append("null"),
            EncodedValueKind::Other => self.append_value_fallback(encoded),
        }
    }

    pub fn append_label(&mut self, name: &str, address: u32) {
        if !name.is_empty() {
            self.append("\x1b[32m");
            self.append(name);
            self.append("\x1b[0m:");
        }

        if let Ok(index) = self.basic_block_start_offsets.binary_search(&address) {
            self.append("\x1b[35mblock");
            self.append(&index.to_string());
            self.append("\x1b[0m");
        } else {
            self.append("@");
            self.append(&format!("{address:x}"));
        }
    }

    fn append_double(&mut self, value: f64) {
        append_double_formatted_like_ak(self.output, value);
    }

    fn append_bigint(&mut self, encoded: u64) {
        Utf16View::of_string(&Value::from_bits(encoded).as_bigint().to_utf16_string()).append_as_wtf8_to(self.output);
    }

    fn append_string_value(&mut self, encoded: u64) {
        Utf16View::of_string(&Value::from_bits(encoded).as_string().utf16_string()).append_as_wtf8_to(self.output);
    }

    fn append_value_fallback(&mut self, encoded: u64) {
        append_value_formatted_like_ak(self.output, Value::from_bits(encoded));
    }

    fn append_quoted(&mut self, text: Utf16View<'_>) {
        self.append("\x1b[36m`");
        text.append_as_wtf8_to(self.output);
        self.append("`\x1b[0m");
    }

    pub fn append_identifier_quoted(&mut self, index: u32) {
        self.append_quoted(Utf16View::of_fly_string(
            &self.executable.identifier_table[index as usize],
        ));
    }

    pub fn append_identifier_plain(&mut self, index: u32) {
        Utf16View::of_fly_string(&self.executable.identifier_table[index as usize]).append_as_wtf8_to(self.output);
    }

    /// The table holds the property keys as the strings they were made from. Formatting a PropertyKey writes an array
    /// index key as its number, which is the same text, since only canonical index strings become numbers.
    pub fn append_property_key_quoted(&mut self, index: u32) {
        self.append_quoted(Utf16View::of_fly_string(
            &self.executable.property_key_table()[index as usize],
        ));
    }

    pub fn append_property_key_plain(&mut self, index: u32) {
        Utf16View::of_fly_string(&self.executable.property_key_table()[index as usize]).append_as_wtf8_to(self.output);
    }

    pub fn append_string(&mut self, index: u32) {
        Utf16View::of_fly_string(&self.executable.string_table[index as usize]).append_as_wtf8_to(self.output);
    }

    pub fn append_bool(&mut self, name: &str, value: bool) {
        self.append(name);
        self.append(":");
        self.append(if value { "true" } else { "false" });
    }

    pub fn append_number(&mut self, name: &str, value: impl std::fmt::Display) {
        self.append(name);
        self.append(":");
        self.append(&value.to_string());
    }

    pub fn append_put_kind(&mut self, name: &str, value: u32) {
        let kind = match value {
            0 => "Normal",
            1 => "Getter",
            2 => "Setter",
            3 => "Prototype",
            4 => "Own",
            _ => unreachable!("invalid PutKind"),
        };
        self.append(name);
        self.append(":");
        self.append(kind);
    }

    pub fn append_operand_list(&mut self, name: &str, bytes: &[u8], offset: usize, count: usize) {
        self.append("\x1b[32m");
        self.append(name);
        self.append("\x1b[0m:[");
        for i in 0..count {
            if i != 0 {
                self.append(", ");
            }
            self.append_operand("", Operand::from_raw(read_u32(bytes, offset + i * 4)));
        }
        self.append("]");
    }

    pub fn append_optional_operand_list(&mut self, name: &str, bytes: &[u8], offset: usize, count: usize) {
        self.append(name);
        self.append(":[");
        let mut first_elem = true;
        for i in 0..count {
            let raw = read_u32(bytes, offset + i * 4);
            let Some(operand) = Operand::optional_from_raw(raw) else {
                continue;
            };
            if !first_elem {
                self.append(", ");
            }
            first_elem = false;
            self.append_operand(name, operand);
        }
        self.append("]");
    }

    pub fn append_label_list(&mut self, name: &str, bytes: &[u8], offset: usize, count: usize) {
        self.append(name);
        self.append(":[");
        for i in 0..count {
            if i != 0 {
                self.append(", ");
            }
            self.append_label("", read_u32(bytes, offset + i * 4));
        }
        self.append("]");
    }

    pub fn append_optional_label_list(&mut self, name: &str, bytes: &[u8], offset: usize, count: usize) {
        self.append(name);
        self.append(":[");
        let mut first_elem = true;
        for i in 0..count {
            let element_offset = offset + i * 8;
            if bytes[element_offset + 4] == 0 {
                continue;
            }
            if !first_elem {
                self.append(", ");
            }
            first_elem = false;
            self.append_label("", read_u32(bytes, element_offset));
        }
        self.append("]");
    }

    pub fn append_value_list(&mut self, name: &str, bytes: &[u8], offset: usize, count: usize) {
        if !name.is_empty() {
            self.append("\x1b[32m");
            self.append(name);
            self.append("\x1b[0m:[");
        }
        for i in 0..count {
            if i != 0 {
                self.append(", ");
            }
            let value = u64::from_ne_bytes(bytes[offset + i * 8..offset + (i + 1) * 8].try_into().unwrap());
            self.append_value_without_side_effects(value);
        }
        self.append("]");
    }

    pub fn append_exception_handlers(&mut self, exception_handlers: &[ExceptionHandler]) {
        if exception_handlers.is_empty() {
            return;
        }

        self.append("\nException handlers:\n");
        for handler in exception_handlers {
            self.append("  [");
            self.append(&format!("{:4x}", handler.start_offset));
            self.append(" .. ");
            self.append(&format!("{:4x}", handler.end_offset));
            self.append("] => handler ");
            self.append_label("", handler.handler_offset);
            self.append("\n");
        }
    }
}

fn collect_basic_block_start_offsets(bytecode: &[u8], exception_handlers: &[ExceptionHandler]) -> Vec<u32> {
    let mut offsets = vec![0];
    let append_offset = |offsets: &mut Vec<u32>, offset: usize| {
        let offset = u32::try_from(offset).expect("bytecode offset exceeds u32::MAX");
        if !offsets.contains(&offset) {
            offsets.push(offset);
        }
    };
    let append_instruction_offset = |offsets: &mut Vec<u32>, offset: u32| {
        if (offset as usize) < bytecode.len() {
            append_offset(offsets, offset as usize);
        }
    };

    let mut at = 0;
    while at < bytecode.len() {
        let opcode = bytecode[at];
        let length = instruction_length_from_bytes(opcode, bytecode, at)
            .expect("validated bytecode should have valid instruction lengths");
        let next_offset = at + length;

        visit_labels_from_bytes(opcode, bytecode, at, &mut |address| {
            append_offset(&mut offsets, address as usize);
        });

        if instruction_is_terminator_from_opcode(opcode) && next_offset < bytecode.len() {
            append_offset(&mut offsets, next_offset);
        }

        at = next_offset;
    }

    for handler in exception_handlers {
        append_instruction_offset(&mut offsets, handler.start_offset);
        append_instruction_offset(&mut offsets, handler.end_offset);
        append_instruction_offset(&mut offsets, handler.handler_offset);
    }

    offsets.sort_unstable();
    offsets
}

/// Count basic blocks in a validated bytecode instruction stream.
pub fn count_basic_blocks(executable: &Executable) -> usize {
    collect_basic_block_start_offsets(executable.bytecode(), &executable.exception_handlers).len()
}

/// Dump the validated bytecode of an executable, and with LADYBIRD_JS_INTERPRETER_DUMP set, the native code of the
/// interpreter handler of each instruction.
pub fn dump_bytecode(output: &mut Vec<u8>, executable: &Executable) {
    let bytecode = executable.bytecode();
    let exception_handlers = &executable.exception_handlers;
    let dump_interpreter = should_dump_interpreter_assembly();
    let basic_block_start_offsets = collect_basic_block_start_offsets(bytecode, exception_handlers);
    let mut dumper = BytecodeDumper {
        output,
        executable,
        basic_block_start_offsets: &basic_block_start_offsets,
        first_piece: true,
    };
    let mut basic_block_offset_index = 0;
    let mut cold_code_ranges = Vec::new();

    let mut at = 0;
    while at < bytecode.len() {
        if basic_block_offset_index < basic_block_start_offsets.len()
            && at == basic_block_start_offsets[basic_block_offset_index] as usize
        {
            if basic_block_offset_index > 0 {
                dumper.append("\n");
            }
            dumper.append("\x1b[35;1mblock");
            dumper.append(&basic_block_offset_index.to_string());
            dumper.append("\x1b[0m:\n");
            basic_block_offset_index += 1;
        }

        dumper.append("  [");
        dumper.append(&format!("{at:4x}"));
        dumper.append("] ");
        dump_instruction_from_bytes(bytecode[at], bytecode, at, &mut dumper);
        dumper.append("\n");

        if dump_interpreter {
            let handler_range = &js_interpreter_handler_ranges[bytecode[at] as usize];
            append_native_code(&mut dumper, handler_range.hot_code());
            record_cold_code_range(
                &mut cold_code_ranges,
                handler_range.cold_code(),
                instruction_name_from_opcode(bytecode[at]),
            );
        }

        at += instruction_length_from_bytes(bytecode[at], bytecode, at)
            .expect("validated bytecode should have valid instruction lengths");
    }

    dumper.append_exception_handlers(exception_handlers);
    append_cold_code_ranges(&mut dumper, &cold_code_ranges);
}
