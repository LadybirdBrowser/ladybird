/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Entering the interpreter, and turning faulting memory accesses inside it into traps.

use crate::abi::EntryRecord;

/// The pseudo-instructions the runtime enters the interpreter at: Enter pushes the arguments, CallEntry calls the
/// function, and the function returns to Exit, which leaves the interpreter.
static ENTRY_CODE: [u8; 3] = [0xf0, 0xf3, 0xf1];

/// The opcode of the pseudo-instruction a faulting memory access continues at.
#[cfg(wasm_interpreter)]
const MEMORY_FAULT_OPCODE: usize = 0xf2;

// The interpreter uses the System V calling convention on every x86-64 target, Windows included.
#[cfg(all(wasm_interpreter, not(all(target_arch = "x86_64", windows))))]
unsafe extern "C" {
    fn wasm_interpreter(ip: *const u8, argument: u64);
}

#[cfg(all(wasm_interpreter, target_arch = "x86_64", windows))]
unsafe extern "sysv64" {
    fn wasm_interpreter(ip: *const u8, argument: u64);
}

#[cfg(wasm_interpreter)]
unsafe extern "C" {
    static wasm_interpreter_end: u8;
    static wasm_interpreter_dispatch_table: [usize; 256];
}

/// Run the function of `record` in the interpreter.
///
/// # Safety
///
/// The record must describe a function of a live instance, its arguments, and room for its results.
pub unsafe fn enter(record: &mut EntryRecord) {
    #[cfg(wasm_interpreter)]
    unsafe {
        wasm_interpreter(ENTRY_CODE.as_ptr(), (record as *mut EntryRecord) as u64);
    }
    #[cfg(not(wasm_interpreter))]
    {
        let _ = &ENTRY_CODE;
        record.trap = crate::abi::TRAP_RUNTIME;
    }
}

#[cfg(wasm_interpreter)]
fn interpreter_code_range() -> (usize, usize) {
    (
        wasm_interpreter as *const () as usize,
        &raw const wasm_interpreter_end as usize,
    )
}

/// The handler of a primary opcode.
#[cfg(wasm_interpreter)]
pub fn dispatch_table_entry(opcode: u8) -> usize {
    unsafe { wasm_interpreter_dispatch_table[usize::from(opcode)] }
}

#[cfg(wasm_interpreter)]
fn memory_fault_handler() -> usize {
    unsafe { wasm_interpreter_dispatch_table[MEMORY_FAULT_OPCODE] }
}

/// The CPU features the interpreter's code uses and that the target's baseline guarantees: x86-64-v3 on x86-64, and
/// ARMv8.2-A on AArch64. A CPU without them would not fault but compute wrong results (lzcnt runs as bsr on older
/// CPUs), so the engine refuses to start instead.
pub fn check_cpu_features() {
    let missing = missing_cpu_features();
    assert!(
        missing.is_empty(),
        "The WebAssembly engine needs CPU features this machine lacks: {}",
        missing.join(", ")
    );
}

#[cfg(target_arch = "x86_64")]
fn missing_cpu_features() -> Vec<&'static str> {
    let mut missing = Vec::new();
    macro_rules! require {
        ($($feature:tt),+) => {
            $(if !std::arch::is_x86_feature_detected!($feature) {
                missing.push($feature);
            })+
        };
    }
    require!(
        "sse4.1", "sse4.2", "popcnt", "lzcnt", "bmi1", "bmi2", "avx2", "fma", "movbe"
    );
    missing
}

#[cfg(target_arch = "aarch64")]
fn missing_cpu_features() -> Vec<&'static str> {
    let mut missing = Vec::new();
    macro_rules! require {
        ($($feature:tt),+) => {
            $(if !std::arch::is_aarch64_feature_detected!($feature) {
                missing.push($feature);
            })+
        };
    }
    require!("neon", "lse", "rdm");
    missing
}

#[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
fn missing_cpu_features() -> Vec<&'static str> {
    Vec::new()
}

