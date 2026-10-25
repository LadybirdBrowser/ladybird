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
//! `LIBWASM_JIT` configures it, as a comma-separated list: `off` (run everything interpreted), `on` (tier up hot
//! functions, the default), `threshold=N` (tier up once calls and loop iterations cost N, 1000 by default), `forced`
//! (tier up at the first call or loop), `eager` (compile every function when its module is instantiated),
//! `perf-map` (write `/tmp/perf-PID.map` for `perf`), `verify-ir` (check every compile's IR and register allocation),
//! `stress-osr` (tier up at loops only, where they start, so that as many loops as can continue compiled),
//! `stress-nested-osr` (the same, at loops in other loops only), `sync` (compile on the thread that runs the code, as
//! `forced` and `stress-osr` do too), and `threads=N` (the number of compile threads, one per processor core but one by
//! default).

use super::ranges::RangeSet;
use super::{FunctionAddress, FunctionInstance, FunctionKind, InstanceAddress, Store, Trap};
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
    /// Like `stress_osr`, but only at loops in other loops, which the frame enters through the loops around them.
    pub stress_nested_osr: bool,
    /// Compile on the thread that runs the code, and run the code as soon as it is compiled.
    pub sync: bool,
    /// The number of compile threads, or 0 for one per processor core but one.
    pub threads: u32,
}

#[cfg(all(unix, any(target_arch = "x86_64", target_arch = "aarch64")))]
impl Options {
    fn compile_options(self) -> libwasm_jit::Options {
        libwasm_jit::Options {
            verify_ir: self.verify_ir,
            stress_nested_osr: self.stress_nested_osr,
            ..libwasm_jit::Options::default()
        }
    }
}

