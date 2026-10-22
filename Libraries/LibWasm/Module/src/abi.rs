/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The data structures the interpreter reads and writes, and the layout of its frames. The build script includes this
//! file to write the layout constants `interpreter.flap` is compiled against, so the two always agree.
//!
//! # Frames
//!
//! Every function runs in a frame on the machine stack, which grows down. Locals sit above the frame pointer and the
//! frame header below it, with the operand stack below the header, growing down in 8-byte slots:
//!
//! ```text
//!   caller's operand stack ... the arguments    (copied into the callee's locals, then dropped)
//!   ─────────────────────────────────────────   <- the caller's operand stack without the arguments ("return vsp")
//!   v128 locals, 16 bytes each from fp + FunctionHeader::v128_locals_offset (16-byte aligned), if there are any
//!   local i at fp + 8 * i
//!   fp ──────────────────────────────────────   16-byte aligned
//!   frame header, fp - FRAME_HEADER_SIZE up to fp (see FRAME_*)
//!   operand stack, empty at fp - FRAME_HEADER_SIZE, growing down; a v128 takes two slots
//!   headroom for the deepest operand stack, then the spill area of the handlers (SPILL_AREA_*)
//!   sp ──────────────────────────────────────   fp - FunctionHeader::frame_size, 16-byte aligned
//! ```
//!
//! A v128 local has a scalar slot at fp + 8 * i too, which stays unused, so that every local's scalar slot is at a
//! fixed offset. A future JIT uses the same frames, so that calls between the tiers need no adapters and entering
//! compiled code at a loop header only loads the slots.

#![allow(dead_code)]

use std::mem::offset_of;

/// The frame header. All fields are 8 bytes, at `fp + FRAME_*`. Its first two words are where a native call and
/// `push fp` leave the return address and the caller's fp, so that compiled frames have the same header.
///
/// The caller's instruction pointer after its call instruction.
pub const FRAME_RETURN_IP: isize = -8;
pub const FRAME_SAVED_FP: isize = -16;
pub const FRAME_SAVED_STP: isize = -24;
/// The `FunctionHeader` of the function running in the frame.
pub const FRAME_FUNCTION: isize = -32;
/// Where the caller's operand stack continues once the arguments are popped. Results are pushed from there.
pub const FRAME_RETURN_VSP: isize = -40;
/// The caller's `memory_base`.
pub const FRAME_SAVED_MEMORY_BASE: isize = -48;
/// The `InstanceData` of the function running in the frame.
pub const FRAME_INSTANCE: isize = -56;
/// The `ExecutionContext` the frame runs in.
pub const FRAME_CONTEXT: isize = -64;
pub const FRAME_HEADER_SIZE: usize = 64;

/// The slots handlers spill values to, at the bottom of every frame, just above the stack pointer.
pub const SPILL_AREA_OFFSET: usize = 0;
pub const SPILL_AREA_SLOTS: usize = 32;
pub const SPILL_AREA_SIZE: usize = SPILL_AREA_SLOTS * 8;

pub const FUNCTION_KIND_WASM: u32 = 0;
pub const FUNCTION_KIND_HOST: u32 = 1;
/// A WebAssembly function with v128 parameters or locals, whose frame has storage for them.
pub const FUNCTION_KIND_WASM_VECTOR: u32 = 2;
/// A WebAssembly function with compiled code, which runs at its `native_entry`.
pub const FUNCTION_KIND_COMPILED: u32 = 3;

/// What a call costs of the callee's tier-up budget.
pub const TIER_UP_CALL_COST: u32 = 10;

/// The pseudo-instructions the runtime enters the interpreter at: Enter pushes the arguments, CallEntry calls the
/// function, and the function returns to Exit, which leaves the interpreter.
pub const OPCODE_ENTER: u8 = 0xf0;
pub const OPCODE_EXIT: u8 = 0xf1;
pub const OPCODE_CALL_ENTRY: u8 = 0xf3;
/// The pseudo-instruction a faulting memory access continues at.
pub const OPCODE_MEMORY_FAULT: u8 = 0xf2;
/// The pseudo-instruction compiled code traps through, with the trap code in the context's first argument.
pub const OPCODE_TRAP_FROM_NATIVE: u8 = 0xef;
/// The pseudo-instruction that calls the function a compiled function the interpreter called tail called, in that
/// function's place, when it does not run compiled.
pub const OPCODE_CALL_PENDING: u8 = 0xee;

