/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod compiler;

use compiler::CraneliftCompiler;

/// Immediates:
///   constants:    imm1 = value (i32 sign-extended, i64, or f32/f64 bits)
///   local ops:    imm1 = local index
///   branch:       imm1 = label index (from control stack)
///   block/loop:   imm1 = end_ip, imm2 = else_ip (-1 if none), imm3 = arity | (param_count << 16)
///   call:         imm1 = function index
///   memory ops:   imm1 = offset, imm3 = memory index
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct CraneliftInsn {
    pub opcode: u64,
    pub sources: [u8; 3],
    pub destination: u8,
    pub imm1: i64,
    pub imm2: i64,
    pub imm3: u32,
    pub _pad: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct RuntimeHelpers {
    // i32 fn(interp, config, func_index); returns 1 on trap
    pub call_function: usize,
    // void fn(interp, msg_ptr, msg_len)
    pub set_trap: usize,
    // i64 fn(config, mem_idx, addr)
    pub memory_load8_s: usize,
    pub memory_load8_u: usize,
    pub memory_load16_s: usize,
    pub memory_load16_u: usize,
    pub memory_load32_s: usize,
    pub memory_load32_u: usize,
    pub memory_load64: usize,
    // i32 fn(config, mem_idx, addr, value); returns 1 on OOB
    pub memory_store8: usize,
    pub memory_store16: usize,
    pub memory_store32: usize,
    pub memory_store64: usize,
    // i64 fn(config, mem_idx); returns size in pages
    pub memory_size: usize,
    // i32 fn(config, mem_idx, pages); returns old size or -1
    pub memory_grow: usize,
    // i64 fn(config, index)
    pub read_global: usize,
    // void fn(config, index, value)
    pub write_global: usize,
    // void fn(config, value)
    pub stack_push: usize,
    // i64 fn(config)
    pub stack_pop: usize,
    pub stack_size: usize,
    // void fn(config, initial_size, result_arity)
    pub stack_cleanup: usize,
    // i64 fn(config, index)
    pub callrec_read: usize,
    // void fn(config, index, value)
    pub callrec_write: usize,
    // i32 fn(interp, config, func_index); call using call record args
    pub call_with_record: usize,
    // i32 fn(interp, config, func_index, ...args); direct call via compiled function table
    pub direct_call_0: usize,
    pub direct_call_1: usize,
    pub direct_call_2: usize,
    pub direct_call_3: usize,
    // i32 fn(interp, config, table_idx, type_idx, element_index)
    pub call_indirect: usize,
    // i32 fn(interp, config, dst_mem, src_mem, dst, src, count)
    pub memory_copy: usize,
    // i32 fn(interp, config, mem_idx, offset, value, count)
    pub memory_fill: usize,

    pub regs_offset: u32,
    pub value_size: u32,
    pub locals_base_offset: u32,
    pub default_memory_base_offset: u32,
    pub compiled_call_result_scratch_offset: u32,
}

pub fn compile_to_bytes(
    insns: &[CraneliftInsn],
    helpers: &RuntimeHelpers,
    outcome_return_value: u64,
    result_arity: u32,
) -> Result<Vec<u8>, &'static str> {
    CraneliftCompiler::compile_to_bytes(insns, helpers, outcome_return_value, result_arity)
}
