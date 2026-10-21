/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime services the interpreter calls for what needs the runtime: growing memories and tables, and calling
//! host functions. Each takes the execution context, whose `arguments` hold the operands.

use super::{FunctionInstance, FunctionKind, MemoryAddress, Store, TableAddress, Trap, Value};
use crate::abi::{
    ExecutionContext, FRAME_FUNCTION, FRAME_HEADER_SIZE, FRAME_INSTANCE, FRAME_RETURN_IP, FRAME_SAVED_FP, MemoryData,
    TableData,
};
use crate::validator::{CatchKind, NO_EXCEPTION_LOCAL, NOT_DELEGATED};

/// Export the runtime functions under the names the interpreter calls them by. The interpreter calls them with the
/// System V calling convention on every x86-64 target, Windows included.
macro_rules! export_to_interpreter {
    ($($symbol:ident => $function:ident),* $(,)?) => {
        $(
            /// # Safety
            ///
            /// Only the interpreter calls this, with the context of the store it runs in.
            #[cfg(all(target_arch = "x86_64", windows))]
            #[unsafe(no_mangle)]
            pub unsafe extern "sysv64" fn $symbol(context: *mut ExecutionContext) -> u64 {
                unsafe { $function(context) }
            }

            /// # Safety
            ///
            /// Only the interpreter calls this, with the context of the store it runs in.
            #[cfg(not(all(target_arch = "x86_64", windows)))]
            #[unsafe(no_mangle)]
            pub unsafe extern "C" fn $symbol(context: *mut ExecutionContext) -> u64 {
                unsafe { $function(context) }
            }
        )*
    };
}

export_to_interpreter! {
    wasm_runtime_memory_grow => memory_grow,
    wasm_runtime_table_grow => table_grow,
    wasm_runtime_call_host => call_host,
    wasm_runtime_allocate => allocate,
    wasm_runtime_throw => throw,
    wasm_runtime_wait => wait,
    wasm_runtime_notify => notify,
}

unsafe fn store_of(context: *mut ExecutionContext) -> &'static mut Store {
    unsafe { &mut *(*context).store.cast::<Store>() }
}

/// Grow the memory of `arguments[0]` (a `MemoryData`) by `arguments[1]` pages. Returns the previous size in pages, or
/// u64::MAX if the memory cannot grow.
///
/// # Safety
///
/// Only the interpreter calls this, with the context of the store it runs in.
unsafe fn memory_grow(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let data = (*context).arguments[0] as *const MemoryData;
        let delta = (*context).arguments[1];
        let store = store_of(context);
        // The data is at the memory's handle.
        let memory = MemoryAddress::from_ptr(data.cast());
        let Some(previous) = store.memory_mut(memory).grow(delta) else {
            return u64::MAX;
        };
        if let Some(callback) = store.memory_grow_callback.clone() {
            callback(memory);
        }
        previous
    }
}

/// Grow the table of `arguments[0]` (a `TableData`) by `arguments[1]` elements initialized to `arguments[2]`. Returns
/// the previous size, or u64::MAX if the table cannot grow.
///
/// # Safety
///
/// Only the interpreter calls this, with the context of the store it runs in.
unsafe fn table_grow(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let data = (*context).arguments[0] as *const TableData;
        let delta = (*context).arguments[1];
        let initial = (*context).arguments[2];
        let store = store_of(context);
        // The data is at the table's handle.
        store
            .table_mut(TableAddress::from_ptr(data.cast()))
            .grow(delta, initial)
            .unwrap_or(u64::MAX)
    }
}

/// Call the host function whose header is `arguments[0]` with the arguments on the operand stack at `arguments[1]`,
/// replacing them with its results. `arguments[2]` is the address the call returns to and `arguments[3]` the caller's
/// frame, which an exception the host function throws unwinds from. Returns the new top of the operand stack, 1 if
/// the call threw an exception that a handler caught (see `wasm_runtime_throw`), or 0 if it trapped.
///
/// # Safety
///
/// Only the interpreter calls this, with the context of the store it runs in.
unsafe fn call_host(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let function = &*((*context).arguments[0] as *const FunctionInstance);
        let top = (*context).arguments[1] as *mut u64;
        let return_ip = (*context).arguments[2];
        let frame = (*context).arguments[3];
        let store = store_of(context);
        let FunctionKind::Host(callback) = &function.kind else {
            return 0;
        };
        let callback = callback.clone();
        let parameter_slots = function.header.param_slots as usize;
        let slots = ak::slice_from_raw_parts(top, parameter_slots);
        // The operand stack grows down, so the first argument is in the highest slots.
        let mut arguments = Vec::with_capacity(function.ty.params.len());
        let mut slot = parameter_slots;
        for &ty in &function.ty.params {
            slot -= ty.slot_count() as usize;
            arguments.push(Value::from_slots(ty, &slots[slot..]));
        }
        let results = match callback.call(store, &arguments) {
            Ok(results) => results,
            Err(Trap::UncaughtException(exception)) => {
                // The exception continues in the caller, as if the call instruction had thrown it.
                if unwind(context, store, exception, frame, return_ip - 1) {
                    return 1;
                }
                store.pending_trap = Some(Trap::UncaughtException(exception));
                return 0;
            }
            Err(trap) => {
                store.pending_trap = Some(trap);
                return 0;
            }
        };
        let mut result_slots = Vec::new();
        for result in results.iter().rev() {
            result.push_slots(&mut result_slots);
        }
        let return_top = top.add(parameter_slots).sub(result_slots.len());
        for (index, value) in result_slots.iter().enumerate() {
            *return_top.add(index) = *value;
        }
        return_top as u64
    }
}