/// One function, as the interpreter sees it. Function references point at these.
#[repr(C)]
pub struct FunctionHeader {
    /// The function's first instruction.
    pub code: *const u8,
    /// The function's final `end` instruction.
    pub code_end: *const u8,
    pub side_table: *const u8,
    pub instance: *const InstanceData,
    pub kind: u32,
    pub param_slots: u32,
    pub result_slots: u32,
    /// The number of locals, parameters included.
    pub local_count: u32,
    /// The bytes of locals above fp, 16-byte aligned.
    pub locals_size: u32,
    /// What the function may still run before the runtime is asked to compile it: calls cost TIER_UP_CALL_COST, loop
    /// entries and back edges 1. The runtime gets the call or loop that would take it below zero.
    pub tier_up_budget: u32,
    /// The bytes below fp the frame needs, operand stack and red zone included.
    pub frame_size: u32,
    /// The store-wide identity of the function's type, equal exactly for equal types.
    pub type_identity: u32,
    /// For host functions, what the runtime needs to call them.
    pub host: *const u8,
    /// For functions with v128 parameters or locals (FUNCTION_KIND_WASM_VECTOR), the offset from fp of each local's
    /// 16-byte storage, or zero for scalar locals. Null otherwise.
    pub vector_storage: *const u32,
    /// Where compiled code calls the function: its compiled code, or zero while it has none.
    pub native_entry: u64,
}

/// A memory, as the interpreter sees it.
#[repr(C)]
pub struct MemoryData {
    pub base: *mut u8,
    /// The accessible size, in bytes.
    pub size: u64,
    /// Nonzero for a memory64, whose addresses and sizes are i64 values.
    pub address64: u64,
    /// Nonzero for a shared memory, which atomic waits may block on.
    pub shared: u64,
}

/// A table, as the interpreter sees it. Elements are 8-byte references, zero for null.
#[repr(C)]
pub struct TableData {
    pub elements: *mut u64,
    pub size: u64,
    /// Nonzero for a table64, whose indices and sizes are i64 values.
    pub address64: u64,
}

/// A module instance, as the interpreter sees it.
#[repr(C)]
pub struct InstanceData {
    /// The base of memory 0, or null if the instance has none.
    pub memory0_base: *mut u8,
    pub memories: *const *mut MemoryData,
    /// The storage of every global: 8 bytes, or 16 for v128 globals.
    pub globals: *const *mut u64,
    pub functions: *const *const FunctionHeader,
    pub tables: *const *mut TableData,
    /// The store-wide identity of each of the module's types.
    pub type_identities: *const u32,
    /// The bytes of each data segment, empty once dropped.
    pub data_segments: *const Segment,
    /// The references of each element segment, empty once dropped.
    pub element_segments: *const Segment,
    /// The identity of each of the instance's tags: the address of its TagInstance.
    pub tags: *const u64,
    pub function_count: u32,
    pub memory_count: u32,
    /// The `ExecutionContext` of the store the instance belongs to, which compiled code finds the runtime through.
    pub context: *mut ExecutionContext,
}

/// A passive segment: its contents and their length, in bytes for data and in elements for element segments.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Segment {
    pub contents: *const u8,
    pub length: u64,
}

