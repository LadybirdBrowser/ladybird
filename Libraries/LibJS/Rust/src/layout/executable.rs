/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use super::buffer::BlockSlice;
use super::cell::CellHeaderTail;
use super::feedback::ExecutableFeedbackHead;
use super::property_lookup_cache::{EnvironmentCoordinate, GlobalVariableCache, PropertyLookupCache};
use super::value::Value;

/// The `jit::CompileState` of executables whose JIT code new frames run in.
pub const JIT_COMPILE_STATE_INSTALLED: u8 = 2;

/// The part of a bytecode executable that the interpreter reads. The rest of the executable follows it.
#[repr(C)]
pub struct ExecutableHead {
    pub registers_and_locals_count: Cell<u32>,
    pub registers_and_locals_and_constants_count: Cell<u32>,
    pub header: CellHeaderTail,
    /// Which of the VM's dispatch tables (see `VmHead::dispatch_tables`) the interpreter runs this executable's frames
    /// with, which it switches to whenever it enters one of them.
    pub dispatch_table_index: Cell<u8>,
    /// The profiling interpreter counts this down as it enters this executable and runs its loop back edges. The
    /// JIT's tier-up policy runs when it reaches zero; while the executable warms up, that moves it to the profiling
    /// handlers.
    pub tier_up_budget: Cell<i32>,
    pub bytecode_data: Cell<*const u8>,
    /// The executable's slot in the VM's JIT entry table (see `jit::entry_table`), which holds its JIT code, or 0.
    pub jit_entry_slot: Cell<u32>,
    /// The executable's `jit::CompileState`. The interpreter runs new frames of the executable in its JIT code while
    /// it is `JIT_COMPILE_STATE_INSTALLED`.
    pub jit_compile_state: Cell<u8>,
    /// How many collections ran since a frame last entered the executable: entering it sets this to 0, and each
    /// collection counts it up to 255. Code that nothing entered for long gets flushed (see `bytecode::flushing`).
    pub idle_collection_count: Cell<u8>,
    /// Where the profiling interpreter records feedback for the optimizing JIT.
    pub feedback: ExecutableFeedbackHead,
    pub constants: BlockSlice<Value>,
    pub property_lookup_caches: BlockSlice<PropertyLookupCache>,
    pub global_variable_caches: BlockSlice<GlobalVariableCache>,
    pub environment_coordinate_caches: BlockSlice<Cell<EnvironmentCoordinate>>,
    /// The call caches, one for each call feedback index of the bytecode, or null. The executable gets them with
    /// its feedback, when it becomes warm.
    pub call_caches: Cell<*const CallCache>,
    /// The `asm_call_metadata` of the executable's function, which calls of it read from here. Its closure and construct
    /// counts can be behind those of the function, as calls do not read them.
    pub call_metadata: Cell<u64>,
}

const _: () = assert!(size_of::<ExecutableHead>() == 112);

/// The bits of `CallCache::callee` while the cache is empty: a reserved tag that no value has.
pub const CALL_CACHE_EMPTY_CALLEE: u64 = super::feedback::value_feedback::EMPTY_VALUE_BUCKET;

/// The tag in the low bits of `CallCache::target`. An executable is 8-aligned, so its pointer has tag 0.
pub const CALL_CACHE_TARGET_TAG_MASK: u64 = 7;
pub const CALL_CACHE_TARGET_EXECUTABLE: u64 = 0;
/// The target is a native function table index, shifted left by `CALL_CACHE_TARGET_INDEX_SHIFT`.
pub const CALL_CACHE_TARGET_NATIVE_FUNCTION: u64 = 1;
pub const CALL_CACHE_TARGET_INDEX_SHIFT: u64 = 3;

/// The function that a call site called last. The interpreter links an entry only to a function that it can call
/// without the help of the runtime, so a call whose callee value has the bits of `callee` needs no checks.
///
/// Nothing in it is traced. The VM's sweep callback clears the entry when the callee or its executable dies or moves.
#[repr(C)]
pub struct CallCache {
    /// The bits of the callee value, or `CALL_CACHE_EMPTY_CALLEE`.
    pub callee: Cell<u64>,
    /// The callee's executable, or its tagged native function table index (see `CALL_CACHE_TARGET_TAG_MASK`).
    pub target: Cell<u64>,
}

const _: () = assert!(size_of::<CallCache>() == 16);

impl Default for CallCache {
    fn default() -> Self {
        Self {
            callee: Cell::new(CALL_CACHE_EMPTY_CALLEE),
            target: Cell::new(0),
        }
    }
}

impl CallCache {
    pub fn clear(&self) {
        self.callee.set(CALL_CACHE_EMPTY_CALLEE);
    }
}