/// Allocate a GC object whose runtime type is `arguments[0]`, with `arguments[1]` bytes of zeroed data and, for an
/// array, `arguments[2]` elements. Returns zero if it is too large.
///
/// # Safety
///
/// Only the interpreter calls this, with the context of the store it runs in.
unsafe fn allocate(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let store = store_of(context);
        let rtt = (*context).arguments[0] as *const crate::abi::Rtt;
        let size = (*context).arguments[1];
        let length = (*context).arguments[2];
        let object = store.allocate_object(rtt, size, length);
        if object == 0 {
            store.pending_trap = Some(Trap::Host("out of memory".to_string()));
        }
        object
    }
}

/// Throw an exception: a new one with the tag whose identity is `arguments[0]` and the values on the operand stack
/// at `arguments[1]`, or the existing one `arguments[4]` refers to if that is not zero. `arguments[2]` is the
/// throwing instruction and `arguments[3]` its frame. Returns 1 if a handler caught the exception, with the frame,
/// instruction pointer, side-table pointer, and operand stack top to continue with in `arguments[0..4]`, or 0 if it
/// left the interpreter uncaught.
///
/// # Safety
///
/// Only the interpreter calls this, with the context of the store it runs in.
unsafe fn throw(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let store = store_of(context);
        let pc = (*context).arguments[2];
        let frame = (*context).arguments[3];
        let mut exception = (*context).arguments[4];
        if exception == 0 {
            let tag = store.tag_from_identity((*context).arguments[0]);
            let slot_count: u32 = store.tag(tag).ty.params.iter().map(|param| param.slot_count()).sum();
            let top = (*context).arguments[1] as *const u64;
            let payload = ak::slice_from_raw_parts(top, slot_count as usize).to_vec();
            exception = store.allocate_exception(tag, &payload);
        }
        if unwind(context, store, exception, frame, pc) {
            return 1;
        }
        store.pending_trap = Some(Trap::UncaughtException(exception));
        0
    }
}

/// Find the handler of `exception`, thrown by the instruction at `pc` in the frame `frame`, in that frame or the ones
/// it was called from, up to where the runtime entered the interpreter. If there is one, push what the handler
/// receives and put where it continues into the context's arguments.
pub(crate) unsafe fn unwind(
    context: *mut ExecutionContext,
    store: &Store,
    exception: u64,
    mut frame: u64,
    mut pc: u64,
) -> bool {
    let read = |address: u64| unsafe { *(address as *const u64) };
    let thrown_tag = store.exception(exception).tag;
    loop {
        // The frame the runtime enters the interpreter with has no instance.
        if read(frame.wrapping_add_signed(FRAME_INSTANCE as i64)) == 0 {
            return false;
        }
        let function = unsafe { &*(read(frame.wrapping_add_signed(FRAME_FUNCTION as i64)) as *const FunctionInstance) };
        if let FunctionKind::Wasm {
            module,
            index,
            instance,
        } = &function.kind
        {
            let info = &module.function_infos[(*index - module.imported_function_count) as usize];
            let offset = pc.wrapping_sub(module.bytes.as_ptr() as u64);
            // Regions are in instruction order, inner ones after the regions they are in. A legacy `delegate` leaves
            // out the regions that start after its bound.
            let mut bound = u32::MAX;
            for region in info.try_regions.iter().rev() {
                if offset < u64::from(region.start) || offset >= u64::from(region.end) || region.start > bound {
                    continue;
                }
                if region.delegate_bound != NOT_DELEGATED {
                    bound = region.delegate_bound;
                    continue;
                }
                let clauses = &info.catch_clauses
                    [region.first_clause as usize..(region.first_clause + region.clause_count) as usize];
                for clause in clauses {
                    let catches = match clause.kind {
                        CatchKind::Catch | CatchKind::CatchRef => {
                            store.instance(*instance).tags[clause.tag as usize] == thrown_tag
                        }
                        CatchKind::CatchAll | CatchKind::CatchAllRef => true,
                    };
                    if !catches {
                        continue;
                    }
                    let payload: &[u64] = match clause.kind {
                        CatchKind::Catch | CatchKind::CatchRef => store.exception(exception).payload,
                        CatchKind::CatchAll | CatchKind::CatchAllRef => &[],
                    };
                    let pushes_reference = matches!(clause.kind, CatchKind::CatchRef | CatchKind::CatchAllRef);
                    let pushed = payload.len() as u64 + u64::from(pushes_reference);
                    let empty_stack = frame - FRAME_HEADER_SIZE as u64;
                    let top = empty_stack - (u64::from(clause.height) + pushed) * 8;
                    let mut slot = top as *mut u64;
                    unsafe {
                        if clause.exception_local != NO_EXCEPTION_LOCAL {
                            *((frame + u64::from(clause.exception_local) * 8) as *mut u64) = exception;
                        }
                        if pushes_reference {
                            *slot = exception;
                            slot = slot.add(1);
                        }
                        for &value in payload {
                            *slot = value;
                            slot = slot.add(1);
                        }
                        (*context).arguments[0] = frame;
                        (*context).arguments[1] = module.bytes.as_ptr() as u64 + u64::from(clause.target_pc);
                        (*context).arguments[2] = info.side_table.as_ptr() as u64 + u64::from(clause.target_stp) * 16;
                        (*context).arguments[3] = top;
                    }
                    return true;
                }
            }
        }
        // Continue in the caller, inside the call instruction this frame returns past.
        pc = read(frame.wrapping_add_signed(FRAME_RETURN_IP as i64)) - 1;
        frame = read(frame.wrapping_add_signed(FRAME_SAVED_FP as i64));
    }
}

