/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The functions of the runtime compiled code calls for bulk memory, table and array operations (`Op::Helper`). They
//! take their operands in the context's `arguments` like the runtime's own helpers, need nothing but the data the
//! operands point at, and return 0, or the TRAP_* code to trap with. Growing, allocating, and waiting on and
//! notifying shared memory locations call the runtime's own helpers.

use crate::RuntimeInfo;
use crate::ir::Helper;
use libwasm_module::abi::{
    ExecutionContext, MemoryData, OBJECT_DATA_OFFSET, OBJECT_LENGTH_OFFSET, Rtt, STORAGE_32, STORAGE_64, STORAGE_I8,
    STORAGE_I16, STORAGE_V128, Segment, TRAP_ARRAY_OUT_OF_BOUNDS, TRAP_MEMORY_OUT_OF_BOUNDS, TRAP_NULL_REFERENCE,
    TRAP_TABLE_OUT_OF_BOUNDS, TableData,
};

/// Makes instruction fetches see code just written at `start`, `length` bytes. On AArch64 the data cache lines are
/// cleaned to where instruction fetches see them and the instruction cache lines invalidated; x86-64 keeps its caches
/// coherent.
pub fn flush_instruction_cache(start: usize, length: usize) {
    #[cfg(all(target_arch = "aarch64", target_os = "macos"))]
    {
        unsafe extern "C" {
            fn sys_icache_invalidate(start: *mut core::ffi::c_void, length: usize);
        }
        // SAFETY: The range is code the caller just wrote.
        unsafe { sys_icache_invalidate(start as *mut core::ffi::c_void, length) };
    }
    #[cfg(all(target_arch = "aarch64", not(target_os = "macos")))]
    // SAFETY: Cache maintenance by address changes no data.
    unsafe {
        let cache_type: u64;
        core::arch::asm!("mrs {}, ctr_el0", out(reg) cache_type, options(nomem, nostack));
        let data_line = 4usize << ((cache_type >> 16) & 15);
        let instruction_line = 4usize << (cache_type & 15);
        let end = start + length;
        let mut line = start & !(data_line - 1);
        while line < end {
            core::arch::asm!("dc cvau, {}", in(reg) line, options(nostack));
            line += data_line;
        }
        core::arch::asm!("dsb ish", options(nostack));
        let mut line = start & !(instruction_line - 1);
        while line < end {
            core::arch::asm!("ic ivau, {}", in(reg) line, options(nostack));
            line += instruction_line;
        }
        core::arch::asm!("dsb ish", "isb", options(nostack));
    }
    #[cfg(not(target_arch = "aarch64"))]
    let _ = (start, length);
}

/// The address of the function `helper` calls.
pub fn helper_address(helper: Helper, runtime: &RuntimeInfo) -> u64 {
    let function: unsafe extern "C" fn(*mut ExecutionContext) -> u64 = match helper {
        Helper::MemoryGrow => return runtime.memory_grow,
        Helper::TableGrow => return runtime.table_grow,
        Helper::AllocateException => return runtime.allocate_exception,
        Helper::MemoryFill => memory_fill,
        Helper::MemoryCopy => memory_copy,
        Helper::MemoryInit => memory_init,
        Helper::TableFill => table_fill,
        Helper::TableCopy => table_copy,
        Helper::TableInit => table_init,
        Helper::AtomicWait => return runtime.wait,
        Helper::AtomicNotify => return runtime.notify,
        Helper::Allocate => return runtime.allocate,
        Helper::ArrayFill => array_fill,
        Helper::ArrayCopy => array_copy,
        Helper::ArrayInitData => array_init_data,
        Helper::ArrayInitElem => array_init_elem,
        Helper::SegmentRange => segment_range,
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

// GC arrays. An array is a cell with its data's address at OBJECT_DATA_OFFSET, its length at OBJECT_LENGTH_OFFSET, and
// its Rtt first; elements take 8 bytes, or 16 for v128 (see abi.rs).

/// An array's data, length, and element storage kind.
unsafe fn array_parts(array: u64) -> (*mut u8, u64, u8) {
    unsafe {
        let cell = array as *const u8;
        let data = cell.add(OBJECT_DATA_OFFSET).cast::<*mut u8>().read();
        let length = cell.add(OBJECT_LENGTH_OFFSET).cast::<u64>().read();
        let rtt = cell.cast::<*const Rtt>().read();
        (data, length, *(*rtt).field_kinds)
    }
}

fn element_size(kind: u8) -> usize {
    if kind == STORAGE_V128 { 16 } else { 8 }
}

/// The bytes an element of storage `kind` takes in a data segment.
fn data_size(kind: u8) -> usize {
    match kind {
        STORAGE_I8 => 1,
        STORAGE_I16 => 2,
        STORAGE_32 => 4,
        STORAGE_V128 => 16,
        _ => 8,
    }
}

unsafe extern "C" fn array_fill(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let &[array, start, low, high, count, _] = arguments(context);
        if array == 0 {
            return u64::from(TRAP_NULL_REFERENCE);
        }
        let (data, length, kind) = array_parts(array);
        if !in_range(start, count, length) {
            return u64::from(TRAP_ARRAY_OUT_OF_BOUNDS);
        }
        let low = match kind {
            STORAGE_I8 => low & 0xff,
            STORAGE_I16 => low & 0xffff,
            _ => low,
        };
        let size = element_size(kind);
        for index in start..start + count {
            let element = data.add(index as usize * size).cast::<u64>();
            element.write(low);
            if kind == STORAGE_V128 {
                element.add(1).write(high);
            }
        }
        0
    }
}

unsafe extern "C" fn array_copy(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let &[destination, destination_start, source, source_start, count, _] = arguments(context);
        if source == 0 || destination == 0 {
            return u64::from(TRAP_NULL_REFERENCE);
        }
        let (source_data, source_length, _) = array_parts(source);
        let (destination_data, destination_length, kind) = array_parts(destination);
        if !in_range(source_start, count, source_length) || !in_range(destination_start, count, destination_length) {
            return u64::from(TRAP_ARRAY_OUT_OF_BOUNDS);
        }
        if count == 0 {
            return 0;
        }
        let size = element_size(kind);
        std::ptr::copy(
            source_data.add(source_start as usize * size),
            destination_data.add(destination_start as usize * size),
            count as usize * size,
        );
        0
    }
}