/// The lowest address the interpreter's frames may use on the current thread, leaving `reserve` bytes below it.
pub fn stack_limit(reserve: usize) -> u64 {
    // OPTIMIZATION: Finding where a thread's stack ends can mean reading /proc/self/maps (glibc does for the main
    //               thread), which took over half a millisecond per call from JavaScript into WebAssembly, so each
    //               thread finds it once.
    thread_local! {
        static LOWEST: std::cell::Cell<usize> = const { std::cell::Cell::new(0) };
    }
    let lowest = LOWEST.with(|lowest| {
        if lowest.get() == 0 {
            let marker = 0u8;
            let current = &raw const marker as usize;
            lowest.set(thread_stack_lowest_address().unwrap_or(current.saturating_sub(1024 * 1024)));
        }
        lowest.get()
    });
    (lowest + reserve) as u64
}

#[cfg(target_os = "linux")]
fn thread_stack_lowest_address() -> Option<usize> {
    unsafe {
        let mut attributes: libc::pthread_attr_t = std::mem::zeroed();
        if libc::pthread_getattr_np(libc::pthread_self(), &raw mut attributes) != 0 {
            return None;
        }
        let mut address = std::ptr::null_mut();
        let mut size = 0;
        let result = libc::pthread_attr_getstack(&raw const attributes, &raw mut address, &raw mut size);
        libc::pthread_attr_destroy(&raw mut attributes);
        (result == 0).then_some(address as usize)
    }
}

#[cfg(target_os = "macos")]
#[allow(clippy::unnecessary_wraps)] // The other platforms' versions can fail.
fn thread_stack_lowest_address() -> Option<usize> {
    unsafe {
        let thread = libc::pthread_self();
        let top = libc::pthread_get_stackaddr_np(thread) as usize;
        Some(top - libc::pthread_get_stacksize_np(thread))
    }
}

#[cfg(windows)]
#[allow(clippy::unnecessary_wraps)] // The other platforms' versions can fail.
fn thread_stack_lowest_address() -> Option<usize> {
    unsafe extern "system" {
        fn GetCurrentThreadStackLimits(low: *mut usize, high: *mut usize);
    }
    let (mut low, mut high) = (0, 0);
    unsafe { GetCurrentThreadStackLimits(&raw mut low, &raw mut high) };
    Some(low)
}

#[cfg(not(any(target_os = "linux", target_os = "macos", windows)))]
fn thread_stack_lowest_address() -> Option<usize> {
    None
}

#[cfg(all(wasm_interpreter, unix))]
mod fault {
    use super::{interpreter_code_range, memory_fault_handler};
    use crate::runtime::memory::is_in_any_reservation;
    use std::sync::Once;

    static INSTALL: Once = Once::new();
    static mut PREVIOUS_SIGSEGV: libc::sigaction = unsafe { std::mem::zeroed() };
    static mut PREVIOUS_SIGBUS: libc::sigaction = unsafe { std::mem::zeroed() };

    #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
    unsafe fn instruction_pointer(context: *mut libc::c_void) -> *mut usize {
        unsafe {
            let context = context.cast::<libc::ucontext_t>();
            (&raw mut (*context).uc_mcontext.gregs[libc::REG_RIP as usize]).cast()
        }
    }

    #[cfg(all(target_os = "linux", target_arch = "aarch64"))]
    unsafe fn instruction_pointer(context: *mut libc::c_void) -> *mut usize {
        unsafe {
            let context = context.cast::<libc::ucontext_t>();
            (&raw mut (*context).uc_mcontext.pc).cast()
        }
    }

    #[cfg(all(target_os = "macos", target_arch = "x86_64"))]
    unsafe fn instruction_pointer(context: *mut libc::c_void) -> *mut usize {
        unsafe {
            let context = context.cast::<libc::ucontext_t>();
            (&raw mut (*(*context).uc_mcontext).__ss.__rip).cast()
        }
    }

    #[cfg(all(target_os = "macos", target_arch = "aarch64"))]
    unsafe fn instruction_pointer(context: *mut libc::c_void) -> *mut usize {
        unsafe {
            let context = context.cast::<libc::ucontext_t>();
            (&raw mut (*(*context).uc_mcontext).__ss.__pc).cast()
        }
    }

