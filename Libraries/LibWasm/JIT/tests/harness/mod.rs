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

pub mod script;

use libwasm_engine::Module;
use libwasm_engine::abi::{ExecutionContext, InstanceData};
use libwasm_engine::runtime::{FunctionKind, InstanceAddress, Store, Value, helpers};
use libwasm_jit::asm::x86_64::{Assembler, Precision, VexMap, VexOpcode, VexOperand, VexPrefix};
use libwasm_jit::asm::{Address, Gpr, Width};
use libwasm_jit::convention::{self, Place};
use libwasm_jit::ir::Type;
use libwasm_jit::{CompileContext, CompileFailure, CompileRequest, Options, RuntimeInfo};
use std::cell::RefCell;
use std::collections::HashMap;
use std::mem::offset_of;
use std::sync::Arc;

/// The trap code of a call that reached a function the JIT did not compile.
pub const TRAP_NOT_COMPILED: u32 = 100;

/// Where the trap exit leaves the trampoline's stack pointer and the trap code.
#[repr(C)]
struct SessionState {
    entry_sp: u64,
    trap: u32,
}

const ENTRY_SP_OFFSET: i32 = offset_of!(SessionState, entry_sp) as i32;
const TRAP_OFFSET: i32 = offset_of!(SessionState, trap) as i32;

const CODE_SPACE_SIZE: usize = 1 << 30;

const VMOVUPS_LOAD: VexOpcode = VexOpcode::new(VexPrefix::None, VexMap::Map0F, 0x10);
const VMOVUPS_STORE: VexOpcode = VexOpcode::new(VexPrefix::None, VexMap::Map0F, 0x11);

/// Executable memory, reserved up front. Code is added in batches, which are written while their pages are not
/// executable.
pub struct CodeSpace {
    base: *mut u8,
    size: usize,
    used: usize,
}

const PAGE_SIZE: usize = 4096;

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
        let first_page = self.used / PAGE_SIZE * PAGE_SIZE;
        let mut end = self.used;
        let mut starts = Vec::with_capacity(codes.len());
        for code in codes {
            let start = end.next_multiple_of(16);
            starts.push(start);
            end = start + code.len();
        }
        assert!(end <= self.size, "the code space is full");
        let pages = end.next_multiple_of(PAGE_SIZE) - first_page;
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
            __clear_cache(region.cast(), region.add(pages).cast());
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

/// The trap exit, entered with the trap code in edi: returns 1 from the trampoline.
fn trap_exit_code(state: *mut SessionState) -> Vec<u8> {
    let mut asm = Assembler::new();
    let rax = Gpr(convention::RAX);
    let sp = Gpr(convention::RSP);
    asm.mov_ri(rax, state as u64);
    asm.mov_mr(Width::W32, &Address::new(rax, TRAP_OFFSET), Gpr(convention::RDI));
    asm.mov_rm(Width::W64, sp, &Address::new(rax, ENTRY_SP_OFFSET));
    restore_and_return(&mut asm, 1);
    asm.finish().unwrap()
}

/// The code of functions the JIT did not compile.
fn not_compiled_code(trap_exit: u64) -> Vec<u8> {
    let mut asm = Assembler::new();
    asm.mov_ri32(Gpr(convention::RDI), TRAP_NOT_COMPILED);
    asm.mov_ri(Gpr(convention::R11), trap_exit);
    asm.jmp_r(Gpr(convention::R11));
    asm.finish().unwrap()
}

fn restore_and_return(asm: &mut Assembler, status: u32) {
    asm.add_ri(Width::W64, Gpr(convention::RSP), 8);
    for register in [
        convention::R15,
        convention::R14,
        convention::R13,
        convention::R12,
        convention::RBX,
        convention::RBP,
    ] {
        asm.pop(Gpr(register));
    }
    asm.mov_ri32(Gpr(convention::RAX), status);
    asm.ret();
}

