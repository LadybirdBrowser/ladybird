/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime's side of the JIT (`libwasm_jit`): when functions are compiled, where their code lives, and the calls
//! between compiled code and the rest of the runtime.
//!
//! - A function with compiled code has `FUNCTION_KIND_COMPILED` and its code at `native_entry`. Every other function
//!   has the runtime adapter there, through which compiled code calls it (`call_from_native`). The interpreter calls
//!   compiled functions through the entry trampoline (`call_compiled`).
//! - Compiled code traps through the trap exit, which leaves the interpreter's innermost entry like a trap in the
//!   interpreter does. A memory access of compiled code that faults goes to the trap exit too.
//! - Compiled code handles the exceptions of its own handlers itself. An exception that leaves a compiled function,
//!   or comes out of a call through the runtime adapter, is unwound here over the compiled frames, to the first
//!   call that handles exceptions (an exception site) or to the entry trampoline, which continue as if the call had
//!   returned, with the exception in the context. From the trampoline, the exception goes on in the interpreter
//!   frame that called the compiled code.
//! - Each store has its own code space; the trap exit, the trampoline and the adapter are the process's.
//!
//! - Hot functions compile on the process's compile threads while the interpreter keeps running them. The runtime
//!   installs finished code when the interpreter asks it again, which it does every few calls or loop iterations of a
//!   function whose compile is pending.
//!
//! `LIBWASM_JIT` configures it, as a comma-separated list: `off` (the default for now), `on` (tier up hot functions),
//! `threshold=N` (tier up once calls and loop iterations cost N, 1000 by default), `forced` (tier up at the first call
//! or loop), `eager` (compile every function when its module is instantiated), and `perf-map` (write
//! `/tmp/perf-PID.map` for `perf`), `verify-ir` (check every compile's IR and register allocation), `stress-osr`
//! (tier up at loops only, where they start, so that as many loops as can continue compiled), `sync` (compile on the
//! thread that runs the code, as `forced` and `stress-osr` do too), and `threads=N` (the number of compile threads,
//! one per processor core but one by default).

use super::ranges::RangeSet;
use super::{FunctionAddress, FunctionInstance, InstanceAddress, Store, Trap, Value};
use crate::abi::ExecutionContext;
use crate::types::ValType;
use std::sync::OnceLock;

/// The code of compiled functions and of the stubs, for the fault handler.
pub static CODE_RANGES: RangeSet = RangeSet::new();

#[derive(Debug, Default, Clone, Copy)]
pub struct Options {
    pub enabled: bool,
    /// Compile every function when its module is instantiated.
    pub eager: bool,
    /// The tier-up budget of a function: compile it once calls (10 each) and loop iterations (1 each) used it up.
    pub threshold: u32,
    /// Write `/tmp/perf-PID.map`, which tells `perf` what code is where.
    pub perf_map: bool,
    /// Check the IR and the register allocation of every compile.
    pub verify_ir: bool,
    /// Tier up at loops only, at the first iteration, so that loops continue compiled (OSR) as often as they can.
    pub stress_osr: bool,
    /// Compile on the thread that runs the code, and run the code as soon as it is compiled.
    pub sync: bool,
    /// The number of compile threads, or 0 for one per processor core but one.
    pub threads: u32,
}

impl Options {
    fn compile_options(self) -> libwasm_jit::Options {
        libwasm_jit::Options {
            verify_ir: self.verify_ir,
            ..libwasm_jit::Options::default()
        }
    }
}

pub fn options() -> Options {
    static OPTIONS: OnceLock<Options> = OnceLock::new();
    *OPTIONS.get_or_init(|| {
        let mut options = Options {
            threshold: DEFAULT_THRESHOLD,
            ..Options::default()
        };
        if let Ok(value) = std::env::var("LIBWASM_JIT") {
            for option in value.split(',') {
                match option.trim() {
                    "off" => options.enabled = false,
                    "on" => options.enabled = true,
                    "eager" => {
                        options.enabled = true;
                        options.eager = true;
                    }
                    "forced" => {
                        options.enabled = true;
                        options.sync = true;
                        options.threshold = 0;
                    }
                    "sync" => options.sync = true,
                    "perf-map" => options.perf_map = true,
                    "verify-ir" => options.verify_ir = true,
                    "stress-osr" => {
                        options.enabled = true;
                        options.sync = true;
                        options.stress_osr = true;
                        options.threshold = 1;
                    }
                    other => {
                        if let Some(threshold) = other.strip_prefix("threshold=").and_then(|value| value.parse().ok()) {
                            options.enabled = true;
                            options.threshold = threshold;
                        } else if let Some(threads) =
                            other.strip_prefix("threads=").and_then(|value| value.parse().ok())
                        {
                            options.threads = threads;
                        }
                    }
                }
            }
        }
        options.enabled &= cfg!(all(unix, target_arch = "x86_64"));
        options
    })
}

