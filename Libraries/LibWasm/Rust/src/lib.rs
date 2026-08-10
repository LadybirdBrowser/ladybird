/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod compiler;
pub mod serialized;

use compiler::CraneliftCompiler;

/// Immediates:
///   constants:    imm1 = value (i32 sign-extended, i64, or f32/f64 bits)
///   local ops:    imm1 = local index
///   global ops:   imm1 = global index
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
pub struct RuntimeLayout {
    pub regs_offset: u32,
    pub value_size: u32,
    pub locals_base_offset: u32,
    pub memory_instances_offset: u32,
    pub global_instances_offset: u32,
    pub global_instance_value_offset: u32,
    pub memory_instance_data_offset: u32,
    pub memory_buffer_storage_offset_offset: u32,
    pub compiled_call_result_scratch_offset: u32,
    pub value_stack_base_offset: u32,
    pub value_stack_top_offset: u32,
    pub call_record_base_offset: u32,
}

/// Stable index assigned to each runtime helper. Embedded in cranelift `ExternalName`
/// entries so we can recover, post-codegen, which helper a given relocation targets.
/// The numeric values are part of the cache blob format -- do NOT reorder.
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[allow(non_camel_case_types)]
pub enum HelperId {
    call_function = 0,
    set_trap = 1,
    memory_size = 2,
    memory_grow = 3,
    call_with_record = 4,
    direct_call_0 = 5,
    direct_call_1 = 6,
    direct_call_2 = 7,
    direct_call_3 = 8,
    call_indirect = 9,
    memory_copy = 10,
    memory_fill = 11,
    primitive_storage_cage_base = 12,
}

pub const HELPER_COUNT: u32 = 13;
pub const SERIALIZED_CODE_ALIGNMENT: usize = 16;

/// One relocation slot in the generated machine code. `code_offset` is the byte offset
/// from the start of the function where 8 contiguous bytes hold the absolute helper
/// address; on cache install, those bytes get rewritten to the current process's
/// helper pointer.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct HelperReloc {
    pub code_offset: u32,
    pub helper_id: u32,
    pub addend: i64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct CraneliftTrap {
    pub offset: u32,
    pub code: u8,
    pub _padding: [u8; 3],
}

/// Output of `compile_to_bytes`: the machine code plus the patch table.
pub struct CompiledFunction {
    pub code: Vec<u8>,
    pub relocs: Vec<HelperReloc>,
    pub traps: Vec<CraneliftTrap>,
}

pub fn compile_to_bytes(
    insns: &[CraneliftInsn],
    layout: &RuntimeLayout,
    outcome_return_value: u64,
    result_arity: u32,
    num_locals: u32,
    num_params: u32,
    local_types: &[u8],
) -> Result<CompiledFunction, &'static str> {
    CraneliftCompiler::compile_to_bytes(
        insns,
        layout,
        outcome_return_value,
        result_arity,
        num_locals,
        num_params,
        local_types,
    )
}
