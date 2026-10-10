/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Installed JIT code and the JIT state of an executable.

use core::cell::{Cell, RefCell};
use core::ffi::c_void;

pub use libjs_jit::code::INLINED_CALL_SITE_BIT;
use libjs_jit::code::{ExitKind, Site};

use super::entry_table::{JIT_ENTRY_SLOT_MASK, JitEntryTable, NO_JIT_ENTRY_SLOT};
use super::executable_memory::ExecutableMemory;
use crate::bytecode::executable::Executable;
use crate::gc::capi;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::execution_context::ExecutionContext;
use crate::layout::function_object::EcmascriptFunctionObject;
use crate::runtime::native_javascript_backed_function::NativeJavaScriptBackedFunction;

/// The status word of what JIT code returns, `libjs_jit::code::JitStatus`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u64)]
pub enum EntryStatus {
    /// `value` is the encoded return value. The frame is still the running execution context; the caller pops it like
    /// the interpreter's Return.
    Returned = 0,
    /// Continue interpreting the running execution context at its program counter: an exit wrote the frame state
    /// back, or a slow path routed an exception to a handler.
    Resume = 1,
    /// An exception propagates out of the interpreter invocation, like a slow path returning a negative control word.
    ExitInterpreter = 2,
}

/// What JIT code returns, in two registers by the SysV and AAPCS64 ABIs.
#[repr(C)]
pub struct JitResult {
    pub value: u64,
    pub status: EntryStatus,
}

/// `entry(VM*, ExecutionContext* frame)`: the frame is fully built for its executable, at pc 0 with Enter not run yet
/// (or, for an on-stack replacement entry, at the loop back edge the entry is for).
pub type JitEntry = unsafe extern "C" fn(*const Vm, *mut ExecutionContext) -> JitResult;

/// `JitResult trampoline(VM*, ExecutionContext* frame, JitEntry entry)`: how native code calls JIT code (see
/// `libjs_jit::codegen::generate_entry_trampoline()`), which expects the VM in its pinned register.
pub type EntryTrampoline = unsafe extern "C" fn(*const Vm, *mut ExecutionContext, JitEntry) -> JitResult;

/// Where an executable is in its JIT compilation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum CompileState {
    /// Not compiled, or the code was discarded.
    None = 0,
    /// A compile job for the executable is running on the compile thread.
    Queued = 1,
    Installed = crate::layout::executable::JIT_COMPILE_STATE_INSTALLED,
    /// The code was discarded while JIT code was still running frames of it. It stays attached (but is not entered)
    /// until no live frame of the executable is run by JIT code.
    Discarding = 3,
    /// The executable is never compiled (again).
    Refused = 4,
}

impl CompileState {
    fn from_u8(value: u8) -> Self {
        match value {
            0 => Self::None,
            1 => Self::Queued,
            2 => Self::Installed,
            3 => Self::Discarding,
            4 => Self::Refused,
            _ => unreachable!("{value} is not a compile state"),
        }
    }
}

/// An executable of a compile job's snapshot, by index. Index 0 is the compiled executable (without a function); the
/// others are inlining candidates, whose frames exits materialize.
#[derive(Clone, Copy)]
pub struct SnapshotExecutable {
    pub executable: Gc<Executable>,
    pub function: Option<InlinedFunction>,
}

/// The function object of an inlined executable, which its materialized frames run.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub enum InlinedFunction {
    Ecmascript(Gc<EcmascriptFunctionObject>),
    /// A builtin written in JavaScript, whose frames are set up like those of other builtins.
    Builtin(Gc<NativeJavaScriptBackedFunction>),
}

impl InlinedFunction {
    pub fn visit(self, visitor: &mut Visitor) {
        match self {
            InlinedFunction::Ecmascript(function) => visitor.visit(function),
            InlinedFunction::Builtin(function) => visitor.visit(function),
        }
    }
}

