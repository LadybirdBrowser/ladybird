/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The functions of the runtime compiled code calls for bulk memory and table operations (`Op::Helper`). They take
//! their operands in the context's `arguments` like the runtime's own helpers, need nothing but the data the
//! operands point at, and return 0, or the TRAP_* code to trap with. Growing calls the runtime's own helpers.

use crate::RuntimeInfo;
use crate::ir::Helper;
use libwasm_module::abi::{
    ExecutionContext, MemoryData, Segment, TRAP_MEMORY_OUT_OF_BOUNDS, TRAP_TABLE_OUT_OF_BOUNDS, TableData,
};

/// The address of the function `helper` calls.
pub fn helper_address(helper: Helper, runtime: &RuntimeInfo) -> u64 {
    let function: unsafe extern "C" fn(*mut ExecutionContext) -> u64 = match helper {
        Helper::MemoryGrow => return runtime.memory_grow,
        Helper::TableGrow => return runtime.table_grow,
        Helper::MemoryFill => memory_fill,
        Helper::MemoryCopy => memory_copy,
        Helper::MemoryInit => memory_init,
        Helper::TableFill => table_fill,
        Helper::TableCopy => table_copy,
        Helper::TableInit => table_init,
    };
    function as usize as u64
}

/// Whether `count` items from `start` are within `size`. The operands of memory64 and table64 operations are
/// 64-bit, so the end can wrap around.
fn in_range(start: u64, count: u64, size: u64) -> bool {
    start.checked_add(count).is_some_and(|end| end <= size)
}

unsafe fn arguments<'a>(context: *mut ExecutionContext) -> &'a [u64; 6] {
    unsafe { &(*context).arguments }
}

unsafe extern "C" fn memory_fill(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let &[memory, destination, value, count, ..] = arguments(context);
        let memory = &*(memory as *const MemoryData);
        if !in_range(destination, count, memory.size) {
            return u64::from(TRAP_MEMORY_OUT_OF_BOUNDS);
        }
        if count == 0 {
            return 0;
        }
        std::ptr::write_bytes(memory.base.add(destination as usize), value as u8, count as usize);
        0
    }
}

unsafe extern "C" fn memory_copy(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let &[destination_memory, source_memory, destination, source, count, _] = arguments(context);
        let destination_memory = &*(destination_memory as *const MemoryData);
        let source_memory = &*(source_memory as *const MemoryData);
        if !in_range(destination, count, destination_memory.size) || !in_range(source, count, source_memory.size) {
            return u64::from(TRAP_MEMORY_OUT_OF_BOUNDS);
        }
        if count == 0 {
            return 0;
        }
        std::ptr::copy(
            source_memory.base.add(source as usize),
            destination_memory.base.add(destination as usize),
            count as usize,
        );
        0
    }
}

unsafe extern "C" fn memory_init(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let &[memory, segment, destination, source, count, _] = arguments(context);
        let memory = &*(memory as *const MemoryData);
        let segment = &*(segment as *const Segment);
        if !in_range(source, count, segment.length) || !in_range(destination, count, memory.size) {
            return u64::from(TRAP_MEMORY_OUT_OF_BOUNDS);
        }
        if count == 0 {
            return 0;
        }
        std::ptr::copy(
            segment.contents.add(source as usize),
            memory.base.add(destination as usize),
            count as usize,
        );
        0
    }
}

unsafe extern "C" fn table_fill(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let &[table, destination, value, count, ..] = arguments(context);
        let table = &*(table as *const TableData);
        if !in_range(destination, count, table.size) {
            return u64::from(TRAP_TABLE_OUT_OF_BOUNDS);
        }
        for index in destination..destination + count {
            *table.elements.add(index as usize) = value;
        }
        0
    }
}

unsafe extern "C" fn table_copy(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let &[destination_table, source_table, destination, source, count, _] = arguments(context);
        let destination_table = &*(destination_table as *const TableData);
        let source_table = &*(source_table as *const TableData);
        if !in_range(destination, count, destination_table.size) || !in_range(source, count, source_table.size) {
            return u64::from(TRAP_TABLE_OUT_OF_BOUNDS);
        }
        if count == 0 {
            return 0;
        }
        std::ptr::copy(
            source_table.elements.add(source as usize),
            destination_table.elements.add(destination as usize),
            count as usize,
        );
        0
    }
}

unsafe extern "C" fn table_init(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let &[table, segment, destination, source, count, _] = arguments(context);
        let table = &*(table as *const TableData);
        let segment = &*(segment as *const Segment);
        if !in_range(source, count, segment.length) || !in_range(destination, count, table.size) {
            return u64::from(TRAP_TABLE_OUT_OF_BOUNDS);
        }
        if count == 0 {
            return 0;
        }
        std::ptr::copy(
            segment.contents.cast::<u64>().add(source as usize),
            table.elements.add(destination as usize),
            count as usize,
        );
        0
    }
}
