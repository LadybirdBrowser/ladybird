/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Runs compiled code without the runtime: maps the code of a module's compiled functions executable, points the
//! `native_entry` of the engine's function headers at it, enters it through a trampoline per signature, and turns
//! traps and memory faults into results. Host functions get the JIT's host stubs, and functions the JIT did not
//! compile a stub that traps with `TRAP_NOT_COMPILED`. Each thread has a session of its own: a code space, whose
//! start is the trap exit, the trampolines and the stubs.

#![allow(dead_code)]

#[cfg(target_arch = "aarch64")]
mod aarch64;
pub mod script;
#[cfg(target_arch = "x86_64")]
mod x86_64;

#[cfg(target_arch = "aarch64")]
use aarch64::{not_compiled_code, trampoline_code, trap_exit_code, trap_stub};
#[cfg(target_arch = "x86_64")]
use x86_64::{not_compiled_code, trampoline_code, trap_exit_code, trap_stub};

use libwasm_engine::Module;
use libwasm_engine::abi::{ExecutionContext, InstanceData};
use libwasm_engine::runtime::{FunctionKind, InstanceAddress, Store, Value, helpers};
use libwasm_jit::ir::Type;
use libwasm_jit::{CompileContext, CompileFailure, CompileRequest, Options, RuntimeInfo};
use std::cell::RefCell;
use std::collections::HashMap;
use std::mem::offset_of;
use std::sync::Arc;

/// Runs a program's `main` on a thread of its own where the C library is musl, whose pthread_getattr_np() finds the
/// main thread's stack by probing it page by page with mremap(), which never ends under qemu-user. Elsewhere it runs
/// `main` as it is.
pub fn run_main<T: Send + 'static>(main: impl FnOnce() -> T + Send + 'static) -> T {
    #[cfg(target_env = "musl")]
    {
        std::thread::Builder::new()
            .stack_size(8 << 20)
            .spawn(main)
            .expect("start the main thread")
            .join()
            .expect("the main thread panicked")
    }
    #[cfg(not(target_env = "musl"))]
    main()
}

/// Counts the panics of every thread from the first call on, so that a test sees those of the runtime's compile
/// threads, which would otherwise only leave the functions they compile interpreted.
pub fn panics() -> &'static std::sync::atomic::AtomicUsize {
    static PANICS: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    static INSTALL: std::sync::Once = std::sync::Once::new();
    INSTALL.call_once(|| {
        let previous = std::panic::take_hook();
        std::panic::set_hook(Box::new(move |info| {
            PANICS.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            previous(info);
        }));
    });
    &PANICS
}

/// The trap code of a call that reached a function the JIT did not compile.
pub const TRAP_NOT_COMPILED: u32 = 100;

/// Where the trap exit leaves the trampoline's stack pointer and the trap code.
#[repr(C)]
pub(crate) struct SessionState {
    entry_sp: u64,
    trap: u32,
}

const ENTRY_SP_OFFSET: i32 = offset_of!(SessionState, entry_sp) as i32;
const TRAP_OFFSET: i32 = offset_of!(SessionState, trap) as i32;

const CODE_SPACE_SIZE: usize = 1 << 30;

/// Executable memory, reserved up front. Code is added in batches, which are written while their pages are not
/// executable.
pub struct CodeSpace {
    base: *mut u8,
    size: usize,
    used: usize,
}

fn page_size() -> usize {
    unsafe { libc::sysconf(libc::_SC_PAGESIZE) as usize }
}

impl CodeSpace {
    fn new(size: usize) -> Self {
        let base = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                size,
                libc::PROT_NONE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_NORESERVE,
                -1,
                0,
            )
        };
        assert_ne!(base, libc::MAP_FAILED);
        Self {
            base: base.cast(),
            size,
            used: 0,
        }
    }

    /// Copies each piece of code in and returns their addresses.
    fn add(&mut self, codes: &[&[u8]]) -> Vec<u64> {
        let page_size = page_size();
        let first_page = self.used / page_size * page_size;
        let mut end = self.used;
        let mut starts = Vec::with_capacity(codes.len());
        for code in codes {
            let start = end.next_multiple_of(16);
            starts.push(start);
            end = start + code.len();
        }
        assert!(end <= self.size, "the code space is full");
        let pages = end.next_multiple_of(page_size) - first_page;
        if pages == 0 {
            return starts.iter().map(|start| self.base as u64 + *start as u64).collect();
        }
        let region = unsafe { self.base.add(first_page) };
        unsafe {
            assert_eq!(
                libc::mprotect(region.cast(), pages, libc::PROT_READ | libc::PROT_WRITE),
                0
            );
            for (code, start) in codes.iter().zip(&starts) {
                std::ptr::copy_nonoverlapping(code.as_ptr(), self.base.add(*start), code.len());
            }
            libwasm_jit::runtime::flush_instruction_cache(region as usize, pages);
            assert_eq!(
                libc::mprotect(region.cast(), pages, libc::PROT_READ | libc::PROT_EXEC),
                0
            );
        }
        self.used = end;
        starts.iter().map(|start| self.base as u64 + *start as u64).collect()
    }
}