/// Export the runtime functions compiled code and the interpreter call, under the names the interpreter calls them
/// by, with the System V calling convention on every x86-64 target.
macro_rules! export {
    ($($symbol:ident => $function:ident),* $(,)?) => {
        $(
            /// # Safety
            ///
            /// Only the interpreter and compiled code call this, with the context of the store they run in.
            #[cfg(all(target_arch = "x86_64", windows))]
            #[unsafe(no_mangle)]
            pub unsafe extern "sysv64" fn $symbol(context: *mut ExecutionContext) -> u64 {
                unsafe { $function(context) }
            }

            /// # Safety
            ///
            /// Only the interpreter and compiled code call this, with the context of the store they run in.
            #[cfg(not(all(target_arch = "x86_64", windows)))]
            #[unsafe(no_mangle)]
            pub unsafe extern "C" fn $symbol(context: *mut ExecutionContext) -> u64 {
                unsafe { $function(context) }
            }
        )*
    };
}

export! {
    wasm_runtime_call_compiled => call_compiled,
    wasm_runtime_call_from_native => call_from_native,
    wasm_runtime_tier_up => tier_up,
    wasm_runtime_allocate_exception => allocate_exception,
    wasm_runtime_throw_from_native => throw_from_native,
}

/// Called when a function's tier-up budget runs out: `arguments[0]` is its header, and `arguments[2]` is 0 for a call
/// and 1 for a loop, whose instruction `arguments[1]` points at, in the frame at `arguments[3]`. Compiles the function
/// if the JIT is on, or has it compiled in the background. At a loop, continues the frame in compiled code if it can
/// (OSR) and returns the top of the frame's operand stack with the results on it, or if the frame threw an exception,
/// 1 if a handler of the interpreter caught it and 2 if it left the interpreter; returns 0 otherwise.
unsafe fn tier_up(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let [header, pc, reason, frame, ..] = (*context).arguments;
        let address = FunctionAddress::from_ptr(header as *const FunctionInstance);
        let store = store_of(context);
        if !options().enabled {
            address.get_mut().header.tier_up_budget = u32::MAX;
            return 0;
        }
        if options().stress_osr && reason == 0 {
            // NB: The interpreter charges the call once this returns, which leaves nothing for the first loop.
            address.get_mut().header.tier_up_budget = crate::abi::TIER_UP_CALL_COST;
            return 0;
        }
        let at_loop = reason == 1;
        // NB: The interpreter charges a call once this returns, so every budget covers that.
        let state = native::compile_function(store, address);
        address.get_mut().header.tier_up_budget = match state {
            // A compiled function keeps a budget, so that frames of it still running in the interpreter continue
            // compiled at their next outermost loop.
            native::CompileState::Compiled => options().threshold.max(1) + crate::abi::TIER_UP_CALL_COST,
            native::CompileState::Pending => POLL_BUDGET + crate::abi::TIER_UP_CALL_COST,
            native::CompileState::Failed => u32::MAX,
        };
        if !at_loop || state == native::CompileState::Failed {
            return 0;
        }
        match native::enter_at_loop(store, address, pc, frame) {
            native::LoopEntry::Ran(top) => top,
            native::LoopEntry::Threw(exception) => {
                // The frame's own handlers had their chance in the compiled code, so the exception goes on in the
                // frame's caller, at the call instruction it returns past.
                let read = |offset: isize| *((frame as *const u8).offset(offset) as *const u64);
                let caller = read(crate::abi::FRAME_SAVED_FP);
                let call = read(crate::abi::FRAME_RETURN_IP) - 1;
                // NB: 0 means the loop goes on in the interpreter, so a trap is 2 here.
                match rethrow_in_interpreter(context, store, exception, caller, call) {
                    0 => 2,
                    resumed => resumed,
                }
            }
            native::LoopEntry::Pending => {
                address.get_mut().header.tier_up_budget = POLL_BUDGET + crate::abi::TIER_UP_CALL_COST;
                0
            }
            native::LoopEntry::Never => 0,
        }
    }
}

/// Continues an exception that compiled code did not handle in the interpreter, at the instruction `pc` of the frame
/// `frame`. Returns 1 if a handler there caught it (see `wasm_runtime_throw`), or 0 if it leaves the interpreter, as a
/// trap the store keeps.
unsafe fn rethrow_in_interpreter(
    context: *mut ExecutionContext,
    store: &mut Store,
    exception: u64,
    frame: u64,
    pc: u64,
) -> u64 {
    unsafe {
        if super::helpers::unwind(context, store, exception, frame, pc) {
            return 1;
        }
        store.pending_trap = Some(Trap::UncaughtException(exception));
        0
    }
}

/// `Helper::AllocateException`: a new exception with the tag whose identity is `arguments[0]` and a zeroed payload.
unsafe fn allocate_exception(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let store = store_of(context);
        let tag = store.tag_from_identity((*context).arguments[0]);
        let slot_count: u32 = store.tag(tag).ty.params.iter().map(|param| param.slot_count()).sum();
        store.allocate_exception(tag, &vec![0; slot_count as usize])
    }
}

/// Throws the exception `arguments[0]` from the compiled frame whose fp is `arguments[1]`, whose own handlers did not
/// catch it. Returns the context, whose `resume` says where the code continues.
unsafe fn throw_from_native(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let [exception, frame, ..] = (*context).arguments;
        native::unwind(store_of(context), exception, frame);
        context as u64
    }
}

/// The budget of a function whose compile is pending: how much it runs in the interpreter before the runtime looks
/// for the compile's result again.
const POLL_BUDGET: u32 = 500;