/// Installed machine code for one executable, with what exits need to rebuild interpreter frames. While JIT code runs
/// frames of its executable, it stays attached to the executable (in its slot of the JIT entry table, where
/// `libjs_jit_exit()` finds it through the frame's executable), even once discarded.
pub struct JitCode {
    exit_count: Cell<u32>,
    memory: ExecutableMemory,
    entry: JitEntry,
    sites: Vec<Site>,
    /// The cells the code compares against or uses. The executable the code is attached to keeps them alive, so they
    /// keep their addresses for as long as the code may run.
    embedded_cells: Vec<Gc<CellHeader>>,
    snapshot_executables: Vec<SnapshotExecutable>,
    /// Where a frame running in the interpreter at the loop back edge `pc` can continue in this code.
    osr_entries: Vec<(u32, JitEntry)>,
    /// Identifies the code among all code the VM installed, for the dependencies it registers (see
    /// `super::dependencies`).
    id: u64,
    /// The bytes that invalidate the code at each offset of it: jumps to the exits of its `AssumeValid` nodes, which
    /// are no-ops until then.
    invalidation_patches: Vec<(u32, Vec<u8>)>,
    invalidated: Cell<bool>,
}

impl JitCode {
    pub fn new(
        memory: ExecutableMemory,
        entry_offset: u32,
        osr_entry_offsets: &[(u32, u32)],
        sites: Vec<Site>,
        embedded_cells: Vec<Gc<CellHeader>>,
        snapshot_executables: Vec<SnapshotExecutable>,
    ) -> Self {
        let entry_at = |offset: u32| {
            assert!((offset as usize) < memory.size());
            // SAFETY: The code has an entry point with the JitEntry calling convention at the offset.
            unsafe { core::mem::transmute::<*const u8, JitEntry>(memory.address().add(offset as usize)) }
        };
        let entry = entry_at(entry_offset);
        let osr_entries = osr_entry_offsets
            .iter()
            .map(|&(pc, offset)| (pc, entry_at(offset)))
            .collect();
        Self {
            exit_count: Cell::new(0),
            memory,
            entry,
            sites,
            embedded_cells,
            snapshot_executables,
            osr_entries,
            id: 0,
            invalidation_patches: Vec::new(),
            invalidated: Cell::new(false),
        }
    }

    /// Gives the code its id among all code the VM installed, and the bytes that invalidate it.
    pub fn with_invalidation(mut self, id: u64, invalidation_patches: Vec<(u32, Vec<u8>)>) -> Self {
        self.id = id;
        self.invalidation_patches = invalidation_patches;
        self
    }

    pub fn id(&self) -> u64 {
        self.id
    }

    pub fn is_invalidated(&self) -> bool {
        self.invalidated.get()
    }

    /// Makes every frame running the code exit where it relies on what it depends on next (see
    /// `libjs_jit::code::Dependency`), which no longer holds. Only the main thread runs JIT code, which is suspended
    /// in other code while this runs.
    pub fn invalidate(&self) {
        if self.invalidated.replace(true) {
            return;
        }
        self.memory.patch(&self.invalidation_patches);
    }

    pub fn entry(&self) -> JitEntry {
        self.entry
    }

    pub fn osr_entry(&self, pc: u32) -> Option<JitEntry> {
        self.osr_entries
            .iter()
            .find(|(entry_pc, _)| *entry_pc == pc)
            .map(|(_, entry)| *entry)
    }

    pub fn site(&self, index: u32) -> &Site {
        &self.sites[index as usize]
    }

    pub fn snapshot_executable(&self, index: u32) -> SnapshotExecutable {
        self.snapshot_executables[index as usize]
    }

    /// How often the code exited.
    pub fn exit_count(&self) -> u32 {
        self.exit_count.get()
    }

    /// Counts an exit and returns how many there were.
    pub fn count_exit(&self) -> u32 {
        let count = self.exit_count.get() + 1;
        self.exit_count.set(count);
        count
    }
}

unsafe impl Trace for JitCode {
    fn trace(&self, visitor: &mut Visitor) {
        for cell in &self.embedded_cells {
            visitor.visit(*cell);
        }
        for executable in &self.snapshot_executables {
            visitor.visit(executable.executable);
            if let Some(function) = executable.function {
                function.visit(visitor);
            }
        }
    }
}