impl Drop for CodeSpace {
    fn drop(&mut self) {
        unsafe { libc::munmap(self.base.cast(), self.size) };
    }
}

// The code spaces that exist, for the fault handler. Tests run on several threads.
static CODE_RANGES: std::sync::Mutex<Vec<(u64, u64)>> = std::sync::Mutex::new(Vec::new());

/// `RuntimeInfo::throw` of the harness: unwinds the exception in `arguments[0]` from the compiled frame whose fp is
/// `arguments[1]` to the first exception site its callers return to. An exception that leaves compiled code traps with
/// TRAP_UNCAUGHT_EXCEPTION, as host functions that throw do here.
unsafe extern "C" fn throw(context: *mut ExecutionContext) -> u64 {
    unsafe {
        let [exception, frame, ..] = (*context).arguments;
        let read = |address: u64| *(address as *const u64);
        with_session(|session| {
            let mut callee = frame;
            loop {
                let return_address = read(callee - 8);
                let caller = read(callee - 16);
                if session.exception_sites.contains(&return_address) {
                    (*context).exception = exception;
                    (*context).resume = [callee, caller, return_address];
                    break;
                }
                if !session
                    .functions
                    .iter()
                    .any(|(start, end)| (*start..*end).contains(&return_address))
                {
                    (*context).resume = [callee, callee, session.uncaught];
                    break;
                }
                callee = caller;
            }
        });
        context as u64
    }
}

fn ir_types(types: &[libwasm_engine::types::ValType]) -> Option<Vec<Type>> {
    types.iter().map(|ty| libwasm_jit::builder::ir_type(*ty)).collect()
}

type Signature = (Vec<Type>, Vec<Type>);

/// What compiled code of a thread runs with.
struct Session {
    space: CodeSpace,
    state: Box<SessionState>,
    runtime: RuntimeInfo,
    not_compiled: u64,
    /// The stub that traps with TRAP_UNCAUGHT_EXCEPTION, where an exception that leaves compiled code continues.
    uncaught: u64,
    /// The code of compiled functions, and their calls that handle exceptions.
    functions: Vec<(u64, u64)>,
    exception_sites: std::collections::HashSet<u64>,
    trampolines: HashMap<Signature, u64>,
    host_stubs: HashMap<Signature, u64>,
}

impl Session {
    fn new() -> Self {
        install_fault_handler();
        let mut space = CodeSpace::new(CODE_SPACE_SIZE);
        let mut state = Box::new(SessionState { entry_sp: 0, trap: 0 });
        // Every code space starts with its trap exit, which the fault handler sends faults to.
        let trap_exit = space.add(&[&trap_exit_code(&raw mut *state)])[0];
        let not_compiled = space.add(&[&not_compiled_code(trap_exit)])[0];
        let uncaught = space.add(&[&trap_stub(trap_exit, libwasm_engine::abi::TRAP_UNCAUGHT_EXCEPTION)])[0];
        CODE_RANGES
            .lock()
            .unwrap()
            .push((space.base as u64, space.base as u64 + space.size as u64));
        Self {
            space,
            state,
            runtime: RuntimeInfo {
                trap_exit,
                memory_grow: helpers::wasm_runtime_memory_grow as *const () as u64,
                table_grow: helpers::wasm_runtime_table_grow as *const () as u64,
                call_host: helpers::wasm_runtime_call_host as *const () as u64,
                allocate: helpers::wasm_runtime_allocate as *const () as u64,
                wait: helpers::wasm_runtime_wait as *const () as u64,
                notify: helpers::wasm_runtime_notify as *const () as u64,
                allocate_exception: libwasm_engine::runtime::jit::wasm_runtime_allocate_exception as *const () as u64,
                throw: throw as *const () as u64,
            },
            not_compiled,
            uncaught,
            functions: Vec::new(),
            exception_sites: std::collections::HashSet::new(),
            trampolines: HashMap::new(),
            host_stubs: HashMap::new(),
        }
    }