unsafe fn store_of(context: *mut ExecutionContext) -> &'static mut Store {
    unsafe { &mut *(*context).store.cast::<Store>() }
}

const DEFAULT_THRESHOLD: u32 = 1000;

/// What `tier_up_budget` starts as in a new function's header. Without the JIT, the runtime is asked again only every
/// few billion calls and loop iterations.
pub fn initial_tier_up_budget() -> u32 {
    let options = options();
    if options.enabled && !options.eager {
        options.threshold
    } else {
        u32::MAX
    }
}

/// What `native_entry` starts as in a new function's header.
pub fn initial_native_entry() -> u64 {
    if !options().enabled {
        return 0;
    }
    native::stubs().adapter
}

impl Store {
    /// Keeps everything in this store interpreted, whatever `LIBWASM_JIT` says, as tests comparing the tiers want.
    pub fn disable_jit(&mut self) {
        self.jit.get_or_insert_with(Default::default).disabled = true;
    }
}

/// Called once a module is instantiated, before its start function runs.
pub fn instantiated(store: &mut Store, instance: InstanceAddress) {
    if options().eager {
        native::compile_instance(store, instance);
    }
}

/// Redirects a memory fault in compiled code to the trap exit. `context` is the signal handler's.
///
/// # Safety
///
/// Only the fault handler calls this, with its context.
#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
pub unsafe fn redirect_fault(context: *mut libc::c_void) -> bool {
    unsafe {
        let context = context.cast::<libc::ucontext_t>();
        let registers = &mut (*context).uc_mcontext.gregs;
        let pc = registers[libc::REG_RIP as usize] as usize;
        let Some(stubs) = native::STUBS.get() else {
            return false;
        };
        if !CODE_RANGES.contains(pc) {
            return false;
        }
        registers[libc::REG_RIP as usize] = stubs.trap_exit as i64;
        registers[libc::REG_RDI as usize] = i64::from(crate::abi::TRAP_MEMORY_OUT_OF_BOUNDS);
        true
    }
}

#[cfg(not(all(target_os = "linux", target_arch = "x86_64")))]
pub unsafe fn redirect_fault(_context: *mut libc::c_void) -> bool {
    false
}

/// Appends a line for code at `address` to the process's perf map.
fn write_perf_map_entry(address: u64, size: usize, name: &str) {
    let _ = ak::perf_map::write_perf_map_entry(address as usize, size, name);
}

/// The TRAP_* code a trap of the runtime continues as in the interpreter, which keeps the details in the store.
fn trap_code(trap: &Trap) -> u32 {
    use crate::abi::*;
    match trap {
        Trap::Unreachable => TRAP_UNREACHABLE,
        Trap::StackExhausted => TRAP_STACK_EXHAUSTED,
        Trap::MemoryOutOfBounds => TRAP_MEMORY_OUT_OF_BOUNDS,
        Trap::IntegerDivideByZero => TRAP_INTEGER_DIVIDE_BY_ZERO,
        Trap::IntegerOverflow => TRAP_INTEGER_OVERFLOW,
        Trap::InvalidConversion => TRAP_INVALID_CONVERSION,
        Trap::UndefinedElement => TRAP_UNDEFINED_ELEMENT,
        Trap::UninitializedElement => TRAP_UNINITIALIZED_ELEMENT,
        Trap::IndirectCallTypeMismatch => TRAP_INDIRECT_CALL_TYPE_MISMATCH,
        Trap::TableOutOfBounds => TRAP_TABLE_OUT_OF_BOUNDS,
        Trap::NullReference => TRAP_NULL_REFERENCE,
        Trap::CastFailure => TRAP_CAST_FAILURE,
        Trap::ArrayOutOfBounds => TRAP_ARRAY_OUT_OF_BOUNDS,
        Trap::UnalignedAtomic => TRAP_UNALIGNED_ATOMIC,
        Trap::ExpectedSharedMemory => TRAP_EXPECTED_SHARED_MEMORY,
        Trap::Host(_) => TRAP_RUNTIME,
        Trap::UncaughtException(_) => TRAP_UNCAUGHT_EXCEPTION,
    }
}

/// The value of type `ty` in `words`, as compiled code holds it: zero-extended i32 values, f32 values in the low 32
/// bits.
fn value_from_words(ty: ValType, words: &[u64]) -> Value {
    match ty {
        ValType::I32 => Value::I32(words[0] as u32 as i32),
        ValType::I64 => Value::I64(words[0] as i64),
        ValType::F32 => Value::F32(words[0] as u32),
        ValType::F64 => Value::F64(words[0]),
        ValType::Ref(_) => Value::Ref(words[0]),
        ValType::V128 => Value::V128(u128::from(words[0]) | (u128::from(words[1]) << 64)),
    }
}

fn words_of_value(value: &Value, words: &mut [u64]) {
    match *value {
        Value::I32(value) => words[0] = u64::from(value as u32),
        Value::I64(value) => words[0] = value as u64,
        Value::F32(bits) => words[0] = u64::from(bits),
        Value::F64(bits) | Value::Ref(bits) => words[0] = bits,
        Value::V128(bits) => {
            words[0] = bits as u64;
            words[1] = (bits >> 64) as u64;
        }
    }
}