pub fn options() -> Options {
    static OPTIONS: OnceLock<Options> = OnceLock::new();
    *OPTIONS.get_or_init(|| {
        let mut options = Options {
            enabled: true,
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
                    "stress-osr" | "stress-nested-osr" => {
                        options.enabled = true;
                        options.sync = true;
                        options.stress_osr = true;
                        options.stress_nested_osr = option.trim() == "stress-nested-osr";
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
        // NB: Only where compiled code can trap on memory faults (see `redirect_fault()`) and have executable memory.
        options.enabled &= cfg!(any(
            all(any(target_os = "linux", target_os = "macos"), target_arch = "x86_64"),
            all(any(target_os = "linux", target_os = "macos"), target_arch = "aarch64")
        ));
        options.enabled &= super::interpreter::cpu_supports_jit();
        options
    })
}

export_runtime_functions! {
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
/// 1 if a handler of the interpreter caught it and 2 if it left the interpreter, or 3 if the frame tail called a
/// function that does not run compiled, which the interpreter calls in its place at the CallPending pseudo-instruction
/// `arguments[4]` points at (see `call_compiled()`); returns 0 otherwise.
unsafe fn tier_up(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let [header, pc, reason, frame, ..] = (*context).arguments;
        let address = FunctionAddress::from_ptr(header as *const FunctionInstance);
        let store = store_of(context);
        if !enabled() {
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
        let state = native::compile_function(store, address, native::Compile::InBackground);
        address.get_mut().header.tier_up_budget = match state {
            // A compiled function keeps a budget, so that frames of it still running in the interpreter continue
            // compiled at the next loop header they reach.
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
            native::LoopEntry::TailCalled => {
                let read = |offset: isize| *((frame as *const u8).offset(offset) as *const u64);
                native::pass_tail_call(store, context);
                let arguments = &mut (*context).arguments;
                arguments[2] = read(crate::abi::FRAME_RETURN_VSP);
                arguments[3] = read(crate::abi::FRAME_RETURN_IP);
                arguments[4] = super::interpreter::call_pending_code();
                3
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
    if enabled() && !options.eager {
        options.threshold
    } else {
        u32::MAX
    }
}

/// What `native_entry` starts as in a new function's header.
pub fn initial_native_entry() -> u64 {
    if !enabled() {
        return 0;
    }
    native::stubs().adapter
}

/// Whether the JIT runs: if `LIBWASM_JIT` wants it, the CPU has the features its code uses, and the process has
/// executable memory for its stubs, which a hardened runtime without the JIT entitlement does not give it.
pub(crate) fn enabled() -> bool {
    options().enabled && native::try_stubs().is_some()
}

impl Store {
    /// Keeps everything in this store interpreted, whatever `LIBWASM_JIT` says, as tests comparing the tiers want.
    pub fn disable_jit(&mut self) {
        self.jit.get_or_insert_with(Default::default).disabled = true;
    }
}

/// Called once a module is instantiated, before its start function runs.
pub fn instantiated(store: &mut Store, instance: InstanceAddress) {
    if let Some(jit) = store.jit.as_mut() {
        native::forget_dead_modules(jit);
    }
    if options().eager && enabled() {
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
        let Some(stubs) = native::STUBS.get().and_then(Option::as_ref) else {
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

/// Redirects a memory fault in compiled code to the trap exit. `context` is the signal handler's.
///
/// # Safety
///
/// Only the fault handler calls this, with its context.
#[cfg(all(target_os = "linux", target_arch = "aarch64"))]
pub unsafe fn redirect_fault(context: *mut libc::c_void) -> bool {
    unsafe {
        let context = context.cast::<libc::ucontext_t>();
        let mcontext = &mut (*context).uc_mcontext;
        let Some(stubs) = native::STUBS.get().and_then(Option::as_ref) else {
            return false;
        };
        if !CODE_RANGES.contains(mcontext.pc as usize) {
            return false;
        }
        mcontext.pc = stubs.trap_exit;
        mcontext.regs[0] = u64::from(crate::abi::TRAP_MEMORY_OUT_OF_BOUNDS);
        true
    }
}

/// Redirects a memory fault in compiled code to the trap exit. `context` is the signal handler's.
///
/// # Safety
///
/// Only the fault handler calls this, with its context.
#[cfg(all(target_os = "macos", target_arch = "aarch64"))]
pub unsafe fn redirect_fault(context: *mut libc::c_void) -> bool {
    unsafe {
        let context = context.cast::<libc::ucontext_t>();
        let state = &mut (*(*context).uc_mcontext).__ss;
        let Some(stubs) = native::STUBS.get().and_then(Option::as_ref) else {
            return false;
        };
        if !CODE_RANGES.contains(state.__pc as usize) {
            return false;
        }
        state.__pc = stubs.trap_exit;
        state.__x[0] = u64::from(crate::abi::TRAP_MEMORY_OUT_OF_BOUNDS);
        true
    }
}

/// Redirects a memory fault in compiled code to the trap exit. `context` is the signal handler's.
///
/// # Safety
///
/// Only the fault handler calls this, with its context.
#[cfg(all(target_os = "macos", target_arch = "x86_64"))]
pub unsafe fn redirect_fault(context: *mut libc::c_void) -> bool {
    unsafe {
        let context = context.cast::<libc::ucontext_t>();
        let state = &mut (*(*context).uc_mcontext).__ss;
        let Some(stubs) = native::STUBS.get().and_then(Option::as_ref) else {
            return false;
        };
        if !CODE_RANGES.contains(state.__rip as usize) {
            return false;
        }
        state.__rip = stubs.trap_exit;
        state.__rdi = u64::from(crate::abi::TRAP_MEMORY_OUT_OF_BOUNDS);
        true
    }
}

#[cfg(not(any(
    all(any(target_os = "linux", target_os = "macos"), target_arch = "x86_64"),
    all(any(target_os = "linux", target_os = "macos"), target_arch = "aarch64")
)))]
pub unsafe fn redirect_fault(_context: *mut libc::c_void) -> bool {
    false
}

/// Appends a line for code at `address` to the process's perf map.
#[cfg(all(unix, any(target_arch = "x86_64", target_arch = "aarch64")))]
fn write_perf_map_entry(address: u64, size: usize, name: &str) {
    let _ = ak::perf_map::write_perf_map_entry(address as usize, size, name);
}

fn ir_type(ty: ValType) -> libwasm_jit::ir::Type {
    libwasm_jit::builder::ir_type(ty).expect("the JIT has a type for every value")
}

/// Where compiled code passes the parameters and results of a function, worked out once per function (in
/// `FunctionInstance::native_shapes`), so that calls between the tiers allocate nothing, and own nothing that a trap,
/// which leaves through the compiled code and skips their frames, would leak.
pub struct NativeShapes {
    /// Of calls of the function.
    pub call: CallShape,
    /// Of an OSR compile of it, which takes the fp of an interpreter frame.
    pub osr: CallShape,
}

/// Where compiled code passes values of a signature (see `libwasm_jit::convention`).
pub struct CallShape {
    params: Box<[ValueShape]>,
    results: Box<[ValueShape]>,
    /// The slots of the parameters' home area, after which the result area pointer is, and where the result area
    /// starts.
    parameter_slots: usize,
    #[cfg_attr(
        not(all(unix, any(target_arch = "x86_64", target_arch = "aarch64"))),
        allow(dead_code)
    )]
    result_area_slot: usize,
    /// The slots of the outgoing area: the home slots, and the result area pointer and the result area if there is
    /// one.
    #[cfg_attr(
        not(all(unix, any(target_arch = "x86_64", target_arch = "aarch64"))),
        allow(dead_code)
    )]
    image_slots: usize,
    has_result_area: bool,
}

/// Where compiled code passes one value: its type, its place, the slot of its home (or of its place among the
/// results), the slots it takes, and for a register, its position among the registers of its class.
#[derive(Clone, Copy)]
struct ValueShape {
    ty: libwasm_jit::ir::Type,
    place: libwasm_jit::convention::Place,
    home: usize,
    slots: usize,
    register: usize,
}

impl CallShape {
    fn new(params: &[libwasm_jit::ir::Type], results: &[libwasm_jit::ir::Type]) -> Self {
        use libwasm_jit::convention;
        let shapes = |types: &[libwasm_jit::ir::Type], places: &[convention::Place]| -> Box<[ValueShape]> {
            let (mut gprs, mut xmms) = (0, 0);
            types
                .iter()
                .zip(places)
                .enumerate()
                .map(|(index, (&ty, &place))| {
                    let counter = if ty.uses_xmm() { &mut xmms } else { &mut gprs };
                    let register = *counter;
                    if let convention::Place::Register(_) = place {
                        *counter += 1;
                    }
                    ValueShape {
                        ty,
                        place,
                        home: convention::home_slot(types, index) as usize,
                        slots: ty.slots() as usize,
                        register,
                    }
                })
                .collect()
        };
        let mut places = Vec::new();
        convention::parameter_places(params, &mut places);
        let param_shapes = shapes(params, &places);
        convention::result_places(results, &mut places);
        let result_shapes = shapes(results, &places);
        Self {
            params: param_shapes,
            results: result_shapes,
            parameter_slots: convention::slots(params) as usize,
            result_area_slot: convention::result_area_slot(params) as usize,
            image_slots: (convention::outgoing_size(params, results) / 8) as usize,
            has_result_area: convention::has_result_area(results),
        }
    }
}

/// The shapes of `function`'s calls, made the first time they are needed.
fn shapes(function: &FunctionInstance) -> &NativeShapes {
    function.native_shapes.get_or_init(|| {
        let params: Vec<_> = function.ty.params.iter().map(|ty| ir_type(*ty)).collect();
        let results: Vec<_> = function.ty.results.iter().map(|ty| ir_type(*ty)).collect();
        Box::new(NativeShapes {
            call: CallShape::new(&params, &results),
            osr: CallShape::new(&[libwasm_jit::ir::Type::Pointer], &results),
        })
    })
}

/// The bits of a slot of a value of type `ty` that compiled code holds in a 64-bit register: i32 and f32 values
/// zero-extended.
fn slot_bits(ty: libwasm_jit::ir::Type, word: u64) -> u64 {
    if matches!(ty, libwasm_jit::ir::Type::I32 | libwasm_jit::ir::Type::F32) {
        word & 0xffff_ffff
    } else {
        word
    }
}

/// How many of the thread's large call images are in use, which the runtime notes when it enters the interpreter and
/// restores when the entry returns, so that the images of calls a trap skipped are free again.
pub fn large_images_in_use() -> usize {
    native::large_images_in_use()
}

pub fn release_large_images(in_use: usize) {
    native::release_large_images(in_use);
}

/// Calls a compiled function from the interpreter: `arguments[0]` is its header and `arguments[1]` the top of the
/// operand stack, where its arguments are, `arguments[2]` where the call returns to and `arguments[3]` the caller's
/// frame. Replaces the arguments with its results and returns the new top, or if it threw an exception, 1 if a
/// handler of the interpreter caught it, or 0 if it left the interpreter (see `rethrow_in_interpreter`). Returns 2 if
/// it tail called a function that does not run compiled, which the interpreter calls in its place at the CallPending
/// pseudo-instruction `arguments[4]` points at, with where the results go in `arguments[2]` and where the call returns
/// to in `arguments[3]`.
unsafe fn call_compiled(context: *mut ExecutionContext) -> u64 {
    unsafe {
        // NB: The compiled code's calls into the runtime and the helpers it calls reuse the context's arguments.
        let [header, top, return_ip, frame, ..] = (*context).arguments;
        match native::call_compiled(context) {
            native::Ran::Returned(top) => top,
            native::Ran::Threw(exception) => {
                rethrow_in_interpreter(context, store_of(context), exception, frame, return_ip - 1)
            }
            native::Ran::TailCalled => {
                let parameter_slots = (*(header as *const FunctionInstance)).header.param_slots;
                native::pass_tail_call(store_of(context), context);
                let arguments = &mut (*context).arguments;
                arguments[2] = top + 8 * u64::from(parameter_slots);
                arguments[3] = return_ip;
                arguments[4] = super::interpreter::call_pending_code();
                2
            }
        }
    }
}

/// Calls a function that runs in the runtime from compiled code, through the runtime adapter: `arguments[0]` is its
/// header, `arguments[1]` the adapter's save area, and `arguments[2]` the adapter's frame, above which the home
/// slots of the arguments are. Returns 0 with the results in place, or a trap code, with the trap in the store.
unsafe fn call_from_native(context: *mut ExecutionContext) -> u64 {
    use libwasm_jit::convention::Place;
    unsafe {
        let [header, save_area, frame, ..] = (*context).arguments;
        let function = &*(header as *const FunctionInstance);
        let shape = &shapes(function).call;
        let save_area = save_area as *mut u64;
        let frame = frame as *mut u64;
        // Where a value is in the adapter's save area, or in the home slots above its frame.
        let location = |value: &ValueShape| match value.place {
            Place::Register(_) if value.ty.uses_xmm() => {
                save_area.add(libwasm_jit::convention::GPR_ARGUMENTS.len() + 2 * value.register)
            }
            Place::Register(_) => save_area.add(value.register),
            Place::Memory(slot) => frame.add(slot as usize),
        };
        let store = store_of(context);
        let address = FunctionAddress::from_ptr(function);
        let parameter_slots = function.header.param_slots as usize;
        let result_slots = function.header.result_slots as usize;
        // NB: On the stack where they fit; this frame is never skipped, since traps come back as results here.
        const SMALL: usize = 32;
        let (mut small_arguments, mut small_results) = ([0u64; SMALL], [0u64; SMALL]);
        let (mut large_arguments, mut large_results) = (Vec::new(), Vec::new());
        let arguments: &mut [u64] = if parameter_slots <= SMALL {
            &mut small_arguments[..parameter_slots]
        } else {
            large_arguments.resize(parameter_slots, 0);
            &mut large_arguments
        };
        let results: &mut [u64] = if result_slots <= SMALL {
            &mut small_results[..result_slots]
        } else {
            large_results.resize(result_slots, 0);
            &mut large_results
        };
        // The operand stack grows down, so the first value is in the highest slots.
        for param in &shape.params {
            let source = location(param);
            let start = parameter_slots - param.home - param.slots;
            for slot in 0..param.slots {
                arguments[start + slot] = slot_bits(param.ty, *source.add(slot));
            }
        }
        let outcome = if let FunctionKind::Host(_) = function.kind {
            store.call_host_slots(address, arguments.as_ptr(), results.as_mut_ptr())
        } else {
            // NB: A tail call from compiled code that the interpreter called returns to the interpreter, which makes
            //     it in that code's place, so that functions that tail call each other across the tiers do not keep
            //     a native frame per call.
            if native::defer_tail_call(store, address, frame as u64, arguments) {
                return 0;
            }
            store.invoke_slots(address, arguments, results)
        };
        match outcome {
            Ok(()) => {}
            Err(Trap::UncaughtException(exception)) => {
                // NB: The adapter's frame is a frame of the compiled caller's callee.
                native::unwind(store, exception, frame as u64);
                return libwasm_jit::stubs::ADAPTER_RESUME;
            }
            Err(trap) => {
                let code = trap.code();
                store.pending_trap = Some(trap);
                return u64::from(code);
            }
        }
        let area = if shape.has_result_area {
            *frame.add(shape.parameter_slots) as *mut u64
        } else {
            std::ptr::null_mut()
        };
        for result in &shape.results {
            let destination = match result.place {
                Place::Register(_) if result.ty.uses_xmm() => {
                    save_area.add(libwasm_jit::convention::GPR_ARGUMENTS.len() + 2 * result.register)
                }
                Place::Register(_) => save_area.add(result.register),
                Place::Memory(slot) => area.add(slot as usize),
            };
            let start = result_slots - result.home - result.slots;
            for slot in 0..result.slots {
                *destination.add(slot) = slot_bits(result.ty, results[start + slot]);
            }
        }
        0
    }
}

#[cfg(all(unix, any(target_arch = "x86_64", target_arch = "aarch64")))]
mod native {
    use super::{CODE_RANGES, CallShape, shapes, slot_bits};
    use crate::abi::{ExecutionContext, FUNCTION_KIND_COMPILED, FunctionHeader, OPCODE_TRAP_FROM_NATIVE};
    use crate::module::Module;
    use crate::runtime::{FunctionAddress, FunctionInstance, FunctionKind, InstanceAddress, Store, helpers};
    use libwasm_jit::convention::Place;
    use libwasm_jit::stubs::NativeCall;
    use libwasm_jit::{CompileContext, CompileRequest, CompiledFunction, RuntimeInfo};
    use std::collections::{HashMap, HashSet};
    use std::sync::mpsc::{Receiver, Sender, channel};
    use std::sync::{Arc, Mutex, OnceLock, Weak};

    /// The code the process's stores share.
    pub struct Stubs {
        pub trap_exit: u64,
        pub entry: u64,
        /// Where the entry trampoline's call of compiled code returns to.
        pub entry_return: u64,
        pub adapter: u64,
        pub runtime: RuntimeInfo,
    }

    /// The stubs, or None if the process has no executable memory for them.
    pub static STUBS: OnceLock<Option<Stubs>> = OnceLock::new();

    /// The stubs, once the JIT is known to run (see `super::enabled()`).
    pub fn stubs() -> &'static Stubs {
        try_stubs().expect("compiled code runs only with its stubs")
    }

    pub fn try_stubs() -> Option<&'static Stubs> {
        STUBS.get_or_init(make_stubs).as_ref()
    }

    fn make_stubs() -> Option<Stubs> {
        let handler = crate::runtime::interpreter::dispatch_table_entry(OPCODE_TRAP_FROM_NATIVE) as u64;
        // NB: The trap exit goes first, so that the others can jump to it. The chunk is never freed.
        let mut chunk = CodeChunk::new(64 * 1024)?;
        let trap_exit = chunk.add(&libwasm_jit::stubs::trap_exit(handler))?;
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
        let entry = chunk.add(&trampoline)?;
        let adapter_code =
            libwasm_jit::stubs::runtime_adapter(super::wasm_runtime_call_from_native as *const () as u64, &runtime);
        let adapter = chunk.add(&adapter_code)?;
        if super::options().perf_map {
            super::write_perf_map_entry(entry, trampoline.len(), "wasm-entry-trampoline");
            super::write_perf_map_entry(adapter, adapter_code.len(), "wasm-runtime-adapter");
        }
        std::mem::forget(chunk);
        Some(Stubs {
            trap_exit,
            entry,
            entry_return: entry + return_offset as u64,
            adapter,
            runtime,
        })
    }

    /// Executable memory, written while it is not executable (on macOS, while the writing thread's view of it is not).
    /// Failing to map it, or to make it writable, fails the compile, which leaves the function interpreted.
    struct CodeChunk {
        base: *mut u8,
        size: usize,
        used: usize,
    }

    const CHUNK_SIZE: usize = 16 << 20;

    /// How many compiles a store installs between looking for modules that died.
    const SWEEP_INTERVAL: usize = 64;

    fn page_size() -> usize {
        static PAGE_SIZE: OnceLock<usize> = OnceLock::new();
        *PAGE_SIZE.get_or_init(|| unsafe { libc::sysconf(libc::_SC_PAGESIZE) } as usize)
    }

    /// The longest an eager instantiation waits for its compiles.
    const EAGER_WAIT: std::time::Duration = std::time::Duration::from_secs(60);

    impl CodeChunk {
        fn new(size: usize) -> Option<Self> {
            // NB: macOS on Apple silicon maps code writable and executable at once, with MAP_JIT, and switches each
            //     thread's view of it between the two with pthread_jit_write_protect_np().
            #[cfg(target_os = "macos")]
            let (protection, flags) = (
                libc::PROT_READ | libc::PROT_WRITE | libc::PROT_EXEC,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_JIT,
            );
            #[cfg(not(target_os = "macos"))]
            let (protection, flags) = (
                libc::PROT_READ | libc::PROT_EXEC,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_NORESERVE,
            );
            let base = unsafe { libc::mmap(std::ptr::null_mut(), size, protection, flags, -1, 0) };
            if base == libc::MAP_FAILED {
                return None;
            }
            // NB: The fault handler has to know the code, or faults in it would crash the process.
            if !CODE_RANGES.add(base as usize, size) {
                unsafe { libc::munmap(base, size) };
                return None;
            }
            Some(Self {
                base: base.cast(),
                size,
                used: 0,
            })
        }

        fn has_room(&self, length: usize) -> bool {
            self.used.next_multiple_of(16) + length <= self.size
        }

        /// Copies `code` in and returns its address, or None if the pages could not be made writable, which leaves them
        /// as they were.
        // NB: On macOS, switching the thread's view of the code cannot fail.
        #[cfg_attr(target_os = "macos", allow(clippy::unnecessary_wraps))]
        fn add(&mut self, code: &[u8]) -> Option<u64> {
            let start = self.used.next_multiple_of(16);
            debug_assert!(start + code.len() <= self.size);
            #[cfg(target_os = "macos")]
            unsafe {
                libc::pthread_jit_write_protect_np(0);
                std::ptr::copy_nonoverlapping(code.as_ptr(), self.base.add(start), code.len());
                libc::pthread_jit_write_protect_np(1);
                libwasm_jit::runtime::flush_instruction_cache(self.base as usize + start, code.len());
            }
            #[cfg(not(target_os = "macos"))]
            unsafe {
                let page_size = page_size();
                let first_page = start / page_size * page_size;
                let length = (start + code.len()).next_multiple_of(page_size) - first_page;
                let region = self.base.add(first_page);
                if libc::mprotect(region.cast(), length, libc::PROT_READ | libc::PROT_WRITE) != 0 {
                    return None;
                }
                std::ptr::copy_nonoverlapping(code.as_ptr(), self.base.add(start), code.len());
                // NB: The pages may hold code that is installed already, which must not stop being executable. The
                //     change back can only fail for lack of kernel memory, when the process cannot go on anyway.
                if libc::mprotect(region.cast(), length, libc::PROT_READ | libc::PROT_EXEC) != 0 {
                    eprintln!(
                        "LibWasm JIT: making compiled code executable again failed: {}",
                        std::io::Error::last_os_error()
                    );
                    std::process::abort();
                }
                libwasm_jit::runtime::flush_instruction_cache(self.base as usize + start, code.len());
            }
            self.used = start + code.len();
            Some(self.base as u64 + start as u64)
        }
    }

    #[cfg(test)]
    mod tests {
        use super::{CODE_RANGES, CompileContext, StoreJit};
        use std::sync::Arc;

        #[test]
        fn a_failed_mapping_makes_no_chunk() {
            assert!(super::CodeChunk::new(1 << 60).is_none());
        }

        #[test]
        fn code_goes_away_with_its_module() {
            // (module (func))
            const MODULE: &[u8] = &[
                0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x03, 0x02, 0x01,
                0x00, 0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b,
            ];
            let module = Arc::new(crate::compile(MODULE).expect("the module is valid"));
            let mut jit = StoreJit::default();
            let key = jit.key(&module, 0, None);
            let request = jit.request(key).expect("the module lives");
            let code = super::compile(&mut CompileContext::default(), &request);
            drop(request);
            jit.finish(key, code);
            let entry = jit.entry(key).expect("the function compiled");
            assert!(CODE_RANGES.contains(entry as usize));
            jit.forget_dead_modules();
            assert!(jit.entry(key).is_some(), "the module still lives");
            drop(module);
            jit.forget_dead_modules();
            // NB: The chunks live only in the module's code, so this is what unmaps them. Asking CODE_RANGES whether the
            //     entry is gone would race with the tests running beside this one, which may map a chunk of their own
            //     at the address just freed.
            assert!(jit.modules.is_empty());
        }

        #[test]
        fn traps_inside_one_entry_free_their_large_images() {
            use crate::runtime::{ExternValue, FunctionAddress, Store, Value};
            use std::cell::Cell;
            use std::rc::Rc;
            if !crate::runtime::jit::enabled() {
                return;
            }
            let bytes = include_bytes!("../../tests/fixtures/nested_traps.wasm");
            let module = Arc::new(crate::compile(bytes).expect("the module is valid"));
            let mut store = Store::new();
            let wide: Rc<Cell<Option<FunctionAddress>>> = Rc::default();
            let wide_in_host = wide.clone();
            let host = store.allocate_host_function(
                crate::types::FuncType {
                    params: Vec::new(),
                    results: Vec::new(),
                },
                Rc::new(move |store: &mut Store, _: &[Value]| {
                    // NB: The interpreter calls the compiled function, so every call has a large image, which the trap
                    //     skips.
                    let wide = wide_in_host.get().expect("the instance exists");
                    let arguments = [Value::I64(0); 70];
                    for _ in 0..1_000_000 {
                        assert!(store.invoke(wide, &arguments).is_err());
                    }
                    Ok(Vec::new())
                }),
            );
            let instance = store
                .instantiate(module, &[ExternValue::Function(host)])
                .expect("the module instantiates");
            let export = |name| match store.instance(instance).export(name) {
                Some(ExternValue::Function(function)) => function,
                _ => panic!("no function {name}"),
            };
            let (outer, wide_function) = (export("outer"), export("wide"));
            wide.set(Some(wide_function));
            // The function whose frames the traps skip runs compiled, with a call image too large for the stack.
            while super::compile_function(&mut store, wide_function, super::Compile::InBackground)
                != super::CompileState::Compiled
            {
                std::thread::sleep(std::time::Duration::from_millis(1));
            }
            store.invoke(outer, &[]).expect("the host function returns");
            assert!(super::large_image_buffers() <= 2);
            assert_eq!(super::large_images_in_use(), 0);
        }

        #[test]
        fn tail_calls_across_tiers_keep_the_stack_bounded() {
            use crate::runtime::{ExternValue, Store, Value};
            if !crate::runtime::jit::enabled() {
                return;
            }
            let bytes = include_bytes!("../../tests/fixtures/tail_calls_across_tiers.wasm");
            let module = Arc::new(crate::compile(bytes).expect("the module is valid"));
            let mut store = Store::new();
            let instance = store.instantiate(module, &[]).expect("the module instantiates");
            let export = |name| match store.instance(instance).export(name) {
                Some(ExternValue::Function(function)) => function,
                _ => panic!("no function {name}"),
            };
            let (even, odd) = (export("even"), export("odd"));
            // NB: Even runs compiled, and odd runs interpreted for as long as it does not ask for its compile, as if
            //     its compile thread never finished it, or its compile failed.
            assert_eq!(
                super::compile_function(&mut store, even, super::Compile::Now),
                super::CompileState::Compiled
            );
            unsafe { odd.get_mut() }.header.tier_up_budget = u32::MAX;
            assert_eq!(store.invoke(even, &[Value::I64(1_000_000)]), Ok(vec![Value::I64(44)]));
            assert_ne!(unsafe { odd.get() }.header.kind, crate::abi::FUNCTION_KIND_COMPILED);
        }

        #[test]
        fn tail_calls_from_compiled_code_pass_more_arguments_to_the_interpreter() {
            use crate::runtime::{ExternValue, Store, Value};
            if !crate::runtime::jit::enabled() {
                return;
            }
            let bytes = include_bytes!("../../tests/fixtures/tail_calls_across_tiers.wasm");
            let module = Arc::new(crate::compile(bytes).expect("the module is valid"));
            let mut store = Store::new();
            let instance = store.instantiate(module, &[]).expect("the module instantiates");
            let export = |name| match store.instance(instance).export(name) {
                Some(ExternValue::Function(function)) => function,
                _ => panic!("no function {name}"),
            };
            let (few, many) = (export("few"), export("many"));
            assert_eq!(
                super::compile_function(&mut store, few, super::Compile::Now),
                super::CompileState::Compiled
            );
            unsafe { many.get_mut() }.header.tier_up_budget = u32::MAX;
            assert_eq!(store.invoke(few, &[Value::I64(5)]), Ok(vec![Value::I64(5039)]));
        }

        #[test]
        fn frames_continued_compiled_at_a_loop_tail_call_the_interpreter() {
            use crate::runtime::{ExternValue, FunctionKind, Store, Value};
            if !crate::runtime::jit::enabled() {
                return;
            }
            let bytes = include_bytes!("../../tests/fixtures/tail_calls_across_tiers.wasm");
            let module = Arc::new(crate::compile(bytes).expect("the module is valid"));
            let mut store = Store::new();
            let instance = store.instantiate(module, &[]).expect("the module instantiates");
            let export = |name| match store.instance(instance).export(name) {
                Some(ExternValue::Function(function)) => function,
                _ => panic!("no function {name}"),
            };
            let (loops, many) = (export("loops"), export("many"));
            unsafe { many.get_mut() }.header.tier_up_budget = u32::MAX;
            // NB: The loop is the function's first instruction, and its frame continues compiled there at once.
            let FunctionKind::Wasm { module, index, .. } = &unsafe { loops.get() }.kind else {
                unreachable!("the function is a WebAssembly function");
            };
            let body = &module.code[(*index - module.imported_function_count) as usize];
            let jit = store.jit.get_or_insert_with(Default::default);
            let key = jit.key(module, *index, Some(body.code.start));
            assert_eq!(jit.compile(key, super::Compile::Now), super::CompileState::Compiled);
            unsafe { loops.get_mut() }.header.tier_up_budget = crate::abi::TIER_UP_CALL_COST;
            assert_eq!(store.invoke(loops, &[Value::I64(3)]), Ok(vec![Value::I64(6039)]));
        }

        #[test]
        fn interpreted_handlers_catch_exceptions_of_compiled_callees_that_called_the_host() {
            use crate::runtime::{ExternValue, Store, Value};
            use std::rc::Rc;
            if !crate::runtime::jit::enabled() {
                return;
            }
            let bytes = include_bytes!("../../tests/fixtures/exceptions_across_tiers.wasm");
            let module = Arc::new(crate::compile(bytes).expect("the module is valid"));
            let mut store = Store::new();
            let host = store.allocate_host_function(
                crate::types::FuncType {
                    params: Vec::new(),
                    results: Vec::new(),
                },
                Rc::new(|_: &mut Store, _: &[Value]| Ok(Vec::new())),
            );
            let instance = store
                .instantiate(module, &[ExternValue::Function(host)])
                .expect("the module instantiates");
            let export = |name| match store.instance(instance).export(name) {
                Some(ExternValue::Function(function)) => function,
                _ => panic!("no function {name}"),
            };
            let (throws, catches) = (export("throws"), export("catches"));
            // NB: The handler runs interpreted, and the function that throws runs compiled and calls the host through
            //     the runtime adapter first, which uses the context's arguments too.
            assert_eq!(
                super::compile_function(&mut store, throws, super::Compile::Now),
                super::CompileState::Compiled
            );
            unsafe { catches.get_mut() }.header.tier_up_budget = u32::MAX;
            assert_eq!(store.invoke(catches, &[]), Ok(vec![Value::I32(42)]));
        }

        #[test]
        fn a_compile_finishing_after_its_module_died_maps_nothing() {
            // (module (func))
            const MODULE: &[u8] = &[
                0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x03, 0x02, 0x01,
                0x00, 0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b,
            ];
            let module = Arc::new(crate::compile(MODULE).expect("the module is valid"));
            let mut jit = StoreJit::default();
            let key = jit.key(&module, 0, None);
            let request = jit.request(key).expect("the module lives");
            let code = super::compile(&mut CompileContext::default(), &request);
            drop(request);
            drop(module);
            jit.finish(key, code);
            assert!(jit.entry(key).is_none());
            assert!(jit.modules.values().all(|module| module.chunks.is_empty()));
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
        code: Option<CompiledFunction>,
        /// The module the compile is of. It keeps the module's address from going to another module until the
        /// result is installed: a store that forgot the module meanwhile would install the code for the module at
        /// that address, which the key also names.
        _module: Weak<Module>,
    }

    /// Compiles `request`, or returns None if the compile fails.
    ///
    /// NB: A panic of the compiler is a bug, and aborts the process like any other: the crates of a process share the
    ///     panic hooks and runtime of whichever copy of the standard library the dynamic linker binds first, so
    ///     neither catching it nor keeping it out of the crash report can be relied on.
    fn compile(context: &mut CompileContext, request: &CompileRequest) -> Option<CompiledFunction> {
        libwasm_jit::compile(context, request).ok()
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
                            let code = compile(&mut context, &job.request);
                            // NB: The store may be gone, and its results with it.
                            let _ = job.reply.send(Finished {
                                key: job.key,
                                code,
                                _module: Arc::downgrade(&job.request.module),
                            });
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

    /// Where a compile runs.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum Compile {
        /// On the compile threads, unless the JIT compiles on the thread that runs the code (`Options::sync`).
        InBackground,
        /// On this thread, at once, even if a compile thread has the function.
        Now,
    }

    /// What became of entering compiled code at a loop.
    pub enum LoopEntry {
        /// The frame ran to its end compiled; this is the top of its operand stack, with the results on it.
        Ran(u64),
        /// The frame threw this exception, which its own handlers did not catch.
        Threw(u64),
        /// The frame tail called a function that does not run compiled (see `defer_tail_call()`).
        TailCalled,
        /// The code for the loop is still compiling.
        Pending,
        /// The frame cannot continue compiled at this loop.
        Never,
    }

    /// A store's compiled code, kept with the module it is of, for as long as the module lives: its functions and
    /// instances hold it, and code of a module only runs in frames of its functions.
    #[derive(Default)]
    pub struct StoreJit {
        context: CompileContext,
        /// What the store compiled of each module, by the module's address.
        modules: HashMap<usize, ModuleCode>,
        /// The return addresses of the calls in compiled code that handle exceptions.
        exception_sites: HashSet<u64>,
        /// The compiles the compile threads have yet to finish, and where they send them.
        pending: HashSet<CompileKey>,
        finished: Option<(Sender<Finished>, Receiver<Finished>)>,
        /// How many modules gained code since the store last looked for modules that died.
        new_code: usize,
        /// A tail call that compiled code the entry trampoline called made of a function that does not run compiled,
        /// with its argument slots in operand stack order, which the interpreter makes once the trampoline returns
        /// (see `defer_tail_call()`).
        tail_call: Option<FunctionAddress>,
        tail_call_arguments: Vec<u64>,
        pub disabled: bool,
    }

    /// The code a store compiled of one module. The reference to the module is weak, so that the module, and then
    /// its code, goes away with its last function or instance; until the store forgets it, the address is not
    /// reused for another module.
    struct ModuleCode {
        module: Weak<Module>,
        chunks: Vec<CodeChunk>,
        /// The code of each compile (by function and OSR loop), and the compiles that failed.
        code: HashMap<(u32, Option<u32>), u64>,
        failed: HashSet<(u32, Option<u32>)>,
        /// The exception sites in the code.
        exception_sites: Vec<u64>,
    }

    /// The first chunk of a module's code; each next one is twice as large, up to `CHUNK_SIZE`.
    const FIRST_CHUNK_SIZE: usize = 64 << 10;

    impl ModuleCode {
        /// Copies `code` into executable memory and returns its address, or None if there is none to be had.
        fn add_code(&mut self, code: &[u8]) -> Option<u64> {
            if !self.chunks.last().is_some_and(|chunk| chunk.has_room(code.len())) {
                let size = self
                    .chunks
                    .last()
                    .map_or(FIRST_CHUNK_SIZE, |chunk| (chunk.size * 2).min(CHUNK_SIZE));
                self.chunks
                    .push(CodeChunk::new(size.max(code.len().next_multiple_of(page_size())))?);
            }
            self.chunks.last_mut().expect("there is a chunk").add(code)
        }
    }

    impl StoreJit {
        fn key(&mut self, module: &Arc<Module>, function: u32, osr: Option<u32>) -> CompileKey {
            let address = Arc::as_ptr(module) as usize;
            self.modules.entry(address).or_insert_with(|| ModuleCode {
                module: Arc::downgrade(module),
                chunks: Vec::new(),
                code: HashMap::new(),
                failed: HashSet::new(),
                exception_sites: Vec::new(),
            });
            CompileKey {
                module: address,
                function,
                osr,
            }
        }

        fn request(&self, key: CompileKey) -> Option<CompileRequest> {
            Some(CompileRequest {
                module: self.modules[&key.module].module.upgrade()?,
                function: key.function,
                runtime: stubs().runtime,
                options: super::options().compile_options(),
                osr: key.osr,
            })
        }

        /// The code of a compile, if there is some.
        fn entry(&self, key: CompileKey) -> Option<u64> {
            self.modules
                .get(&key.module)?
                .code
                .get(&(key.function, key.osr))
                .copied()
        }

        /// Installs the result of a compile.
        fn finish(&mut self, key: CompileKey, compiled: Option<CompiledFunction>) {
            if self.new_code >= SWEEP_INTERVAL {
                self.forget_dead_modules();
            }
            // NB: A module that died while its function compiled gets no code.
            let Some(module) = self
                .modules
                .get_mut(&key.module)
                .filter(|module| module.module.strong_count() > 0)
            else {
                return;
            };
            // NB: A compile thread may finish a function that was compiled here and now while it worked on it.
            if module.code.contains_key(&(key.function, key.osr)) {
                return;
            }
            let entry = compiled.and_then(|compiled| Some((module.add_code(&compiled.code)?, compiled)));
            let Some((entry, compiled)) = entry else {
                module.failed.insert((key.function, key.osr));
                return;
            };
            self.new_code += 1;
            for offset in &compiled.exception_sites {
                let site = entry + u64::from(*offset);
                module.exception_sites.push(site);
                self.exception_sites.insert(site);
            }
            if super::options().perf_map {
                let name = match key.osr {
                    None => format!("wasm-function-{}", key.function),
                    Some(offset) => format!("wasm-function-{}-osr-{offset}", key.function),
                };
                super::write_perf_map_entry(entry, compiled.code.len(), &name);
            }
            module.code.insert((key.function, key.osr), entry);
        }

        /// Frees the code of the modules that died, which no function or instance runs any more.
        fn forget_dead_modules(&mut self) {
            self.new_code = 0;
            let exception_sites = &mut self.exception_sites;
            self.modules.retain(|_, module| {
                if module.module.strong_count() > 0 {
                    return true;
                }
                for site in &module.exception_sites {
                    exception_sites.remove(site);
                }
                false
            });
        }

        /// Where the compile of `key` is, after compiling it here and now in sync mode or `Compile::Now`, or else
        /// having it compiled in the background.
        fn compile(&mut self, key: CompileKey, when: Compile) -> CompileState {
            if self.entry(key).is_some() {
                return CompileState::Compiled;
            }
            if self.modules[&key.module].failed.contains(&(key.function, key.osr)) {
                return CompileState::Failed;
            }
            if super::options().sync || when == Compile::Now {
                let code = self
                    .request(key)
                    .and_then(|request| compile(&mut self.context, &request));
                self.finish(key, code);
                return self.compile(key, Compile::InBackground);
            }
            if self.pending.insert(key) {
                self.submit(key);
            }
            CompileState::Pending
        }

        fn submit(&mut self, key: CompileKey) {
            let request = self
                .request(key)
                .expect("a function of the module asks for the compile");
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
                // NB: Every job gets an answer, but if one never came, the store would wait forever; past the
                //     deadline, what is still pending is installed later, as in the background.
                let deadline = std::time::Instant::now() + EAGER_WAIT;
                for _ in 0..self.pending.len() {
                    let Ok(done) = receiver.recv_timeout(deadline.saturating_duration_since(std::time::Instant::now()))
                    else {
                        break;
                    };
                    finished.push(done);
                }
            } else {
                finished.extend(receiver.try_iter());
            }
            for Finished { key, code, .. } in finished {
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

    pub fn forget_dead_modules(jit: &mut StoreJit) {
        jit.forget_dead_modules();
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
                jit.compile(key, Compile::InBackground);
            }
        }
        jit.take_finished(true);
        for address in functions {
            if compile_function(store, address, Compile::InBackground) == CompileState::Pending {
                // NB: Eager functions have no budget, so one whose compile is late asks for it like a hot one.
                unsafe { address.get_mut() }.header.tier_up_budget = super::POLL_BUDGET + crate::abi::TIER_UP_CALL_COST;
            }
        }
    }

    /// Compiles a WebAssembly function, unless the store compiled its module's function already, and installs the
    /// code once there is some.
    pub fn compile_function(store: &mut Store, address: FunctionAddress, when: Compile) -> CompileState {
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
        let state = jit.compile(key, when);
        if state == CompileState::Compiled {
            function.header.native_entry = jit.entry(key).expect("the function is compiled");
            function.header.kind = FUNCTION_KIND_COMPILED;
        }
        state
    }

    pub unsafe fn call_compiled(context: *mut ExecutionContext) -> Ran {
        unsafe {
            let [header, top, ..] = (*context).arguments;
            let function = &*(header as *const FunctionInstance);
            let shape = &shapes(function).call;
            let parameter_slots = function.header.param_slots as usize;
            // The operand stack grows down, so the first argument is in the highest slots, and the results replace
            // the arguments.
            let stack = top as *mut u64;
            run(
                function,
                function.header.native_entry,
                shape,
                |_, param| stack.add(parameter_slots - param.home - param.slots).cast_const(),
                stack.add(parameter_slots),
            )
        }
    }

    /// The images of calls whose image does not fit on the stack, one per call in progress, and how many calls use one.
    /// A trap skips the calls between it and the innermost interpreter entry, which leaves their images counted until
    /// that entry returns and releases them (see `super::release_large_images()`).
    struct LargeImages {
        buffers: Vec<Box<[u64]>>,
        in_use: usize,
    }

    thread_local! {
        static LARGE_IMAGES: std::cell::RefCell<LargeImages> = const {
            std::cell::RefCell::new(LargeImages {
                buffers: Vec::new(),
                in_use: 0,
            })
        };
    }

    pub fn large_images_in_use() -> usize {
        LARGE_IMAGES.with(|images| images.borrow().in_use)
    }

    pub fn release_large_images(in_use: usize) {
        LARGE_IMAGES.with(|images| images.borrow_mut().in_use = in_use);
    }

    #[cfg(test)]
    pub fn large_image_buffers() -> usize {
        LARGE_IMAGES.with(|images| images.borrow().buffers.len())
    }

    /// How a call of compiled code through the entry trampoline ended.
    pub enum Ran {
        /// It returned, with its results on the operand stack, whose new top this is.
        Returned(u64),
        /// It threw this exception, which its own handlers did not catch.
        Threw(u64),
        /// It tail called a function that does not run compiled, which the interpreter calls in its place (see
        /// `defer_tail_call()`).
        TailCalled,
    }

    /// Calls `entry`, compiled code of `function` whose values `shape` describes, with the `index`th argument at
    /// `argument(index, param)`. Stores the function's results on an operand stack ending at `end`.
    ///
    /// NB: A trap leaves through the compiled code and does not return, skipping this frame and its callers' up to
    ///     the interpreter, so none of them may own anything that needs dropping.
    unsafe fn run(
        function: &FunctionInstance,
        entry: u64,
        shape: &CallShape,
        argument: impl Fn(usize, &super::ValueShape) -> *const u64,
        end: *mut u64,
    ) -> Ran {
        unsafe {
            let header_data: &FunctionHeader = &function.header;
            let result_slots = header_data.result_slots as usize;
            let mut call = NativeCall {
                entry,
                header: header_data as *const FunctionHeader as u64,
                instance: header_data.instance as u64,
                memory_base: (*header_data.instance).memory0_base as u64,
                result_pointer_slot: u64::MAX,
                ..NativeCall::default()
            };
            const SMALL_IMAGE: usize = 64;
            let mut small_image = [0u64; SMALL_IMAGE];
            let large = shape.image_slots > SMALL_IMAGE;
            let image = if large {
                LARGE_IMAGES.with(|images| {
                    let mut images = images.borrow_mut();
                    let index = images.in_use;
                    images.in_use += 1;
                    if images.buffers.len() <= index {
                        images.buffers.push(Box::new([]));
                    }
                    if images.buffers[index].len() < shape.image_slots {
                        images.buffers[index] = vec![0; shape.image_slots].into_boxed_slice();
                    }
                    images.buffers[index].as_mut_ptr()
                })
            } else {
                small_image.as_mut_ptr()
            };
            for (index, param) in shape.params.iter().enumerate() {
                let source = argument(index, param);
                std::ptr::copy_nonoverlapping(source, image.add(param.home), param.slots);
                if let Place::Register(_) = param.place {
                    if param.ty.uses_xmm() {
                        call.xmms[param.register] = [*source, if param.slots == 2 { *source.add(1) } else { 0 }];
                    } else {
                        call.gprs[param.register] = *source;
                    }
                }
            }
            if shape.has_result_area {
                call.result_pointer_slot = shape.parameter_slots as u64;
                call.result_area_slot = shape.result_area_slot as u64;
            }
            call.image = image as u64;
            call.image_slots = shape.image_slots as u64;
            let trampoline: extern "C" fn(*mut NativeCall) = std::mem::transmute(stubs().entry as usize);
            trampoline(&raw mut call);
            if large {
                LARGE_IMAGES.with(|images| images.borrow_mut().in_use -= 1);
            }
            let context = (*header_data.instance).context;
            if (*context).exception != 0 {
                return Ran::Threw(std::mem::take(&mut (*context).exception));
            }
            let jit = super::store_of(context).jit.as_ref().expect("compiled code ran");
            if jit.tail_call.is_some() {
                return Ran::TailCalled;
            }
            let return_top = end.sub(result_slots);
            for result in &shape.results {
                let mut words = [0u64; 2];
                match result.place {
                    Place::Register(_) if result.ty.uses_xmm() => words = call.result_xmms[result.register],
                    Place::Register(_) => words[0] = call.result_gprs[result.register],
                    Place::Memory(slot) => {
                        let area = image.add(shape.result_area_slot + slot as usize);
                        words[..result.slots].copy_from_slice(ak::slice_from_raw_parts(area, result.slots));
                    }
                }
                // The first result is in the highest slots.
                let destination = return_top.add(result_slots - result.home - result.slots);
                for (slot, word) in words[..result.slots].iter().enumerate() {
                    *destination.add(slot) = slot_bits(result.ty, *word);
                }
            }
            Ran::Returned(return_top as u64)
        }
    }

    /// Whether the call of `address` with `arguments`, through the runtime adapter whose frame is `frame`, is a tail
    /// call of compiled code that the entry trampoline called. If so, keeps the call for the interpreter to make in
    /// place of that code once the trampoline returns, which it does when the adapter returns. The interpreter's
    /// tail calls of compiled code make their calls through the trampoline too, so functions that tail call each
    /// other across the tiers then run in constant stack space.
    pub unsafe fn defer_tail_call(store: &mut Store, address: FunctionAddress, frame: u64, arguments: &[u64]) -> bool {
        let return_address = unsafe { *(frame.wrapping_add_signed(crate::abi::FRAME_RETURN_IP as i64) as *const u64) };
        if return_address != stubs().entry_return {
            return false;
        }
        let jit = store.jit.as_mut().expect("compiled code ran");
        jit.tail_call = Some(address);
        jit.tail_call_arguments.clear();
        jit.tail_call_arguments.extend_from_slice(arguments);
        true
    }

    /// Passes the tail call `defer_tail_call()` kept to the interpreter: the callee's header in the context's first
    /// argument, and the address of its argument slots in the second.
    pub unsafe fn pass_tail_call(store: &mut Store, context: *mut ExecutionContext) {
        let jit = store.jit.as_mut().expect("compiled code ran");
        let function = jit.tail_call.take().expect("compiled code tail called");
        unsafe {
            (*context).arguments[0] = function.as_ptr() as u64;
            (*context).arguments[1] = jit.tail_call_arguments.as_ptr() as u64;
        }
    }

    /// Continues the interpreter frame at `frame`, of `address`, compiled at the loop at `pc` (OSR), once there is
    /// code for that. It cannot be entered at a loop in a legacy catch handler (see `CompileFailure::NoOsrEntry`).
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
            match jit.compile(key, Compile::InBackground) {
                CompileState::Compiled => {}
                CompileState::Pending => return LoopEntry::Pending,
                CompileState::Failed => return LoopEntry::Never,
            }
            let entry = jit.entry(key).expect("the loop is compiled");
            let base = (frame as *mut u8).sub(crate::abi::FRAME_HEADER_SIZE).cast::<u64>();
            match run(function, entry, &shapes(function).osr, |_, _| &raw const frame, base) {
                Ran::Returned(top) => LoopEntry::Ran(top),
                Ran::Threw(exception) => LoopEntry::Threw(exception),
                Ran::TailCalled => LoopEntry::TailCalled,
            }
        }
    }
}