    fn trampoline(&mut self, signature: &Signature) -> u64 {
        if let Some(address) = self.trampolines.get(signature) {
            return *address;
        }
        let code = trampoline_code(&signature.0, &signature.1, &raw mut *self.state);
        let address = self.space.add(&[&code])[0];
        self.trampolines.insert(signature.clone(), address);
        address
    }

    fn host_stub(&mut self, signature: &Signature) -> u64 {
        if let Some(address) = self.host_stubs.get(signature) {
            return *address;
        }
        let code = libwasm_jit::stubs::host_stub(&signature.0, &signature.1, &self.runtime);
        let address = self.space.add(&[&code])[0];
        self.host_stubs.insert(signature.clone(), address);
        address
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        let start = self.space.base as u64;
        CODE_RANGES
            .lock()
            .unwrap()
            .retain(|(range_start, _)| *range_start != start);
    }
}

thread_local! {
    static SESSION: RefCell<Option<Session>> = const { RefCell::new(None) };
}

fn with_session<R>(f: impl FnOnce(&mut Session) -> R) -> R {
    SESSION.with(|session| f(session.borrow_mut().get_or_insert_with(Session::new)))
}

/// A module's functions compiled into the thread's code space.
pub struct CompiledModule {
    pub module: Arc<Module>,
    /// The native entry of each function (counting imports), or 0 if it was not compiled.
    pub entries: Vec<u64>,
    /// Why each function was not compiled.
    pub failures: Vec<Option<CompileFailure>>,
    /// Whether each function and everything it calls directly was compiled.
    pub runnable: Vec<bool>,
}