fn ir_types(types: &[ValType]) -> Vec<libwasm_jit::ir::Type> {
    types
        .iter()
        .map(|ty| libwasm_jit::builder::ir_type(*ty).expect("the JIT has a type for every value"))
        .collect()
}

/// Calls a compiled function from the interpreter: `arguments[0]` is its header and `arguments[1]` the top of the
/// operand stack, where its arguments are, `arguments[2]` where the call returns to and `arguments[3]` the caller's
/// frame. Replaces the arguments with its results and returns the new top, or if it threw an exception, 1 if a
/// handler of the interpreter caught it, or 0 if it left the interpreter (see `rethrow_in_interpreter`).
unsafe fn call_compiled(context: *mut ExecutionContext) -> u64 {
    unsafe {
        match native::call_compiled(context) {
            Ok(top) => top,
            Err(exception) => {
                let [_, _, return_ip, frame, ..] = (*context).arguments;
                rethrow_in_interpreter(context, store_of(context), exception, frame, return_ip - 1)
            }
        }
    }
}

/// Calls a function that runs in the runtime from compiled code, through the runtime adapter: `arguments[0]` is its
/// header, `arguments[1]` the adapter's save area, and `arguments[2]` the adapter's frame, above which the home
/// slots of the arguments are. Returns 0 with the results in place, or a trap code, with the trap in the store.
unsafe fn call_from_native(context: *mut ExecutionContext) -> u64 {
    use libwasm_jit::convention::{self, Place};
    unsafe {
        let [header, save_area, frame, ..] = (*context).arguments;
        let function = &*(header as *const FunctionInstance);
        let ty = function.ty.clone();
        let save_area = save_area as *mut u64;
        let params = ir_types(&ty.params);
        let mut places = Vec::new();
        convention::parameter_places(&params, &mut places);
        let (mut gprs, mut xmms) = (0, 0);
        let mut arguments = Vec::with_capacity(params.len());
        for (index, place) in places.iter().enumerate() {
            let words = match *place {
                Place::Register(_) if params[index].uses_xmm() => {
                    xmms += 1;
                    ak::slice_from_raw_parts(save_area.add(6 + 2 * (xmms - 1)), 2)
                }
                Place::Register(_) => {
                    gprs += 1;
                    ak::slice_from_raw_parts(save_area.add(gprs - 1), 1)
                }
                Place::Memory(slot) => {
                    ak::slice_from_raw_parts((frame as *const u64).add(slot as usize), params[index].slots() as usize)
                }
            };
            arguments.push(value_from_words(ty.params[index], words));
        }
        let store = store_of(context);
        let address = FunctionAddress::from_ptr(function);
        let results = match store.invoke(address, &arguments) {
            Ok(results) => results,
            Err(Trap::UncaughtException(exception)) => {
                // NB: The adapter's frame is a frame of the compiled caller's callee.
                native::unwind(store, exception, frame);
                return libwasm_jit::stubs::ADAPTER_RESUME;
            }
            Err(trap) => {
                let code = trap_code(&trap);
                store.pending_trap = Some(trap);
                return u64::from(code);
            }
        };
        let result_types = ir_types(&ty.results);
        convention::result_places(&result_types, &mut places);
        let area = *(frame as *const u64).add(convention::result_area_pointer(&params) as usize) as *mut u64;
        let (mut gprs, mut xmms) = (0, 0);
        for (index, place) in places.iter().enumerate() {
            let words = match *place {
                Place::Register(_) if result_types[index].uses_xmm() => {
                    xmms += 1;
                    ak::slice_from_raw_parts_mut(save_area.add(6 + 2 * (xmms - 1)), 2)
                }
                Place::Register(_) => {
                    gprs += 1;
                    ak::slice_from_raw_parts_mut(save_area.add(gprs - 1), 1)
                }
                Place::Memory(slot) => {
                    ak::slice_from_raw_parts_mut(area.add(slot as usize), result_types[index].slots() as usize)
                }
            };
            words_of_value(&results[index], words);
        }
        0
    }
}

#[cfg(all(unix, target_arch = "x86_64"))]
mod native {
    use super::{CODE_RANGES, ir_types, value_from_words, words_of_value};
    use crate::abi::{ExecutionContext, FUNCTION_KIND_COMPILED, FunctionHeader, OPCODE_TRAP_FROM_NATIVE};
    use crate::module::Module;
    use crate::runtime::{FunctionAddress, FunctionInstance, FunctionKind, InstanceAddress, Store, helpers};
    use libwasm_jit::convention::{self, Place};
    use libwasm_jit::stubs::NativeCall;
    use libwasm_jit::{CompileContext, CompileRequest, CompiledFunction, RuntimeInfo};
    use std::collections::{HashMap, HashSet};
    use std::sync::mpsc::{Receiver, Sender, channel};
    use std::sync::{Arc, Mutex, OnceLock};

    /// The code the process's stores share.
    pub struct Stubs {
        pub trap_exit: u64,
        pub entry: u64,
        /// Where the entry trampoline's call of compiled code returns to.
        pub entry_return: u64,
        pub adapter: u64,
        pub runtime: RuntimeInfo,
    }

    pub static STUBS: OnceLock<Stubs> = OnceLock::new();