    unsafe fn forward(signal: i32, info: *mut libc::siginfo_t, context: *mut libc::c_void) {
        unsafe {
            let previous = if signal == libc::SIGSEGV {
                &raw const PREVIOUS_SIGSEGV
            } else {
                &raw const PREVIOUS_SIGBUS
            };
            let previous = &*previous;
            if previous.sa_flags & libc::SA_SIGINFO != 0 && previous.sa_sigaction > libc::SIG_IGN {
                let handler: extern "C" fn(i32, *mut libc::siginfo_t, *mut libc::c_void) =
                    std::mem::transmute(previous.sa_sigaction);
                handler(signal, info, context);
                return;
            }
            if previous.sa_sigaction > libc::SIG_IGN {
                let handler: extern "C" fn(i32) = std::mem::transmute(previous.sa_sigaction);
                handler(signal);
                return;
            }
            // Return with the default action in place, so that the faulting access faults again and kills the
            // process as it would have without us.
            libc::signal(signal, libc::SIG_DFL);
        }
    }

    extern "C" fn handle_fault(signal: i32, info: *mut libc::siginfo_t, context: *mut libc::c_void) {
        unsafe {
            let pc = instruction_pointer(context);
            let (start, end) = interpreter_code_range();
            let address = (*info).si_addr() as usize;
            if (start..end).contains(&*pc) && is_in_any_reservation(address) {
                // The handlers keep their frame in the pinned registers, so the trap handler can find the
                // execution context of the access from the frame pointer.
                *pc = memory_fault_handler();
                return;
            }
            if is_in_any_reservation(address) && crate::runtime::jit::redirect_fault(context) {
                return;
            }
            forward(signal, info, context);
        }
    }

    pub fn install() {
        INSTALL.call_once(|| unsafe {
            let mut action: libc::sigaction = std::mem::zeroed();
            action.sa_sigaction = handle_fault as *const () as usize;
            action.sa_flags = libc::SA_SIGINFO | libc::SA_ONSTACK;
            libc::sigemptyset(&raw mut action.sa_mask);
            libc::sigaction(libc::SIGSEGV, &raw const action, &raw mut PREVIOUS_SIGSEGV);
            libc::sigaction(libc::SIGBUS, &raw const action, &raw mut PREVIOUS_SIGBUS);
        });
    }
}

/// Windows reports faults to vectored exception handlers before it looks for handlers in the faulting thread's frames.
#[cfg(all(wasm_interpreter, windows))]
mod fault {
    use super::{interpreter_code_range, memory_fault_handler};
    use crate::runtime::memory::is_in_any_reservation;
    use std::ffi::c_void;
    use std::sync::Once;

    const EXCEPTION_ACCESS_VIOLATION: u32 = 0xc000_0005;
    const EXCEPTION_CONTINUE_EXECUTION: i32 = -1;
    const EXCEPTION_CONTINUE_SEARCH: i32 = 0;
    /// Where Rip is in the x86-64 CONTEXT structure.
    const CONTEXT_RIP_OFFSET: usize = 0xf8;

    #[repr(C)]
    struct ExceptionRecord {
        code: u32,
        flags: u32,
        record: *mut ExceptionRecord,
        address: *mut c_void,
        parameter_count: u32,
        /// For access violations, whether the access was a write, and the address it accessed.
        information: [usize; 15],
    }

    #[repr(C)]
    struct ExceptionPointers {
        record: *mut ExceptionRecord,
        context: *mut u8,
    }

    unsafe extern "system" {
        fn AddVectoredExceptionHandler(
            first: u32,
            handler: unsafe extern "system" fn(*mut ExceptionPointers) -> i32,
        ) -> *mut c_void;
    }

    static INSTALL: Once = Once::new();

    unsafe extern "system" fn handle_exception(pointers: *mut ExceptionPointers) -> i32 {
        unsafe {
            let record = &*(*pointers).record;
            if record.code != EXCEPTION_ACCESS_VIOLATION || record.parameter_count < 2 {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            let pc = (*pointers).context.add(CONTEXT_RIP_OFFSET).cast::<usize>();
            let (start, end) = interpreter_code_range();
            if (start..end).contains(&*pc) && is_in_any_reservation(record.information[1]) {
                // The handlers keep their frame in the pinned registers, so the trap handler can find the
                // execution context of the access from the frame pointer.
                *pc = memory_fault_handler();
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            EXCEPTION_CONTINUE_SEARCH
        }
    }

    pub fn install() {
        INSTALL.call_once(|| unsafe {
            AddVectoredExceptionHandler(1, handle_exception);
        });
    }
}

pub fn install_fault_handler() {
    #[cfg(wasm_interpreter)]
    fault::install();
}
