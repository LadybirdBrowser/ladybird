/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::ObjectFormat;
use crate::hash::HashMap;
use crate::intrinsic::{IntegerWidth, PairWidth};
use crate::target::ir::{MachineFunction as Handler, MachineInstruction, MachineOperand as Operand, MachineProgram};
use crate::target::registers::PhysicalRegister;
use std::fmt::Write;

/// Like `writeln!`, but without the `.unwrap()` -- writing to a `String` is infallible.
macro_rules! w {
    ($dst:expr) => { { let _ = writeln!($dst); } };
    ($dst:expr, $($arg:tt)*) => { { let _ = writeln!($dst, $($arg)*); } };
}
pub(crate) use w;

/// One entry per value of the opcode byte the interpreter dispatches on.
pub(crate) const DISPATCH_TABLE_SIZE: usize = 256;

#[derive(Clone, Copy)]
pub(crate) enum SimpleRegister {
    Native,
    Integer(IntegerWidth),
    Pair(PairWidth),
    Word,
    Single,
}

#[derive(Clone, Copy)]
pub(crate) enum SimpleOperand {
    Register(usize, SimpleRegister),
    Immediate(usize),
    HexImmediate(usize),
    HashImmediate(usize, bool),
    HashValue(i64),
    ShiftImmediate(usize),
    ShiftValue(u8),
    Label(usize),
    Resolved(usize, &'static str),
    ResolvedInteger(usize, IntegerWidth),
    Relocation(usize),
    Literal(&'static str),
    Value(i64),
}

pub(crate) type SimpleInstruction = (&'static str, [Option<SimpleOperand>; 4]);

macro_rules! simple {
    ($mnemonic:expr) => {
        ($mnemonic, [None, None, None, None])
    };
    ($mnemonic:expr; $first:expr) => {
        ($mnemonic, [Some($first), None, None, None])
    };
    ($mnemonic:expr; $first:expr, $second:expr) => {
        ($mnemonic, [Some($first), Some($second), None, None])
    };
    ($mnemonic:expr; $first:expr, $second:expr, $third:expr) => {
        ($mnemonic, [Some($first), Some($second), Some($third), None])
    };
    ($mnemonic:expr; $first:expr, $second:expr, $third:expr, $fourth:expr) => {
        ($mnemonic, [Some($first), Some($second), Some($third), Some($fourth)])
    };
}
pub(crate) use simple;

pub(crate) const fn native(index: usize) -> SimpleOperand {
    SimpleOperand::Register(index, SimpleRegister::Native)
}

pub(crate) const fn integer(index: usize, width: IntegerWidth) -> SimpleOperand {
    SimpleOperand::Register(index, SimpleRegister::Integer(width))
}

pub(crate) const fn pair(index: usize, width: PairWidth) -> SimpleOperand {
    SimpleOperand::Register(index, SimpleRegister::Pair(width))
}

pub(crate) const fn word(index: usize) -> SimpleOperand {
    SimpleOperand::Register(index, SimpleRegister::Word)
}

pub(crate) const fn single(index: usize) -> SimpleOperand {
    SimpleOperand::Register(index, SimpleRegister::Single)
}

fn simple_register_name(register: PhysicalRegister, format: SimpleRegister) -> String {
    match format {
        SimpleRegister::Native => register.as_str().to_string(),
        SimpleRegister::Integer(width) => register.integer_name(width),
        SimpleRegister::Pair(PairWidth::Word) => register.word_name(),
        SimpleRegister::Pair(PairWidth::DoubleWord) => register.as_str().to_string(),
        SimpleRegister::Word => register.word_name(),
        SimpleRegister::Single => register.single_name(),
    }
}

pub(crate) fn emit_simple_instruction(
    out: &mut String,
    instruction: &MachineInstruction,
    simple_instruction: Option<SimpleInstruction>,
    mut resolve_operand: impl FnMut(&Operand) -> String,
) -> bool {
    let Some((mnemonic, operands)) = simple_instruction else {
        return false;
    };
    let _ = write!(out, "    {mnemonic}");
    for (index, operand) in operands.into_iter().flatten().enumerate() {
        let _ = write!(out, "{}", if index == 0 { " " } else { ", " });
        match operand {
            SimpleOperand::Register(index, format) => {
                let _ = write!(
                    out,
                    "{}",
                    simple_register_name(instruction.physical_register(index), format)
                );
            }
            SimpleOperand::Immediate(index) => {
                let _ = write!(out, "{}", instruction.immediate(index));
            }
            SimpleOperand::HexImmediate(index) => {
                let _ = write!(out, "{:#x}", instruction.immediate(index));
            }
            SimpleOperand::HashImmediate(index, hexadecimal) => {
                let immediate = instruction.immediate(index);
                if hexadecimal {
                    let _ = write!(out, "#0x{immediate:x}");
                } else {
                    let _ = write!(out, "#{immediate}");
                }
            }
            SimpleOperand::HashValue(value) => {
                let _ = write!(out, "#{value}");
            }
            SimpleOperand::ShiftImmediate(index) => {
                let _ = write!(out, "lsl #{}", instruction.immediate(index));
            }
            SimpleOperand::ShiftValue(value) => {
                let _ = write!(out, "lsl #{value}");
            }
            SimpleOperand::Label(index) => {
                let _ = write!(out, "{}", resolve_operand(instruction.operand(index)));
            }
            SimpleOperand::Resolved(index, prefix) => {
                let _ = write!(out, "{prefix}{}", resolve_operand(instruction.operand(index)));
            }
            SimpleOperand::ResolvedInteger(index, width) => {
                if let Operand::PhysicalRegister(register) = instruction.operand(index) {
                    let _ = write!(out, "{}", register.integer_name(width));
                } else {
                    let _ = write!(out, "{}", resolve_operand(instruction.operand(index)));
                }
            }
            SimpleOperand::Relocation(index) => {
                let _ = write!(out, "CSYM({})", instruction.relocation(index));
            }
            SimpleOperand::Literal(value) => {
                let _ = write!(out, "{value}");
            }
            SimpleOperand::Value(value) => {
                let _ = write!(out, "{value}");
            }
        }
    }
    w!(out);
    true
}

pub(crate) fn emit_label(out: &mut String, instruction: &MachineInstruction, handler: &Handler) {
    let Some(Operand::Label(name)) = instruction.operands.first() else {
        unreachable!("machine label operand was verified")
    };
    if name.starts_with('.') {
        w!(out, ".Lasm_{}{name}:", handler.name);
    } else {
        w!(out, "{name}:");
    }
}

pub(crate) fn emit_file_header(out: &mut String, comment_prefix: &str, intel_syntax: bool) {
    w!(out, "{comment_prefix} Generated by flapc -- DO NOT EDIT");
    if intel_syntax {
        w!(out, ".intel_syntax noprefix");
    }
    w!(out);
    w!(out, "#ifdef __APPLE__");
    w!(out, "#define CSYM(name) _##name");
    w!(out, "#else");
    w!(out, "#define CSYM(name) name");
    w!(out, "#endif");
    w!(out);
    w!(out, ".text");
    w!(out);
}

/// Resolve a label operand to its full name (with handler prefix for local labels).
pub(crate) fn resolve_label(op: &Operand, handler: &Handler) -> String {
    match op {
        Operand::Label(name) => {
            if name.starts_with('.') {
                format!(".Lasm_{}{name}", handler.name)
            } else {
                name.to_string()
            }
        }
        other => panic!("expected label operand in handler '{}', got {other:?}", handler.name),
    }
}

pub(crate) fn emit_dispatch_tables(out: &mut String, program: &MachineProgram) {
    match program.target.object_format {
        ObjectFormat::MachO => w!(out, ".section __DATA,__const"),
        ObjectFormat::Elf => w!(out, ".section .data.rel.ro"),
        ObjectFormat::Coff => w!(out, ".section .rdata,\"dr\""),
    }
    w!(out, ".p2align 3");
    w!(out, "asm_dispatch_table:");

    let handler_names = program
        .functions
        .iter()
        .map(|handler| (handler.id, handler.name.as_str()))
        .collect::<HashMap<_, _>>();
    for handler in &program.dispatch_handlers {
        let handler_name = handler
            .and_then(|handler| handler_names.get(&handler).copied())
            .unwrap_or("fallback");
        w!(out, "    .quad asm_handler_{handler_name}");
    }
    for _ in program.dispatch_handlers.len()..DISPATCH_TABLE_SIZE {
        w!(out, "    .quad asm_handler_fallback");
    }
    w!(out);

    w!(out, "asm_debug_dispatch_table:");
    for _ in 0..DISPATCH_TABLE_SIZE {
        w!(out, "    .quad asm_debugger_trampoline");
    }
    w!(out);

    w!(out, ".globl CSYM(js_interpreter_handler_ranges)");
    w!(out, ".p2align 3");
    w!(out, "CSYM(js_interpreter_handler_ranges):");
    for handler in &program.dispatch_handlers {
        let handler_name = handler
            .and_then(|handler| handler_names.get(&handler).copied())
            .unwrap_or("fallback");
        emit_handler_range(out, handler_name);
    }
    for _ in program.dispatch_handlers.len()..DISPATCH_TABLE_SIZE {
        emit_handler_range(out, "fallback");
    }
    w!(out);
    w!(out, ".text");
    w!(out);
}

pub(crate) fn emit_handler_range(out: &mut String, handler_name: &str) {
    w!(out, "    .quad asm_handler_{handler_name}");
    w!(out, "    .quad asm_handler_{handler_name}_hot_end");
    w!(out, "    .quad asm_handler_{handler_name}_cold");
    w!(out, "    .quad asm_handler_{handler_name}_cold_end");
}

pub(crate) fn emit_handlers(
    out: &mut String,
    program: &MachineProgram,
    comment_prefix: &str,
    mut emit_alignment: impl FnMut(&mut String, bool),
    mut emit_instruction: impl FnMut(&mut String, &MachineInstruction, &Handler),
    emit_between_hot_and_cold: impl FnOnce(&mut String),
) {
    let mut cold_handlers = String::new();
    for handler in &program.functions {
        if handler.is_cold {
            emit_alignment(&mut cold_handlers, true);
            w!(cold_handlers, "asm_handler_{}:", handler.name);
            for instruction in &handler.hot_instructions {
                emit_instruction(&mut cold_handlers, instruction, handler);
            }
            w!(cold_handlers, "asm_handler_{}_hot_end:", handler.name);
            w!(cold_handlers, "asm_handler_{}_cold:", handler.name);
            for instruction in &handler.cold_instructions {
                emit_instruction(&mut cold_handlers, instruction, handler);
            }
            w!(cold_handlers, "asm_handler_{}_cold_end:", handler.name);
            w!(cold_handlers);
            continue;
        }

        emit_alignment(out, false);
        w!(out, "asm_handler_{}:", handler.name);
        for instruction in &handler.hot_instructions {
            emit_instruction(out, instruction, handler);
        }
        w!(out, "asm_handler_{}_hot_end:", handler.name);
        w!(out);

        w!(cold_handlers, "asm_handler_{}_cold:", handler.name);
        if !handler.cold_instructions.is_empty() {
            w!(cold_handlers, "{comment_prefix} Cold paths for {}", handler.name);
            for instruction in &handler.cold_instructions {
                emit_instruction(&mut cold_handlers, instruction, handler);
            }
        }
        w!(cold_handlers, "asm_handler_{}_cold_end:", handler.name);
    }
    emit_between_hot_and_cold(out);
    if !cold_handlers.is_empty() {
        w!(out, "{comment_prefix} Cold handler paths");
        w!(out, "asm_cold_handler_paths:");
        out.push_str(&cold_handlers);
    }
}

pub(crate) fn emit_program(
    mut out: String,
    program: &MachineProgram,
    seh_indentation: &str,
    emit_entry: impl FnOnce(&mut String),
    emit_fallback: impl FnOnce(&mut String),
    emit_handler_bodies: impl FnOnce(&mut String),
    emit_exit: impl FnOnce(&mut String),
) -> String {
    emit_dispatch_tables(&mut out, program);
    emit_proc_start(&mut out, program.target.object_format, seh_indentation);
    emit_entry(&mut out);
    emit_fallback(&mut out);
    emit_handler_bodies(&mut out);
    emit_exit(&mut out);
    emit_proc_end(&mut out, program.target.object_format);
    emit_file_trailer(&mut out, program.target.object_format);
    out
}

pub(crate) fn emit_proc_start(out: &mut String, format: ObjectFormat, seh_indentation: &str) {
    w!(out, ".globl CSYM(js_interpreter)");
    w!(out, ".p2align 4");
    w!(out, "CSYM(js_interpreter):");
    match format {
        ObjectFormat::Coff => w!(out, "{seh_indentation}.seh_proc CSYM(js_interpreter)"),
        ObjectFormat::Elf | ObjectFormat::MachO => w!(out, "    .cfi_startproc"),
    }
}

pub(crate) fn emit_proc_end(out: &mut String, format: ObjectFormat) {
    match format {
        ObjectFormat::Coff => w!(out, "    .seh_endproc"),
        ObjectFormat::Elf | ObjectFormat::MachO => w!(out, ".cfi_endproc"),
    }
    w!(out);
}

pub(crate) fn emit_file_trailer(out: &mut String, format: ObjectFormat) {
    if matches!(format, ObjectFormat::Elf) {
        w!(out, ".section .note.GNU-stack,\"\",@progbits");
    }
}