/// `extern "C" fn(entry, arguments, results, instance) -> u32`: calls `entry` with the arguments' slots under the
/// compiled convention, stores the results' slots, and returns 0, or 1 if the code trapped.
fn trampoline_code(params: &[Type], results: &[Type], state: *mut SessionState) -> Vec<u8> {
    let mut asm = Assembler::new();
    let g = Gpr;
    let (rdi, rsi, rdx, rcx) = (
        g(convention::RDI),
        g(convention::RSI),
        g(convention::RDX),
        g(convention::RCX),
    );
    let sp = g(convention::RSP);
    let scratch = g(convention::R11);
    for register in [
        convention::RBP,
        convention::RBX,
        convention::R12,
        convention::R13,
        convention::R14,
        convention::R15,
    ] {
        asm.push(g(register));
    }
    asm.sub_ri(Width::W64, sp, 8);
    asm.mov_mr(Width::W64, &Address::new(sp, 0), rdx);
    asm.mov_ri(scratch, state as u64);
    asm.mov_mr(Width::W64, &Address::new(scratch, ENTRY_SP_OFFSET), sp);
    let outgoing = convention::outgoing_size(params, results).next_multiple_of(16) as i32;
    if outgoing > 0 {
        asm.sub_ri(Width::W64, sp, outgoing);
    }
    let entry = g(convention::R10);
    asm.mov_rr(Width::W64, entry, rdi);
    asm.mov_rr(Width::W64, scratch, rsi);
    asm.mov_rr(Width::W64, g(convention::INSTANCE_REGISTER), rcx);
    asm.mov_rm(
        Width::W64,
        g(convention::MEMORY_BASE_REGISTER),
        &Address::new(rcx, offset_of!(InstanceData, memory0_base) as i32),
    );
    // Copies the 8 or 16 bytes of a value of type `ty` between memory locations, through xmm15.
    let copy = |asm: &mut Assembler, ty: Type, from: Address, to: Address| {
        if ty == Type::V128 {
            asm.vex(VMOVUPS_LOAD, 15, 0, VexOperand::Memory(from), None);
            asm.vex(VMOVUPS_STORE, 15, 0, VexOperand::Memory(to), None);
        } else {
            asm.vmovs_rm(Precision::Double, libwasm_jit::asm::Fpr(15), &from);
            asm.vmovs_mr(Precision::Double, &to, libwasm_jit::asm::Fpr(15));
        }
    };
    // Loads or stores a value of type `ty` in `register` (true) at `address`.
    let transfer = |asm: &mut Assembler, ty: Type, register: u8, address: Address, load: bool| {
        if ty == Type::V128 {
            let opcode = if load { VMOVUPS_LOAD } else { VMOVUPS_STORE };
            asm.vex(
                opcode,
                register - convention::FPR_BASE,
                0,
                VexOperand::Memory(address),
                None,
            );
        } else if convention::is_fpr(register) {
            let register = libwasm_jit::asm::Fpr(register - convention::FPR_BASE);
            if load {
                asm.vmovs_rm(Precision::Double, register, &address);
            } else {
                asm.vmovs_mr(Precision::Double, &address, register);
            }
        } else if load {
            asm.mov_rm(Width::W64, g(register), &address);
        } else {
            asm.mov_mr(Width::W64, &address, g(register));
        }
    };
    let mut places = Vec::new();
    convention::parameter_places(params, &mut places);
    // Stack arguments first, then registers, so that rsi and the others are read before they are written. Argument
    // slots are laid out like home slots, so a parameter's slot is the same in both.
    for (index, place) in places.iter().enumerate() {
        if let Place::Memory(slot) = place {
            let from = Address::new(scratch, 8 * *slot as i32);
            copy(&mut asm, params[index], from, Address::new(sp, 8 * *slot as i32));
        }
    }
    for (index, place) in places.iter().enumerate() {
        if let Place::Register(register) = place {
            let slot = convention::home_slot(params, index);
            transfer(
                &mut asm,
                params[index],
                *register,
                Address::new(scratch, 8 * slot as i32),
                true,
            );
        }
    }
    let parameter_slots = convention::slots(params) as i32;
    if convention::has_result_area(results) {
        let pointer = 8 * parameter_slots;
        asm.lea(Width::W64, g(convention::RAX), &Address::new(sp, pointer + 8));
        asm.mov_mr(Width::W64, &Address::new(sp, pointer), g(convention::RAX));
    }
    asm.call_r(entry);
    // NB: Tail calls may have moved rsp.
    asm.mov_ri(scratch, state as u64);
    asm.mov_rm(Width::W64, sp, &Address::new(scratch, ENTRY_SP_OFFSET));
    if outgoing > 0 {
        asm.sub_ri(Width::W64, sp, outgoing);
    }
    asm.mov_rm(Width::W64, scratch, &Address::new(sp, outgoing));
    convention::result_places(results, &mut places);
    for (index, place) in places.iter().enumerate() {
        let slot = convention::home_slot(results, index);
        let address = Address::new(scratch, 8 * slot as i32);
        match place {
            Place::Register(register) => transfer(&mut asm, results[index], *register, address, false),
            Place::Memory(slot) => {
                let area = 8 * (parameter_slots + 1) + 8 * *slot as i32;
                copy(&mut asm, results[index], Address::new(sp, area), address);
            }
        }
    }
    if outgoing > 0 {
        asm.add_ri(Width::W64, sp, outgoing);
    }
    restore_and_return(&mut asm, 0);
    asm.finish().unwrap()
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
            },
            not_compiled,
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

unsafe extern "C" {
    fn __clear_cache(start: *mut libc::c_char, end: *mut libc::c_char);
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
                Ok(compiled) => codes.push((index, compiled.code)),
                Err(failure) => *slot = Some(failure),
            }
        }
        let slices = codes.iter().map(|(_, code)| code.as_slice()).collect::<Vec<_>>();
        let addresses = with_session(|session| session.space.add(&slices));
        for ((index, _), address) in codes.iter().zip(addresses) {
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

extern "C" fn handle_fault(signal: i32, info: *mut libc::siginfo_t, context: *mut libc::c_void) {
    unsafe {
        let context = context.cast::<libc::ucontext_t>();
        let pc = (*context).uc_mcontext.gregs[libc::REG_RIP as usize] as u64;
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
            (*context).uc_mcontext.gregs[libc::REG_RIP as usize] = start as i64;
            (*context).uc_mcontext.gregs[libc::REG_RDI as usize] =
                i64::from(libwasm_engine::abi::TRAP_MEMORY_OUT_OF_BOUNDS);
            return;
        }
        let previous = std::ptr::read(&raw const PREVIOUS);
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
    });
}
