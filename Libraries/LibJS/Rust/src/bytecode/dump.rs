/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;

use super::instruction::dump_instruction_from_bytes;
use super::instruction::instruction_is_terminator_from_opcode;
use super::instruction::instruction_length_from_bytes;
use super::instruction::visit_labels_from_bytes;
use super::operand::Operand;
use super::validator::read_u32;
use crate::abort_on_panic;
use crate::runtime::value::EncodedValue;
use crate::runtime::value::EncodedValueKind;

#[repr(C)]
pub struct FFIDumpExceptionHandler {
    pub start_offset: usize,
    pub end_offset: usize,
    pub handler_offset: usize,
}

#[repr(C)]
pub struct FFIBytecodeDumpCallbacks {
    pub append: unsafe extern "C" fn(ctx: *mut c_void, data: *const u8, len: usize),
    pub append_local: unsafe extern "C" fn(ctx: *mut c_void, index: u32),
    pub append_identifier: unsafe extern "C" fn(ctx: *mut c_void, index: u32, quoted: bool),
    pub append_property_key: unsafe extern "C" fn(ctx: *mut c_void, index: u32, quoted: bool),
    pub append_string: unsafe extern "C" fn(ctx: *mut c_void, index: u32),
    pub append_value_double: unsafe extern "C" fn(ctx: *mut c_void, value: f64),
    pub append_value_string: unsafe extern "C" fn(ctx: *mut c_void, encoded: u64),
    pub append_value_bigint: unsafe extern "C" fn(ctx: *mut c_void, encoded: u64),
    pub append_value_fallback: unsafe extern "C" fn(ctx: *mut c_void, encoded: u64),
}

#[repr(C)]
pub struct FFIBytecodeDumpMetadata {
    pub number_of_registers: u32,
    pub registers_and_locals_count: u32,
    pub local_index_base: u32,
    pub argument_index_base: u32,
    pub constants: *const u64,
    pub constant_count: usize,
}

pub struct BytecodeDumper<'a> {
    ctx: *mut c_void,
    callbacks: &'a FFIBytecodeDumpCallbacks,
    metadata: &'a FFIBytecodeDumpMetadata,
    constants: &'a [u64],
    basic_block_start_offsets: &'a [u32],
    first_piece: bool,
}

impl<'a> BytecodeDumper<'a> {
    fn new(
        ctx: *mut c_void,
        callbacks: &'a FFIBytecodeDumpCallbacks,
        metadata: &'a FFIBytecodeDumpMetadata,
        constants: &'a [u64],
        basic_block_start_offsets: &'a [u32],
    ) -> Self {
        Self {
            ctx,
            callbacks,
            metadata,
            constants,
            basic_block_start_offsets,
            first_piece: true,
        }
    }