/// The JIT state of an executable that the interpreter and JIT code do not read: they read the fields of its head.
#[derive(Default)]
pub struct ExecutableJitState {
    /// Places where JIT code exited. Recompiles never repeat a speculation of the same kind at the same pc.
    exit_sites: RefCell<Vec<(u32, ExitKind)>>,
    /// Places in builtins written in JavaScript where the executable's JIT code exited while running them inlined,
    /// by the address of the builtin's executable. Builtins are inlined into all their callers, so these are kept per
    /// caller: what failed in one caller says nothing about the others.
    builtin_exit_sites: RefCell<Vec<(u64, u32, ExitKind)>>,
    /// How often the executable's code was discarded.
    discard_count: Cell<u32>,
}

/// After this many exits, JIT code is discarded so that the executable can be recompiled with what the exits taught
/// it.
pub const EXIT_COUNT_BEFORE_DISCARD: u32 = 10;

/// An executable whose code was discarded this often is not compiled again.
pub const MAX_DISCARD_COUNT: u32 = 8;

impl Executable {
    pub fn jit_compile_state(&self) -> CompileState {
        CompileState::from_u8(self.head.jit_compile_state.get())
    }

    pub fn set_jit_compile_state(&self, state: CompileState) {
        self.head.jit_compile_state.set(state as u8);
    }

    /// The VM whose heap the executable lives in.
    fn heap_vm(&self) -> &Vm {
        // SAFETY: The VM creates its heap with itself as the context, and outlives every cell in it.
        unsafe { &*capi::gc_cell_heap_context(core::ptr::from_ref(self).cast()).cast::<Vm>() }
    }

    /// The executable's slot in the JIT entry table and the table, if it has one.
    fn jit_entry_table_slot(&self) -> Option<(&JitEntryTable, u32)> {
        let slot = self.head.jit_entry_slot.get();
        if slot & JIT_ENTRY_SLOT_MASK == NO_JIT_ENTRY_SLOT {
            return None;
        }
        let table = self
            .heap_vm()
            .jit
            .entry_table
            .as_ref()
            .expect("executables only have slots while the JIT is on");
        assert!(
            table.is_owned_by(slot, core::ptr::from_ref(&self.head) as u64),
            "the executable owns its JIT entry slot"
        );
        Some((table, slot))
    }

    /// Gives the executable a slot in the JIT entry table, if the JIT is on. Executables without one are never
    /// compiled.
    pub fn allocate_jit_entry_slot(&self, vm: &Vm) {
        let Some(table) = vm.jit.entry_table.as_ref() else {
            return;
        };
        let slot = table.allocate(core::ptr::from_ref(&self.head) as u64);
        self.head.jit_entry_slot.set(slot);
        if slot == NO_JIT_ENTRY_SLOT {
            super::tier_up::refuse_jit_compile(vm, self);
        }
    }

    /// Frees the slot of the executable, which died, and drops its JIT code.
    pub fn free_jit_entry_slot(&self, vm: &Vm) {
        if let Some(table) = vm.jit.entry_table.as_ref() {
            table.free(self.head.jit_entry_slot.get(), core::ptr::from_ref(&self.head) as u64);
        }
    }

    /// The executable's attached JIT code, if any.
    pub fn jit_code(&self) -> Option<&JitCode> {
        let (table, slot) = self.jit_entry_table_slot()?;
        table.code(slot)
    }

    /// Visits the cells the executable's attached JIT code keeps alive.
    pub fn trace_jit_code(&self, visitor: &mut Visitor) {
        if let Some(code) = self.jit_code() {
            code.trace(visitor);
        }
    }

    pub fn jit_exit_sites(&self) -> Vec<(u32, ExitKind)> {
        self.jit_state().exit_sites.borrow().clone()
    }

    pub fn add_jit_exit_site(&self, site: (u32, ExitKind)) {
        let mut sites = self.jit_state().exit_sites.borrow_mut();
        if !sites.contains(&site) {
            sites.push(site);
        }
    }