pub const TRAP_NONE: u32 = 0;
pub const TRAP_UNREACHABLE: u32 = 1;
pub const TRAP_STACK_EXHAUSTED: u32 = 2;
pub const TRAP_MEMORY_OUT_OF_BOUNDS: u32 = 3;
pub const TRAP_INTEGER_DIVIDE_BY_ZERO: u32 = 4;
pub const TRAP_INTEGER_OVERFLOW: u32 = 5;
pub const TRAP_INVALID_CONVERSION: u32 = 6;
pub const TRAP_UNDEFINED_ELEMENT: u32 = 7;
pub const TRAP_UNINITIALIZED_ELEMENT: u32 = 8;
pub const TRAP_INDIRECT_CALL_TYPE_MISMATCH: u32 = 9;
pub const TRAP_TABLE_OUT_OF_BOUNDS: u32 = 10;
pub const TRAP_NULL_REFERENCE: u32 = 11;
pub const TRAP_CAST_FAILURE: u32 = 12;
pub const TRAP_ARRAY_OUT_OF_BOUNDS: u32 = 13;
/// The runtime reported a trap of its own, whose details it keeps.
pub const TRAP_RUNTIME: u32 = 14;
pub const TRAP_UNCAUGHT_EXCEPTION: u32 = 15;
pub const TRAP_UNALIGNED_ATOMIC: u32 = 16;
pub const TRAP_EXPECTED_SHARED_MEMORY: u32 = 17;

/// What `wasm_runtime_wait` returns, besides its results 0 to 2, when the agent cannot block: the store has the trap
/// (TRAP_RUNTIME).
pub const WAIT_CANNOT_BLOCK: u64 = 3;

/// The state one thread executes WebAssembly with.
#[repr(C)]
pub struct ExecutionContext {
    /// The lowest address a frame may reach.
    pub stack_limit: u64,
    /// The fp of the innermost entry into the interpreter.
    pub entry_fp: u64,
    /// The `EntryRecord` of the innermost entry into the interpreter.
    pub entry_record: *mut EntryRecord,
    pub store: *mut u8,
    /// Where the interpreter passes operands to the runtime functions it calls, which take the context alone.
    pub arguments: [u64; 6],
    /// The runtime type of each store-wide type identity.
    pub rtts: *const *const Rtt,
    /// The exception that compiled code handles where a call returns to it, or 0 (see `runtime/jit.rs`).
    pub exception: u64,
    /// Where an exception continues in compiled code, or in the entry trampoline: rsp, rbp and the address the
    /// resume stub jumps to.
    pub resume: [u64; 3],
}

/// What the interpreter knows about a type at runtime: how to check subtyping against it, and how objects of it are
/// laid out. Rtts live in cells of their own, after the cell header (see heap.rs).
///
/// A GC object is a cell that starts with the address of its type's Rtt, followed by the cell header LibGC owns, the
/// address of its data (OBJECT_DATA_OFFSET), and an array's length (OBJECT_LENGTH_OFFSET). The data is a struct's
/// fields or an array's elements, inline at OBJECT_INLINE_DATA_OFFSET if they fit there and outside the heap otherwise.
#[repr(C)]
pub struct Rtt {
    pub kind: u32,
    /// The number of supertypes above this type.
    pub depth: u32,
    /// The identities of this type's supertypes, from the root down, and of this type last: `depth + 1` of them.
    pub display: *const u32,
    /// For structs, the byte offset of each field from the start of the data.
    pub field_offsets: *const u32,
    /// The STORAGE_* kind of each field of a struct, or of the elements of an array.
    pub field_kinds: *const u8,
    /// For structs, the size of the data; for arrays, the size of an element.
    pub size: u32,
    pub field_count: u32,
}

pub const RTT_KIND_FUNCTION: u32 = 0;
pub const RTT_KIND_STRUCT: u32 = 1;
pub const RTT_KIND_ARRAY: u32 = 2;

/// How a struct field or array element is stored: one 8-byte word, with packed and 32-bit values zero-extended
/// in it, or 16 bytes for a v128. References the collector follows (those of the any, extern, and exn hierarchies)
/// have a kind of their own.
pub const STORAGE_64: u8 = 0;
pub const STORAGE_I8: u8 = 1;
pub const STORAGE_I16: u8 = 2;
pub const STORAGE_V128: u8 = 3;
pub const STORAGE_32: u8 = 4;
/// A reference in the any, extern, or exn hierarchy, which the collector follows.
pub const STORAGE_REFERENCE: u8 = 5;
/// A function reference, which the collector follows to its function. Stored like STORAGE_64.
pub const STORAGE_FUNCTION_REFERENCE: u8 = 6;