// NB: The interface of the native module, for targets without a code generator, where the JIT is off and most of it
//     goes unused.
#[cfg(not(all(unix, any(target_arch = "x86_64", target_arch = "aarch64"))))]
#[allow(dead_code)]
mod native {
    use crate::abi::ExecutionContext;
    use crate::runtime::{InstanceAddress, Store};

    pub struct Stubs {
        pub trap_exit: u64,
        pub adapter: u64,
    }

    pub static STUBS: std::sync::OnceLock<Option<Stubs>> = std::sync::OnceLock::new();

    pub fn stubs() -> &'static Stubs {
        unreachable!("the JIT is off where it has no code generator")
    }

    pub fn try_stubs() -> Option<&'static Stubs> {
        None
    }

    pub fn forget_dead_modules(_jit: &mut StoreJit) {}

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
        TailCalled,
        Pending,
        Never,
    }

    pub enum Ran {
        Returned(u64),
        Threw(u64),
        TailCalled,
    }

    pub unsafe fn defer_tail_call(
        _store: &mut Store,
        _address: crate::runtime::FunctionAddress,
        _frame: u64,
        _arguments: &[u64],
    ) -> bool {
        unreachable!("the JIT is off where it has no code generator")
    }

    pub unsafe fn pass_tail_call(_store: &mut Store, _context: *mut ExecutionContext) {
        unreachable!("the JIT is off where it has no code generator")
    }

    pub unsafe fn unwind(_store: &mut Store, _exception: u64, _frame: u64) {
        unreachable!("the JIT is off where it has no code generator")
    }

    pub fn compile_instance(_store: &mut Store, _instance: InstanceAddress) {}

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum Compile {
        InBackground,
        Now,
    }

    pub fn compile_function(
        _store: &mut Store,
        _address: crate::runtime::FunctionAddress,
        _when: Compile,
    ) -> CompileState {
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

    pub unsafe fn call_compiled(_context: *mut ExecutionContext) -> Ran {
        unreachable!("the JIT is off where it has no code generator")
    }

    pub fn large_images_in_use() -> usize {
        0
    }

    pub fn release_large_images(_in_use: usize) {}
}

pub use native::StoreJit;