    pub fn stubs() -> &'static Stubs {
        STUBS.get_or_init(|| {
            let handler = crate::runtime::interpreter::dispatch_table_entry(OPCODE_TRAP_FROM_NATIVE) as u64;
            // NB: The trap exit goes first, so that the others can jump to it. The chunk is never freed.
            let mut chunk = CodeChunk::new(64 * 1024);
            let trap_exit = chunk.add(&libwasm_jit::stubs::trap_exit(handler));
            let runtime = RuntimeInfo {
                trap_exit,
                memory_grow: helpers::wasm_runtime_memory_grow as *const () as u64,
                table_grow: helpers::wasm_runtime_table_grow as *const () as u64,
                call_host: helpers::wasm_runtime_call_host as *const () as u64,
                allocate: helpers::wasm_runtime_allocate as *const () as u64,
                wait: helpers::wasm_runtime_wait as *const () as u64,
                notify: helpers::wasm_runtime_notify as *const () as u64,
                allocate_exception: super::wasm_runtime_allocate_exception as *const () as u64,
                throw: super::wasm_runtime_throw_from_native as *const () as u64,
            };
            let (trampoline, return_offset) = libwasm_jit::stubs::entry_trampoline();
            let entry = chunk.add(&trampoline);
            let adapter_code =
                libwasm_jit::stubs::runtime_adapter(super::wasm_runtime_call_from_native as *const () as u64, &runtime);
            let adapter = chunk.add(&adapter_code);
            if super::options().perf_map {
                super::write_perf_map_entry(entry, trampoline.len(), "wasm-entry-trampoline");
                super::write_perf_map_entry(adapter, adapter_code.len(), "wasm-runtime-adapter");
            }
            std::mem::forget(chunk);
            Stubs {
                trap_exit,
                entry,
                entry_return: entry + return_offset as u64,
                adapter,
                runtime,
            }
        })
    }

    /// Executable memory, written while it is not executable.
    struct CodeChunk {
        base: *mut u8,
        size: usize,
        used: usize,
    }

    const PAGE_SIZE: usize = 4096;
    const CHUNK_SIZE: usize = 16 << 20;

    impl CodeChunk {
        fn new(size: usize) -> Self {
            let base = unsafe {
                libc::mmap(
                    std::ptr::null_mut(),
                    size,
                    libc::PROT_READ | libc::PROT_EXEC,
                    libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_NORESERVE,
                    -1,
                    0,
                )
            };
            assert_ne!(base, libc::MAP_FAILED, "no memory for compiled code");
            assert!(
                CODE_RANGES.add(base as usize, size),
                "too much compiled code to fault in"
            );
            Self {
                base: base.cast(),
                size,
                used: 0,
            }
        }

        fn has_room(&self, length: usize) -> bool {
            self.used.next_multiple_of(16) + length <= self.size
        }

        /// Copies `code` in and returns its address.
        fn add(&mut self, code: &[u8]) -> u64 {
            let start = self.used.next_multiple_of(16);
            assert!(start + code.len() <= self.size);
            let first_page = start / PAGE_SIZE * PAGE_SIZE;
            let length = (start + code.len()).next_multiple_of(PAGE_SIZE) - first_page;
            unsafe {
                let region = self.base.add(first_page);
                assert_eq!(
                    libc::mprotect(region.cast(), length, libc::PROT_READ | libc::PROT_WRITE),
                    0
                );
                std::ptr::copy_nonoverlapping(code.as_ptr(), self.base.add(start), code.len());
                assert_eq!(
                    libc::mprotect(region.cast(), length, libc::PROT_READ | libc::PROT_EXEC),
                    0
                );
            }
            self.used = start + code.len();
            self.base as u64 + start as u64
        }
    }

    impl Drop for CodeChunk {
        fn drop(&mut self) {
            CODE_RANGES.remove(self.base as usize);
            unsafe { libc::munmap(self.base.cast(), self.size) };
        }
    }

    /// What a compile is of: a function of a module (by its address, which the store keeps alive), and for an OSR
    /// compile, the module offset of the loop it continues a frame at.
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    struct CompileKey {
        module: usize,
        function: u32,
        osr: Option<u32>,
    }

    /// A compile a compile thread finished, for the store that asked for it.
    struct Finished {
        key: CompileKey,
        code: Result<CompiledFunction, libwasm_jit::CompileFailure>,
    }

    struct Job {
        key: CompileKey,
        request: CompileRequest,
        reply: Sender<Finished>,
    }

    /// The process's compile threads, which every store sends its compiles to. Each thread takes the oldest job no
    /// other thread took yet, so jobs finish in any order.
    struct CompileThreads {
        jobs: Mutex<Sender<Job>>,
    }

    fn compile_threads() -> &'static CompileThreads {
        static THREADS: OnceLock<CompileThreads> = OnceLock::new();
        THREADS.get_or_init(|| {
            let (jobs, receiver) = channel::<Job>();
            let receiver = Arc::new(Mutex::new(receiver));
            let count = match super::options().threads {
                0 => std::thread::available_parallelism().map_or(1, |cores| cores.get().saturating_sub(1).max(1)),
                count => count as usize,
            };
            for index in 0..count {
                let receiver = Arc::clone(&receiver);
                let spawned = std::thread::Builder::new()
                    .name("LibWasm JIT".to_string())
                    .spawn(move || {
                        let mut context = CompileContext::default();
                        loop {
                            // NB: The lock is held only while waiting for a job, so the other threads compile
                            //     meanwhile.
                            let job = receiver.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).recv();
                            let Ok(job) = job else {
                                return;
                            };
                            let code = libwasm_jit::compile(&mut context, &job.request);
                            // NB: The store may be gone, and its results with it.
                            let _ = job.reply.send(Finished { key: job.key, code });
                        }
                    });
                // NB: The JIT needs one thread. If the process cannot start more, it compiles on those it started.
                if index == 0 {
                    spawned.expect("starting a WebAssembly compile thread");
                } else if spawned.is_err() {
                    break;
                }
            }
            CompileThreads { jobs: Mutex::new(jobs) }
        })
    }

    /// Where a store's function is in compiling.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum CompileState {
        Compiled,
        Pending,
        Failed,
    }

    /// What became of entering compiled code at a loop.
    pub enum LoopEntry {
        /// The frame ran to its end compiled; this is the top of its operand stack, with the results on it.
        Ran(u64),
        /// The frame threw this exception, which its own handlers did not catch.
        Threw(u64),
        /// The code for the loop is still compiling.
        Pending,
        /// The frame cannot continue compiled at this loop.
        Never,
    }

    /// A store's compiled code.
    #[derive(Default)]
    pub struct StoreJit {
        chunks: Vec<CodeChunk>,
        context: CompileContext,
        /// The code of each compile, and the compiles that failed.
        code: HashMap<CompileKey, u64>,
        failed: HashSet<CompileKey>,
        /// The return addresses of the calls in compiled code that handle exceptions.
        exception_sites: HashSet<u64>,
        /// The compiles the compile threads have yet to finish, and where they send them.
        pending: HashSet<CompileKey>,
        finished: Option<(Sender<Finished>, Receiver<Finished>)>,
        /// The modules of the keys, so that their addresses stay theirs.
        modules: HashMap<usize, Arc<Module>>,
        pub disabled: bool,
    }

    impl StoreJit {
        fn add_code(&mut self, code: &[u8]) -> u64 {
            if !self.chunks.last().is_some_and(|chunk| chunk.has_room(code.len())) {
                self.chunks
                    .push(CodeChunk::new(CHUNK_SIZE.max(code.len().next_multiple_of(PAGE_SIZE))));
            }
            self.chunks.last_mut().expect("there is a chunk").add(code)
        }

        fn key(&mut self, module: &Arc<Module>, function: u32, osr: Option<u32>) -> CompileKey {
            let address = Arc::as_ptr(module) as usize;
            self.modules.entry(address).or_insert_with(|| module.clone());
            CompileKey {
                module: address,
                function,
                osr,
            }
        }

        fn request(&self, key: CompileKey) -> CompileRequest {
            CompileRequest {
                module: self.modules[&key.module].clone(),
                function: key.function,
                runtime: stubs().runtime,
                options: super::options().compile_options(),
                osr: key.osr,
            }
        }

        /// Installs the result of a compile.
        fn finish(&mut self, key: CompileKey, compiled: Result<CompiledFunction, libwasm_jit::CompileFailure>) {
            let Ok(CompiledFunction {
                code, exception_sites, ..
            }) = compiled
            else {
                self.failed.insert(key);
                return;
            };
            let entry = self.add_code(&code);
            self.exception_sites
                .extend(exception_sites.iter().map(|offset| entry + u64::from(*offset)));
            if super::options().perf_map {
                let name = match key.osr {
                    None => format!("wasm-function-{}", key.function),
                    Some(offset) => format!("wasm-function-{}-osr-{offset}", key.function),
                };
                super::write_perf_map_entry(entry, code.len(), &name);
            }
            self.code.insert(key, entry);
        }

        /// Where the compile of `key` is, after compiling it here and now in sync mode, or else having it compiled
        /// in the background.
        fn compile(&mut self, key: CompileKey) -> CompileState {
            if self.code.contains_key(&key) {
                return CompileState::Compiled;
            }
            if self.failed.contains(&key) {
                return CompileState::Failed;
            }
            if super::options().sync {
                let request = self.request(key);
                let code = libwasm_jit::compile(&mut self.context, &request);
                self.finish(key, code);
                return self.compile(key);
            }
            if self.pending.insert(key) {
                self.submit(key);
            }
            CompileState::Pending
        }

        fn submit(&mut self, key: CompileKey) {
            let request = self.request(key);
            let (reply, _) = self.finished.get_or_insert_with(channel);
            let job = Job {
                key,
                request,
                reply: reply.clone(),
            };
            compile_threads()
                .jobs
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner())
                .send(job)
                .expect("the compile threads run while the process does");
        }

        /// Installs what the compile threads finished; with `wait`, waits for every pending compile first.
        fn take_finished(&mut self, wait: bool) {
            let Some((_, receiver)) = &self.finished else {
                return;
            };
            let mut finished = Vec::new();
            if wait {
                for _ in 0..self.pending.len() {
                    finished.push(receiver.recv().expect("the store holds a sender"));
                }
            } else {
                finished.extend(receiver.try_iter());
            }
            for Finished { key, code } in finished {
                self.pending.remove(&key);
                self.finish(key, code);
            }
        }
    }

    /// Unwinds `exception` from the compiled frame whose fp is `frame`: finds the first exception site its callers
    /// return to, or else the entry trampoline, and has the code continue there (see `ExecutionContext::resume`),
    /// with the exception in the context.
    pub unsafe fn unwind(store: &mut Store, exception: u64, frame: u64) {
        unsafe {
            let read = |address: u64| *(address as *const u64);
            let entry_return = stubs().entry_return;
            let sites = &store.jit.as_ref().expect("compiled code ran").exception_sites;
            // The return address of the callee whose fp `callee` is leads into its caller's frame.
            let mut callee = frame;
            loop {
                let return_address = read(callee.wrapping_add_signed(crate::abi::FRAME_RETURN_IP as i64));
                if return_address == entry_return || sites.contains(&return_address) {
                    let context = &mut *store.context;
                    context.exception = exception;
                    context.resume = [
                        callee,
                        read(callee.wrapping_add_signed(crate::abi::FRAME_SAVED_FP as i64)),
                        return_address,
                    ];
                    return;
                }
                callee = read(callee.wrapping_add_signed(crate::abi::FRAME_SAVED_FP as i64));
            }
        }
    }

    pub fn install_finished(jit: &mut StoreJit) {
        if !jit.pending.is_empty() {
            jit.take_finished(false);
        }
    }

    /// Compiles every function of an instance, on the compile threads unless in sync mode, and installs the code.
    pub fn compile_instance(store: &mut Store, instance: InstanceAddress) {
        let functions = store.instance(instance).functions.clone();
        let jit = store.jit.get_or_insert_with(Default::default);
        if jit.disabled {
            return;
        }
        for address in &functions {
            let function = unsafe { address.get() };
            if let FunctionKind::Wasm { module, index, .. } = &function.kind {
                let key = jit.key(module, *index, None);
                jit.compile(key);
            }
        }
        jit.take_finished(true);
        for address in functions {
            compile_function(store, address);
        }
    }

    /// Compiles a WebAssembly function, unless the store compiled its module's function already, and installs the
    /// code once there is some.
    pub fn compile_function(store: &mut Store, address: FunctionAddress) -> CompileState {
        let jit = store.jit.get_or_insert_with(Default::default);
        if jit.disabled {
            return CompileState::Failed;
        }
        install_finished(jit);
        let function = unsafe { address.get_mut() };
        let FunctionKind::Wasm { module, index, .. } = &function.kind else {
            return CompileState::Failed;
        };
        let key = jit.key(module, *index, None);
        let state = jit.compile(key);
        if state == CompileState::Compiled {
            function.header.native_entry = jit.code[&key];
            function.header.kind = FUNCTION_KIND_COMPILED;
        }
        state
    }

    pub unsafe fn call_compiled(context: *mut ExecutionContext) -> Result<u64, u64> {
        unsafe {
            let [header, top, ..] = (*context).arguments;
            let function = &*(header as *const FunctionInstance);
            let params = ir_types(&function.ty.params);
            let parameter_slots = function.header.param_slots as usize;
            // The operand stack grows down, so the first argument is in the highest slots, and the results replace
            // the arguments.
            let stack = top as *mut u64;
            let arguments = |index: usize| {
                let home = convention::home_slot(&params, index) as usize;
                stack
                    .add(parameter_slots - home - params[index].slots() as usize)
                    .cast_const()
            };
            run(
                function,
                function.header.native_entry,
                &params,
                arguments,
                stack.add(parameter_slots),
            )
        }
    }

    /// Calls `entry`, compiled code of `function` that takes arguments of types `params`, the `index`th of which
    /// is at `argument(index)`. Stores the function's results on an operand stack ending at `end`, and returns its
    /// new top, or the exception it threw. A trap leaves through the code and does not return.
    unsafe fn run(
        function: &FunctionInstance,
        entry: u64,
        params: &[libwasm_jit::ir::Type],
        argument: impl Fn(usize) -> *const u64,
        end: *mut u64,
    ) -> Result<u64, u64> {
        unsafe {
            let header_data: &FunctionHeader = &function.header;
            let results = ir_types(&function.ty.results);
            let result_slots = header_data.result_slots as usize;
            let mut call = NativeCall {
                entry,
                header: header_data as *const FunctionHeader as u64,
                instance: header_data.instance as u64,
                memory_base: (*header_data.instance).memory0_base as u64,
                result_pointer_slot: u64::MAX,
                ..NativeCall::default()
            };
            let parameter_slots = convention::slots(params) as usize;
            let image_slots = (convention::outgoing_size(params, &results) / 8) as usize;
            // NB: On the stack where it fits, since a trap leaves through compiled code and skips this frame, which
            //     leaks what it owns.
            const SMALL_IMAGE: usize = 64;
            let mut small_image = [0u64; SMALL_IMAGE];
            let mut large_image = Vec::new();
            let image = if image_slots <= SMALL_IMAGE {
                small_image.as_mut_ptr()
            } else {
                large_image.resize(image_slots, 0);
                large_image.as_mut_ptr()
            };
            let mut places = Vec::new();
            convention::parameter_places(params, &mut places);
            let (mut gprs, mut xmms) = (0, 0);
            for (index, place) in places.iter().enumerate() {
                let home = convention::home_slot(params, index) as usize;
                let count = params[index].slots() as usize;
                let source = argument(index);
                std::ptr::copy_nonoverlapping(source, image.add(home), count);
                if let Place::Register(_) = place {
                    if params[index].uses_xmm() {
                        call.xmms[xmms] = [*source, if count == 2 { *source.add(1) } else { 0 }];
                        xmms += 1;
                    } else {
                        call.gprs[gprs] = *source;
                        gprs += 1;
                    }
                }
            }
            if convention::has_result_area(&results) {
                call.result_pointer_slot = parameter_slots as u64;
            }
            call.image = image as u64;
            call.image_slots = image_slots as u64;
            let trampoline: extern "C" fn(*mut NativeCall) = std::mem::transmute(stubs().entry as usize);
            trampoline(&raw mut call);
            let context = (*header_data.instance).context;
            if (*context).exception != 0 {
                return Err(std::mem::take(&mut (*context).exception));
            }
            let return_top = end.sub(result_slots);
            convention::result_places(&results, &mut places);
            let (mut gprs, mut xmms) = (0, 0);
            for (index, place) in places.iter().enumerate() {
                let home = convention::home_slot(&results, index) as usize;
                let count = results[index].slots() as usize;
                let mut words = [0u64; 2];
                match *place {
                    Place::Register(_) if results[index].uses_xmm() => {
                        words = call.result_xmms[xmms];
                        xmms += 1;
                    }
                    Place::Register(_) => {
                        words[0] = call.result_gprs[gprs];
                        gprs += 1;
                    }
                    Place::Memory(slot) => {
                        let area = image.add(parameter_slots + 1 + slot as usize);
                        words[..count].copy_from_slice(ak::slice_from_raw_parts(area, count));
                    }
                }
                // NB: Through a value, so that i32 and f32 results take their slot's low half only.
                let value = value_from_words(function.ty.results[index], &words);
                words_of_value(&value, &mut words);
                // The first result is in the highest slots.
                let destination = return_top.add(result_slots - home - count);
                std::ptr::copy_nonoverlapping(words.as_ptr(), destination, count);
            }
            Ok(return_top as u64)
        }
    }

    /// Continues the interpreter frame at `frame`, of `address`, compiled at the loop at `pc` (OSR), once there is
    /// code for that. It cannot be entered at a loop in another loop, or if the function does not compile.
    pub unsafe fn enter_at_loop(store: &mut Store, address: FunctionAddress, pc: u64, frame: u64) -> LoopEntry {
        unsafe {
            let function = address.get();
            let FunctionKind::Wasm { module, index, .. } = &function.kind else {
                return LoopEntry::Never;
            };
            let offset = (pc - module.bytes.as_ptr() as u64) as u32;
            let jit = store.jit.get_or_insert_with(Default::default);
            if jit.disabled {
                return LoopEntry::Never;
            }
            let key = jit.key(module, *index, Some(offset));
            match jit.compile(key) {
                CompileState::Compiled => {}
                CompileState::Pending => return LoopEntry::Pending,
                CompileState::Failed => return LoopEntry::Never,
            }
            let entry = jit.code[&key];
            let params = [libwasm_jit::ir::Type::Pointer];
            let base = (frame as *mut u8).sub(crate::abi::FRAME_HEADER_SIZE).cast::<u64>();
            match run(function, entry, &params, |_| &raw const frame, base) {
                Ok(top) => LoopEntry::Ran(top),
                Err(exception) => LoopEntry::Threw(exception),
            }
        }
    }
}