impl CompiledModule {
    pub fn compile(module: Arc<Module>, options: Options) -> Self {
        let runtime = with_session(|session| session.runtime);
        let total = module.total_function_count() as usize;
        let first = module.imported_function_count as usize;
        let mut entries = vec![0; total];
        let mut failures = vec![None; total];
        let mut codes = Vec::new();
        let mut context = CompileContext::default();
        for (index, slot) in failures.iter_mut().enumerate().skip(first) {
            let request = CompileRequest {
                osr: None,
                module: module.clone(),
                function: index as u32,
                runtime,
                options,
            };
            match libwasm_jit::compile(&mut context, &request) {
                Ok(compiled) => codes.push((index, compiled.code, compiled.exception_sites)),
                Err(failure) => *slot = Some(failure),
            }
        }
        let slices = codes.iter().map(|(_, code, _)| code.as_slice()).collect::<Vec<_>>();
        let addresses = with_session(|session| {
            let addresses = session.space.add(&slices);
            for ((_, code, sites), address) in codes.iter().zip(&addresses) {
                session.functions.push((*address, *address + code.len() as u64));
                session
                    .exception_sites
                    .extend(sites.iter().map(|offset| *address + u64::from(*offset)));
            }
            addresses
        });
        for ((index, _, _), address) in codes.iter().zip(addresses) {
            entries[*index] = address;
        }
        // A function runs compiled if it and every function it calls directly compiled.
        let callees = (0..total)
            .map(|index| {
                if index < first {
                    Vec::new()
                } else {
                    direct_callees(&module, index as u32)
                }
            })
            .collect::<Vec<_>>();
        let mut runnable = (0..total)
            .map(|index| index < first || entries[index] != 0)
            .collect::<Vec<_>>();
        loop {
            let mut changed = false;
            for index in first..total {
                if runnable[index] && callees[index].iter().any(|callee| !runnable[*callee as usize]) {
                    runnable[index] = false;
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }
        for flag in runnable.iter_mut().take(first) {
            *flag = false;
        }
        Self {
            module,
            entries,
            failures,
            runnable,
        }
    }
}

/// The functions `index` calls directly.
fn direct_callees(module: &Module, index: u32) -> Vec<u32> {
    use libwasm_jit::decode::{Decoder, Instruction};
    let body = &module.code[(index - module.imported_function_count) as usize];
    let mut decoder = Decoder::new(&module.bytes, body.code.start as usize, body.code.end as usize);
    let mut callees = Vec::new();
    while !decoder.is_at_end() {
        if let Instruction::Call(callee) | Instruction::ReturnCall(callee) = decoder.decode() {
            callees.push(callee);
        }
    }
    callees
}

/// An engine instance whose functions run compiled.
pub struct TestBinding {
    instance: InstanceAddress,
    data: *const InstanceData,
}

/// The outcome of a call.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Outcome {
    Values(Vec<Value>),
    Trap(u32),
}

#[cfg(target_os = "macos")]
fn lowest_stack_address() -> u64 {
    unsafe {
        let thread = libc::pthread_self();
        (libc::pthread_get_stackaddr_np(thread) as usize - libc::pthread_get_stacksize_np(thread)) as u64
    }
}

#[cfg(not(target_os = "macos"))]
fn lowest_stack_address() -> u64 {
    unsafe {
        let mut attributes: libc::pthread_attr_t = std::mem::zeroed();
        assert_eq!(libc::pthread_getattr_np(libc::pthread_self(), &raw mut attributes), 0);
        let mut address = std::ptr::null_mut();
        let mut size = 0;
        libc::pthread_attr_getstack(&raw const attributes, &raw mut address, &raw mut size);
        libc::pthread_attr_destroy(&raw mut attributes);
        address as u64
    }
}

impl TestBinding {
    /// Points the instance's functions at their compiled code, and its host function imports at host stubs.
    pub fn new(compiled: &CompiledModule, store: &Store, instance: InstanceAddress) -> Self {
        let engine = store.instance(instance);
        let first = compiled.module.imported_function_count as usize;
        with_session(|session| {
            for (index, address) in engine.functions.iter().enumerate() {
                let function = store.function(*address);
                let entry = match &function.kind {
                    FunctionKind::Host(_) => {
                        let signature = (
                            ir_types(&function.ty.params).expect("the JIT has a type for every value"),
                            ir_types(&function.ty.results).expect("the JIT has a type for every value"),
                        );
                        session.host_stub(&signature)
                    }
                    _ if index < first => continue,
                    _ if compiled.entries[index] != 0 => compiled.entries[index],
                    _ => session.not_compiled,
                };
                // NB: The headers are the engine's; nothing else reads or writes this field yet.
                let header = engine.data.functions.wrapping_add(index);
                unsafe {
                    let header = (*header).cast_mut();
                    (&raw mut (*header).native_entry).write(entry);
                }
            }
        });
        Self {
            instance,
            data: &raw const engine.data,
        }
    }