pub const OBJECT_DATA_OFFSET: usize = 16;
pub const OBJECT_LENGTH_OFFSET: usize = 24;
pub const OBJECT_INLINE_DATA_OFFSET: usize = 32;

/// The sizes of the data a GC object can keep inline, the size classes of the runtime's heap. Larger data is
/// allocated outside of the heap.
pub const OBJECT_DATA_SIZE_CLASSES: [usize; 17] = [
    0, 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096,
];

/// The most data an object keeps inline.
pub const MAX_INLINE_DATA_SIZE: usize = OBJECT_DATA_SIZE_CLASSES[OBJECT_DATA_SIZE_CLASSES.len() - 1];

// NB: The bytes a field or element takes in an object's data are `StorageType::object_field_size()` (types.rs); a
//     struct's fields follow each other in order from offset 0.

/// An exception is a cell with the identity of its tag, the address of its payload (its values' slots in operand
/// stack order, the last value in the lowest slot), the payload's length in slots, and how the collector follows
/// each slot.
pub const EXCEPTION_CELL_SIZE: usize = 48;
pub const EXCEPTION_TAG_OFFSET: usize = 16;
pub const EXCEPTION_PAYLOAD_OFFSET: usize = 24;
pub const EXCEPTION_LENGTH_OFFSET: usize = 32;
pub const EXCEPTION_KINDS_OFFSET: usize = 40;

/// What the runtime enters the interpreter with, and what it gets back.
#[repr(C)]
pub struct EntryRecord {
    pub function: *const FunctionHeader,
    /// The slots of the arguments as the operand stack holds them: the last argument at the lowest address.
    pub arguments: *const u64,
    /// Where the slots of the results go, laid out as the operand stack holds them.
    pub results: *mut u64,
    pub context: *mut ExecutionContext,
    /// The `TRAP_*` code the call ended with.
    pub trap: u32,
    pub padding: u32,
    pub saved_entry_fp: u64,
    pub saved_entry_record: *mut EntryRecord,
}