unsafe extern "C" fn array_init_data(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let &[array, segment, destination, source, count, _] = arguments(context);
        if array == 0 {
            return u64::from(TRAP_NULL_REFERENCE);
        }
        let (data, length, kind) = array_parts(array);
        if !in_range(destination, count, length) {
            return u64::from(TRAP_ARRAY_OUT_OF_BOUNDS);
        }
        let segment = &*(segment as *const Segment);
        let bytes = data_size(kind);
        // NB: The count is below 2^32 and the size at most 16, so their product does not wrap.
        if !in_range(source, count * bytes as u64, segment.length) {
            return u64::from(TRAP_MEMORY_OUT_OF_BOUNDS);
        }
        let size = element_size(kind);
        for index in 0..count as usize {
            let from = segment.contents.add(source as usize + index * bytes);
            let to = data.add((destination as usize + index) * size).cast::<u64>();
            match kind {
                STORAGE_I8 => to.write(u64::from(from.read())),
                STORAGE_I16 => to.write(u64::from(from.cast::<u16>().read_unaligned())),
                STORAGE_32 => to.write(u64::from(from.cast::<u32>().read_unaligned())),
                STORAGE_V128 => {
                    to.write(from.cast::<u64>().read_unaligned());
                    to.add(1).write(from.cast::<u64>().add(1).read_unaligned());
                }
                _ => {
                    debug_assert_eq!(kind, STORAGE_64);
                    to.write(from.cast::<u64>().read_unaligned());
                }
            }
        }
        0
    }
}

unsafe extern "C" fn array_init_elem(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let &[array, segment, destination, source, count, _] = arguments(context);
        if array == 0 {
            return u64::from(TRAP_NULL_REFERENCE);
        }
        let (data, length, _) = array_parts(array);
        if !in_range(destination, count, length) {
            return u64::from(TRAP_ARRAY_OUT_OF_BOUNDS);
        }
        let segment = &*(segment as *const Segment);
        if !in_range(source, count, segment.length) {
            return u64::from(TRAP_TABLE_OUT_OF_BOUNDS);
        }
        if count == 0 {
            return 0;
        }
        std::ptr::copy(
            segment.contents.cast::<u64>().add(source as usize),
            data.cast::<u64>().add(destination as usize),
            count as usize,
        );
        0
    }
}

unsafe extern "C" fn segment_range(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let &[segment, start, count, unit, trap, _] = arguments(context);
        let segment = &*(segment as *const Segment);
        // NB: The count is below 2^32 and the unit at most 16, so their product does not wrap.
        if in_range(start, count * unit, segment.length) {
            0
        } else {
            trap
        }
    }
}