    /// Calls compiled function `index` of the instance with `arguments`.
    pub fn call(&mut self, store: &mut Store, compiled: &CompiledModule, index: u32, arguments: &[Value]) -> Outcome {
        assert!(compiled.runnable[index as usize]);
        let ty = compiled.module.function_type(index).unwrap();
        let signature = (ir_types(&ty.params).unwrap(), ir_types(&ty.results).unwrap());
        let trampoline = with_session(|session| session.trampoline(&signature));
        let mut slots = Vec::with_capacity(arguments.len());
        for argument in arguments {
            argument.push_slots(&mut slots);
        }
        let result_count = ty.results.iter().map(|result| result.slot_count() as usize).sum();
        let mut result_slots = vec![0u64; result_count];
        unsafe {
            let context: *mut ExecutionContext = (*self.data).context;
            (*context).stack_limit = lowest_stack_address() + 256 * 1024;
            (*context).store = (store as *mut Store).cast();
        }
        let function: extern "C" fn(u64, *const u64, *mut u64, *const InstanceData) -> u32 =
            unsafe { std::mem::transmute(trampoline) };
        let status = function(
            compiled.entries[index as usize],
            slots.as_ptr(),
            result_slots.as_mut_ptr(),
            self.data,
        );
        if status != 0 {
            return Outcome::Trap(with_session(|session| session.state.trap));
        }
        Outcome::Values(
            ty.results
                .iter()
                .scan(0, |slot, ty| {
                    let value = Value::from_slots(*ty, &result_slots[*slot..]);
                    *slot += ty.slot_count() as usize;
                    Some(value)
                })
                .collect(),
        )
    }
}

/// The module function index of an engine function, if it is a WebAssembly function of `instance`.
pub fn function_index(
    store: &Store,
    address: libwasm_engine::runtime::FunctionAddress,
    instance: InstanceAddress,
) -> Option<u32> {
    match &store.function(address).kind {
        FunctionKind::Wasm {
            index, instance: owner, ..
        } if *owner == instance => Some(*index),
        _ => None,
    }
}

// Memory faults in compiled code become traps through the trap exit.

static INSTALL: std::sync::Once = std::sync::Once::new();
static mut PREVIOUS: libc::sigaction = unsafe { std::mem::zeroed() };
static mut PREVIOUS_SIGBUS: libc::sigaction = unsafe { std::mem::zeroed() };

/// The program counter of a signal handler's context, and the register the trap exit takes the trap code in.
#[cfg(all(target_arch = "x86_64", target_os = "linux"))]
unsafe fn fault_registers(context: *mut libc::ucontext_t) -> (*mut u64, *mut u64) {
    unsafe {
        let registers = &raw mut (*context).uc_mcontext.gregs;
        (
            (&raw mut (*registers)[libc::REG_RIP as usize]).cast(),
            (&raw mut (*registers)[libc::REG_RDI as usize]).cast(),
        )
    }
}

#[cfg(all(target_arch = "x86_64", target_os = "macos"))]
unsafe fn fault_registers(context: *mut libc::ucontext_t) -> (*mut u64, *mut u64) {
    unsafe {
        let state = &raw mut (*(*context).uc_mcontext).__ss;
        ((&raw mut (*state).__rip), (&raw mut (*state).__rdi))
    }
}

#[cfg(all(target_arch = "aarch64", target_os = "linux"))]
unsafe fn fault_registers(context: *mut libc::ucontext_t) -> (*mut u64, *mut u64) {
    unsafe {
        let mcontext = &raw mut (*context).uc_mcontext;
        ((&raw mut (*mcontext).pc), (&raw mut (*mcontext).regs[0]))
    }
}

#[cfg(all(target_arch = "aarch64", target_os = "macos"))]
unsafe fn fault_registers(context: *mut libc::ucontext_t) -> (*mut u64, *mut u64) {
    unsafe {
        let state = &raw mut (*(*context).uc_mcontext).__ss;
        ((&raw mut (*state).__pc), (&raw mut (*state).__x[0]))
    }
}

extern "C" fn handle_fault(signal: i32, info: *mut libc::siginfo_t, context: *mut libc::c_void) {
    unsafe {
        let context = context.cast::<libc::ucontext_t>();
        let (pc_register, code_register) = fault_registers(context);
        let pc = *pc_register;
        let address = (*info).si_addr() as usize;
        let exit = CODE_RANGES.try_lock().ok().and_then(|ranges| {
            ranges
                .iter()
                .find(|(start, end)| pc >= *start && pc < *end)
                .map(|(start, _)| *start)
        });
        if let Some(start) = exit
            && libwasm_engine::runtime::memory::is_in_any_reservation(address)
        {
            // Every code space starts with its trap exit.
            *pc_register = start;
            *code_register = u64::from(libwasm_engine::abi::TRAP_MEMORY_OUT_OF_BOUNDS);
            return;
        }
        let previous = if signal == libc::SIGSEGV {
            std::ptr::read(&raw const PREVIOUS)
        } else {
            std::ptr::read(&raw const PREVIOUS_SIGBUS)
        };
        if previous.sa_flags & libc::SA_SIGINFO != 0 && previous.sa_sigaction > libc::SIG_IGN {
            let handler: extern "C" fn(i32, *mut libc::siginfo_t, *mut libc::c_void) =
                std::mem::transmute(previous.sa_sigaction);
            handler(signal, info, context.cast());
            return;
        }
        libc::signal(signal, libc::SIG_DFL);
    }
}

fn install_fault_handler() {
    INSTALL.call_once(|| unsafe {
        // NB: The engine's handler, which goes first if it is installed, forwards faults it does not know to us.
        let mut action: libc::sigaction = std::mem::zeroed();
        action.sa_sigaction = handle_fault as *const () as usize;
        action.sa_flags = libc::SA_SIGINFO | libc::SA_ONSTACK;
        libc::sigemptyset(&raw mut action.sa_mask);
        libc::sigaction(libc::SIGSEGV, &raw const action, &raw mut PREVIOUS);
        // NB: Accesses of the guard pages raise SIGBUS on macOS.
        libc::sigaction(libc::SIGBUS, &raw const action, &raw mut PREVIOUS_SIGBUS);
    });
}