    pub fn jit_builtin_exit_sites(&self) -> Vec<(u64, u32, ExitKind)> {
        self.jit_state().builtin_exit_sites.borrow().clone()
    }

    pub fn add_jit_builtin_exit_site(&self, site: (u64, u32, ExitKind)) {
        let mut sites = self.jit_state().builtin_exit_sites.borrow_mut();
        if !sites.contains(&site) {
            sites.push(site);
        }
    }

    pub fn jit_discard_count(&self) -> u32 {
        self.jit_state().discard_count.get()
    }

    /// Makes new frames of the executable run in its JIT code. Frames of it that run in the interpreter count down
    /// `tier_up_budget` from here, and continue in the code at an on-stack replacement entry once it runs out.
    pub fn install_jit_code(&self, code: Box<JitCode>, tier_up_budget: i32) {
        let (table, slot) = self
            .jit_entry_table_slot()
            .expect("only executables with a JIT entry slot are compiled");
        table.set_entry(slot, Some(code.entry() as *const c_void));
        assert!(table.set_code(slot, Some(code)).is_none());
        self.set_jit_compile_state(CompileState::Installed);
        self.head.tier_up_budget.set(tier_up_budget);
    }

    /// Stops entering the executable's JIT code and drops it once no live frame of the executable is run by JIT code
    /// anymore (see `detach_discarded_jit_code_if_unused()`). The executable may be compiled again later.
    pub fn discard_jit_code(&self, vm: &Vm) {
        assert_eq!(self.jit_compile_state(), CompileState::Installed);
        let (table, slot) = self.jit_entry_table_slot().expect("the code is attached");
        table.set_entry(slot, None);
        self.set_jit_compile_state(CompileState::Discarding);
        if self.detach_discarded_jit_code_if_unused(vm) {
            return;
        }
        // NB: The interpreter tries again when it next considers compiling the executable.
        self.head
            .tier_up_budget
            .set(super::tier_up::initial_tier_up_budget(&vm.jit));
    }

    /// Drops the JIT code of an executable whose code was discarded if no live frame of the executable is run by JIT
    /// code. Returns whether it did.
    ///
    /// NB: An activation of JIT code runs a frame of the code's executable, which stays live (where the garbage
    ///     collector finds it) and marked as run by JIT code for as long as the activation runs. JIT code saves nothing
    ///     else that refers to its code: exits find their code through the frame's executable, which keeps it attached
    ///     until then.
    pub fn detach_discarded_jit_code_if_unused(&self, vm: &Vm) -> bool {
        assert_eq!(self.jit_compile_state(), CompileState::Discarding);
        let head = core::ptr::from_ref(&self.head);
        let mut may_run = false;
        vm.for_each_live_execution_context(|frame| {
            may_run = frame.runs_jit_code.get()
                && frame
                    .executable
                    .get()
                    .is_some_and(|executable| core::ptr::eq(executable.as_ptr(), head));
            if may_run {
                core::ops::ControlFlow::Break(())
            } else {
                core::ops::ControlFlow::Continue(())
            }
        });
        if may_run {
            return false;
        }
        self.detach_jit_code(vm);
        true
    }

    /// Drops the executable's discarded code, which no frame runs anymore.
    fn detach_jit_code(&self, vm: &Vm) {
        let state = self.jit_state();
        let (table, slot) = self.jit_entry_table_slot().expect("the code is attached");
        drop(table.set_code(slot, None));
        vm.jit.count_coverage("discard");
        let discard_count = state.discard_count.get() + 1;
        state.discard_count.set(discard_count);
        if vm.jit.options.log_exits {
            eprintln!(
                "JIT discard: {}, discard {discard_count}",
                super::describe_executable(self)
            );
        }
        if discard_count >= MAX_DISCARD_COUNT {
            super::tier_up::refuse_jit_compile(vm, self);
            return;
        }
        self.set_jit_compile_state(CompileState::None);
        self.head
            .tier_up_budget
            .set(super::tier_up::initial_tier_up_budget(&vm.jit));
    }
}