/// The layout constants of everything above, in the format flapc reads.
pub fn layout_constants() -> String {
    let mut text = String::from("# Generated by the libwasm_engine build script -- DO NOT EDIT\n");
    let mut signed_constant = |name: &str, value: isize| {
        text.push_str(&format!("const {name} = {value}\n"));
    };
    signed_constant("FRAME_SAVED_FP", FRAME_SAVED_FP);
    signed_constant("FRAME_RETURN_IP", FRAME_RETURN_IP);
    signed_constant("FRAME_SAVED_STP", FRAME_SAVED_STP);
    signed_constant("FRAME_FUNCTION", FRAME_FUNCTION);
    signed_constant("FRAME_RETURN_VSP", FRAME_RETURN_VSP);
    signed_constant("FRAME_SAVED_MEMORY_BASE", FRAME_SAVED_MEMORY_BASE);
    signed_constant("FRAME_INSTANCE", FRAME_INSTANCE);
    signed_constant("FRAME_CONTEXT", FRAME_CONTEXT);
    let mut constant = |name: &str, value: usize| signed_constant(name, value as isize);
    constant("FRAME_HEADER_SIZE", FRAME_HEADER_SIZE);
    constant("SPILL_AREA_OFFSET", SPILL_AREA_OFFSET);
    constant("SPILL_AREA_SLOTS", SPILL_AREA_SLOTS);
    constant("SPILL_AREA_SIZE", SPILL_AREA_SIZE);
    constant("FUNCTION_KIND_WASM", FUNCTION_KIND_WASM as usize);
    constant("FUNCTION_KIND_HOST", FUNCTION_KIND_HOST as usize);
    constant("FUNCTION_KIND_WASM_VECTOR", FUNCTION_KIND_WASM_VECTOR as usize);
    constant("FUNCTION_KIND_COMPILED", FUNCTION_KIND_COMPILED as usize);
    constant("TIER_UP_CALL_COST", TIER_UP_CALL_COST as usize);
    macro_rules! field {
        ($name:literal, $type:ty, $field:ident) => {
            constant($name, offset_of!($type, $field));
        };
    }
    field!("FUNCTION_CODE", FunctionHeader, code);
    field!("FUNCTION_CODE_END", FunctionHeader, code_end);
    field!("FUNCTION_SIDE_TABLE", FunctionHeader, side_table);
    field!("FUNCTION_INSTANCE", FunctionHeader, instance);
    field!("FUNCTION_KIND", FunctionHeader, kind);
    field!("FUNCTION_PARAM_SLOTS", FunctionHeader, param_slots);
    field!("FUNCTION_RESULT_SLOTS", FunctionHeader, result_slots);
    field!("FUNCTION_LOCAL_COUNT", FunctionHeader, local_count);
    field!("FUNCTION_LOCALS_SIZE", FunctionHeader, locals_size);
    field!("FUNCTION_VECTOR_STORAGE", FunctionHeader, vector_storage);
    field!("FUNCTION_NATIVE_ENTRY", FunctionHeader, native_entry);
    field!("FUNCTION_TIER_UP_BUDGET", FunctionHeader, tier_up_budget);
    field!("FUNCTION_FRAME_SIZE", FunctionHeader, frame_size);
    field!("FUNCTION_TYPE_IDENTITY", FunctionHeader, type_identity);
    field!("FUNCTION_HOST", FunctionHeader, host);
    field!("MEMORY_BASE", MemoryData, base);
    field!("MEMORY_SIZE", MemoryData, size);
    field!("MEMORY_ADDRESS64", MemoryData, address64);
    field!("MEMORY_SHARED", MemoryData, shared);
    field!("TABLE_ELEMENTS", TableData, elements);
    field!("TABLE_SIZE", TableData, size);
    field!("TABLE_ADDRESS64", TableData, address64);
    field!("INSTANCE_MEMORY0_BASE", InstanceData, memory0_base);
    field!("INSTANCE_MEMORIES", InstanceData, memories);
    field!("INSTANCE_GLOBALS", InstanceData, globals);
    field!("INSTANCE_FUNCTIONS", InstanceData, functions);
    field!("INSTANCE_TABLES", InstanceData, tables);
    field!("INSTANCE_TYPE_IDENTITIES", InstanceData, type_identities);
    field!("INSTANCE_DATA_SEGMENTS", InstanceData, data_segments);
    field!("INSTANCE_ELEMENT_SEGMENTS", InstanceData, element_segments);
    field!("INSTANCE_MEMORY_COUNT", InstanceData, memory_count);
    field!("INSTANCE_TAGS", InstanceData, tags);
    field!("INSTANCE_CONTEXT", InstanceData, context);
    field!("SEGMENT_CONTENTS", Segment, contents);
    field!("SEGMENT_LENGTH", Segment, length);
    field!("CONTEXT_STACK_LIMIT", ExecutionContext, stack_limit);
    field!("CONTEXT_ENTRY_FP", ExecutionContext, entry_fp);
    field!("CONTEXT_ENTRY_RECORD", ExecutionContext, entry_record);
    field!("CONTEXT_ARGUMENTS", ExecutionContext, arguments);
    field!("CONTEXT_RTTS", ExecutionContext, rtts);
    field!("RTT_KIND", Rtt, kind);
    field!("RTT_DEPTH", Rtt, depth);
    field!("RTT_DISPLAY", Rtt, display);
    field!("RTT_FIELD_OFFSETS", Rtt, field_offsets);
    field!("RTT_FIELD_KINDS", Rtt, field_kinds);
    field!("RTT_SIZE", Rtt, size);
    field!("RTT_FIELD_COUNT", Rtt, field_count);
    constant("RTT_KIND_FUNCTION", RTT_KIND_FUNCTION as usize);
    constant("RTT_KIND_STRUCT", RTT_KIND_STRUCT as usize);
    constant("RTT_KIND_ARRAY", RTT_KIND_ARRAY as usize);
    constant("STORAGE_64", STORAGE_64 as usize);
    constant("STORAGE_I8", STORAGE_I8 as usize);
    constant("STORAGE_I16", STORAGE_I16 as usize);
    constant("STORAGE_V128", STORAGE_V128 as usize);
    constant("STORAGE_32", STORAGE_32 as usize);
    constant("STORAGE_REFERENCE", STORAGE_REFERENCE as usize);
    constant("STORAGE_FUNCTION_REFERENCE", STORAGE_FUNCTION_REFERENCE as usize);
    constant("OBJECT_DATA_OFFSET", OBJECT_DATA_OFFSET);
    constant("OBJECT_LENGTH_OFFSET", OBJECT_LENGTH_OFFSET);
    constant("WAIT_CANNOT_BLOCK", WAIT_CANNOT_BLOCK as usize);
    constant("CONTEXT_ARGUMENT_1", offset_of!(ExecutionContext, arguments) + 8);
    constant("CONTEXT_ARGUMENT_2", offset_of!(ExecutionContext, arguments) + 16);
    constant("CONTEXT_ARGUMENT_3", offset_of!(ExecutionContext, arguments) + 24);
    constant("CONTEXT_ARGUMENT_4", offset_of!(ExecutionContext, arguments) + 32);
    field!("ENTRY_FUNCTION", EntryRecord, function);
    field!("ENTRY_ARGUMENTS", EntryRecord, arguments);
    field!("ENTRY_RESULTS", EntryRecord, results);
    field!("ENTRY_CONTEXT", EntryRecord, context);
    field!("ENTRY_TRAP", EntryRecord, trap);
    field!("ENTRY_SAVED_ENTRY_FP", EntryRecord, saved_entry_fp);
    field!("ENTRY_SAVED_ENTRY_RECORD", EntryRecord, saved_entry_record);
    for (name, value) in [
        ("TRAP_UNREACHABLE", TRAP_UNREACHABLE),
        ("TRAP_STACK_EXHAUSTED", TRAP_STACK_EXHAUSTED),
        ("TRAP_MEMORY_OUT_OF_BOUNDS", TRAP_MEMORY_OUT_OF_BOUNDS),
        ("TRAP_INTEGER_DIVIDE_BY_ZERO", TRAP_INTEGER_DIVIDE_BY_ZERO),
        ("TRAP_INTEGER_OVERFLOW", TRAP_INTEGER_OVERFLOW),
        ("TRAP_INVALID_CONVERSION", TRAP_INVALID_CONVERSION),
        ("TRAP_UNDEFINED_ELEMENT", TRAP_UNDEFINED_ELEMENT),
        ("TRAP_UNINITIALIZED_ELEMENT", TRAP_UNINITIALIZED_ELEMENT),
        ("TRAP_INDIRECT_CALL_TYPE_MISMATCH", TRAP_INDIRECT_CALL_TYPE_MISMATCH),
        ("TRAP_TABLE_OUT_OF_BOUNDS", TRAP_TABLE_OUT_OF_BOUNDS),
        ("TRAP_NULL_REFERENCE", TRAP_NULL_REFERENCE),
        ("TRAP_CAST_FAILURE", TRAP_CAST_FAILURE),
        ("TRAP_ARRAY_OUT_OF_BOUNDS", TRAP_ARRAY_OUT_OF_BOUNDS),
        ("TRAP_RUNTIME", TRAP_RUNTIME),
        ("TRAP_UNCAUGHT_EXCEPTION", TRAP_UNCAUGHT_EXCEPTION),
        ("TRAP_UNALIGNED_ATOMIC", TRAP_UNALIGNED_ATOMIC),
        ("TRAP_EXPECTED_SHARED_MEMORY", TRAP_EXPECTED_SHARED_MEMORY),
    ] {
        constant(name, value as usize);
    }
    text
}