/// The threads waiting in `memory.atomic.wait`, with the address they wait on, oldest first.
static WAITERS: std::sync::Mutex<Vec<(u64, std::sync::Arc<Waiter>)>> = std::sync::Mutex::new(Vec::new());

struct Waiter {
    woken: std::sync::Mutex<bool>,
    condition: std::sync::Condvar,
}

/// Wait on the shared memory location at address `arguments[0]` while it holds `arguments[1]` (compared at
/// `arguments[3]` bytes), for at most `arguments[2]` nanoseconds if that is not negative. Returns 0 when woken, 1 if
/// the location held another value, and 2 when the time ran out.
///
/// # Safety
///
/// Only the interpreter calls this, with an address it checked.
unsafe fn wait(context: *mut ExecutionContext) -> u64 {
    let (address, expected, timeout, size) = unsafe {
        (
            (*context).arguments[0],
            (*context).arguments[1],
            (*context).arguments[2] as i64,
            (*context).arguments[3],
        )
    };
    let waiter = {
        let mut waiters = WAITERS.lock().expect("the waiter list is never poisoned");
        // Notifies take the lock, so none can come between reading the value and waiting for it.
        let current = unsafe {
            if size == 4 {
                u64::from((*(address as *const std::sync::atomic::AtomicU32)).load(std::sync::atomic::Ordering::SeqCst))
            } else {
                (*(address as *const std::sync::atomic::AtomicU64)).load(std::sync::atomic::Ordering::SeqCst)
            }
        };
        if current != expected {
            return 1;
        }
        let waiter = std::sync::Arc::new(Waiter {
            woken: std::sync::Mutex::new(false),
            condition: std::sync::Condvar::new(),
        });
        waiters.push((address, waiter.clone()));
        waiter
    };
    let mut woken = waiter.woken.lock().expect("a waiter is never poisoned");
    if timeout < 0 {
        while !*woken {
            woken = waiter.condition.wait(woken).expect("a waiter is never poisoned");
        }
        return 0;
    }
    let deadline = std::time::Instant::now() + std::time::Duration::from_nanos(timeout as u64);
    while !*woken {
        let now = std::time::Instant::now();
        if now >= deadline {
            break;
        }
        woken = waiter
            .condition
            .wait_timeout(woken, deadline - now)
            .expect("a waiter is never poisoned")
            .0;
    }
    if *woken {
        return 0;
    }
    drop(woken);
    let mut waiters = WAITERS.lock().expect("the waiter list is never poisoned");
    waiters.retain(|(_, other)| !std::sync::Arc::ptr_eq(other, &waiter));
    // A notify may have come in after the time ran out but before the waiter left the list.
    if *waiter.woken.lock().expect("a waiter is never poisoned") {
        0
    } else {
        2
    }
}

/// Wake at most `arguments[1]` of the threads waiting on the shared memory location at address `arguments[0]`, and
/// return how many it woke.
///
/// # Safety
///
/// Only the interpreter calls this.
unsafe fn notify(context: *mut ExecutionContext) -> u64 {
    let (address, count) = unsafe { ((*context).arguments[0], (*context).arguments[1]) };
    let mut waiters = WAITERS.lock().expect("the waiter list is never poisoned");
    let mut woken = 0;
    waiters.retain(|(waiting_on, waiter)| {
        if woken >= count || *waiting_on != address {
            return true;
        }
        *waiter.woken.lock().expect("a waiter is never poisoned") = true;
        waiter.condition.notify_one();
        woken += 1;
        false
    });
    woken
}