    pub fn append(&mut self, text: &str) {
        unsafe {
            (self.callbacks.append)(self.ctx, text.as_ptr(), text.len());
        }
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
        if raw < self.metadata.number_of_registers {
            if raw == 2 {
                self.append("\x1b[33mthis\x1b[0m");
            } else {
                self.append("\x1b[33mreg");
                self.append(&raw.to_string());
                self.append("\x1b[0m");
            }
        } else if raw < self.metadata.registers_and_locals_count {
            let index = raw - self.metadata.local_index_base;
            self.append("\x1b[34m");
            unsafe {
                (self.callbacks.append_local)(self.ctx, index);
            }
            self.append("~");
            self.append(&index.to_string());
            self.append("\x1b[0m");
        } else if raw < self.metadata.argument_index_base {
            let index = raw - self.metadata.registers_and_locals_count;
            self.append_value(self.constants[index as usize]);
        } else {
            let index = raw - self.metadata.argument_index_base;
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
                unsafe {
                    (self.callbacks.append_value_double)(self.ctx, value);
                }
                self.append(")");
            }
            EncodedValueKind::BigInt => {
                self.append("BigInt(");
                unsafe {
                    (self.callbacks.append_value_bigint)(self.ctx, encoded);
                }
                self.append(")");
            }
            EncodedValueKind::String => {
                self.append("String(\"");
                unsafe {
                    (self.callbacks.append_value_string)(self.ctx, encoded);
                }
                self.append("\")");
            }
            EncodedValueKind::Undefined => self.append("Undefined"),
            EncodedValueKind::Null => self.append("Null"),
            EncodedValueKind::Other => {
                self.append("Value(");
                unsafe {
                    (self.callbacks.append_value_fallback)(self.ctx, encoded);
                }
                self.append(")");
            }
        }
    }

    fn append_value_without_side_effects(&mut self, encoded: u64) {
        match EncodedValue::from_encoded(encoded).kind() {
            EncodedValueKind::Empty => self.append("<empty>"),
            EncodedValueKind::Boolean(value) => self.append(if value { "true" } else { "false" }),
            EncodedValueKind::Int32(value) => self.append(&value.to_string()),
            EncodedValueKind::Double(value) => unsafe {
                (self.callbacks.append_value_double)(self.ctx, value);
            },
            EncodedValueKind::BigInt => unsafe {
                (self.callbacks.append_value_bigint)(self.ctx, encoded);
            },
            EncodedValueKind::String => unsafe {
                (self.callbacks.append_value_string)(self.ctx, encoded);
            },
            EncodedValueKind::Undefined => self.append("undefined"),
            EncodedValueKind::Null => self.append("null"),
            EncodedValueKind::Other => unsafe {
                (self.callbacks.append_value_fallback)(self.ctx, encoded);
            },
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

    pub fn append_identifier_quoted(&mut self, index: u32) {
        unsafe {
            (self.callbacks.append_identifier)(self.ctx, index, true);
        }
    }

    pub fn append_identifier_plain(&mut self, index: u32) {
        unsafe {
            (self.callbacks.append_identifier)(self.ctx, index, false);
        }
    }

    pub fn append_property_key_quoted(&mut self, index: u32) {
        unsafe {
            (self.callbacks.append_property_key)(self.ctx, index, true);
        }
    }

    pub fn append_property_key_plain(&mut self, index: u32) {
        unsafe {
            (self.callbacks.append_property_key)(self.ctx, index, false);
        }
    }

    pub fn append_string(&mut self, index: u32) {
        unsafe {
            (self.callbacks.append_string)(self.ctx, index);
        }
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

    pub fn append_exception_handlers(&mut self, exception_handlers: &[FFIDumpExceptionHandler]) {
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
            self.append_label("", handler.handler_offset as u32);
            self.append("\n");
        }
    }
}

fn collect_basic_block_start_offsets(bytecode: &[u8], exception_handlers: &[FFIDumpExceptionHandler]) -> Vec<u32> {
    let mut offsets = vec![0];
    let append_offset = |offsets: &mut Vec<u32>, offset: usize| {
        let offset = u32::try_from(offset).expect("bytecode offset exceeds u32::MAX");
        if !offsets.contains(&offset) {
            offsets.push(offset);
        }
    };
    let append_instruction_offset = |offsets: &mut Vec<u32>, offset: usize| {
        if offset < bytecode.len() {
            append_offset(offsets, offset);
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
///
/// # Safety
/// `bytecode_ptr` must point to `bytecode_len` bytes of validated bytecode, or
/// be null when `bytecode_len` is zero. `exception_handlers` must point to
/// `exception_handler_count` valid entries, or be null when the count is zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_count_basic_blocks(
    bytecode_ptr: *const u8,
    bytecode_len: usize,
    exception_handlers: *const FFIDumpExceptionHandler,
    exception_handler_count: usize,
) -> usize {
    abort_on_panic(|| unsafe {
        let bytecode = if bytecode_len == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(bytecode_ptr, bytecode_len)
        };
        let exception_handlers = if exception_handler_count == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(exception_handlers, exception_handler_count)
        };
        collect_basic_block_start_offsets(bytecode, exception_handlers).len()
    })
}

/// Dump a validated bytecode instruction stream through C++ formatting callbacks.
///
/// # Safety
/// `bytecode_ptr` must point to `bytecode_len` bytes of validated bytecode, or
/// be null when `bytecode_len` is zero. `exception_handlers` must point to
/// `exception_handler_count` valid entries, or be null when the count is zero.
/// `metadata` and `callbacks` must point to valid structs, and every callback
/// must remain callable for the duration of this function. `metadata.constants`
/// must point to `metadata.constant_count` encoded Values, or be null when the
/// count is zero. `ctx` is passed through to callbacks and must remain valid
/// for their requirements.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dump_bytecode(
    bytecode_ptr: *const u8,
    bytecode_len: usize,
    exception_handlers: *const FFIDumpExceptionHandler,
    exception_handler_count: usize,
    metadata: *const FFIBytecodeDumpMetadata,
    ctx: *mut c_void,
    callbacks: *const FFIBytecodeDumpCallbacks,
) {
    abort_on_panic(|| unsafe {
        let bytecode = if bytecode_len == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(bytecode_ptr, bytecode_len)
        };
        let exception_handlers = if exception_handler_count == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(exception_handlers, exception_handler_count)
        };
        let metadata = metadata.as_ref().expect("rust_dump_bytecode metadata must not be null");
        let constants = if metadata.constant_count == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(metadata.constants, metadata.constant_count)
        };
        let callbacks = callbacks
            .as_ref()
            .expect("rust_dump_bytecode callbacks must not be null");
        let basic_block_start_offsets = collect_basic_block_start_offsets(bytecode, exception_handlers);
        let mut dumper = BytecodeDumper::new(ctx, callbacks, metadata, constants, &basic_block_start_offsets);
        let mut basic_block_offset_index = 0;

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

            at += instruction_length_from_bytes(bytecode[at], bytecode, at)
                .expect("validated bytecode should have valid instruction lengths");
        }

        dumper.append_exception_handlers(exception_handlers);
    });
}