#[cfg(not(all(unix, target_arch = "x86_64")))]
mod native {
    use crate::abi::ExecutionContext;
    use crate::runtime::{InstanceAddress, Store};

    pub struct Stubs {
        pub trap_exit: u64,
        pub adapter: u64,
    }

    pub static STUBS: std::sync::OnceLock<Stubs> = std::sync::OnceLock::new();

    pub fn stubs() -> &'static Stubs {
        unreachable!("the JIT is off where it has no code generator")
    }

    #[derive(Default)]
    pub struct StoreJit {
        pub disabled: bool,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum CompileState {
        Compiled,
        Pending,
        Failed,
    }

    pub enum LoopEntry {
        Ran(u64),
        Threw(u64),
        Pending,
        Never,
    }

    pub unsafe fn unwind(_store: &mut Store, _exception: u64, _frame: u64) {
        unreachable!("the JIT is off where it has no code generator")
    }

    pub fn compile_instance(_store: &mut Store, _instance: InstanceAddress) {}

    pub fn compile_function(_store: &mut Store, _address: crate::runtime::FunctionAddress) -> CompileState {
        CompileState::Failed
    }

    pub unsafe fn enter_at_loop(
        _store: &mut Store,
        _address: crate::runtime::FunctionAddress,
        _pc: u64,
        _frame: u64,
    ) -> LoopEntry {
        LoopEntry::Never
    }

    pub unsafe fn call_compiled(_context: *mut ExecutionContext) -> Result<u64, u64> {
        unreachable!("the JIT is off where it has no code generator")
    }
}

pub use native::StoreJit;
