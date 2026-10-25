/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Installed JIT code and the JIT state of an executable.

use core::cell::{Cell, OnceCell, RefCell};
use core::ffi::c_void;

use libjs_jit::bytecode::{EnvironmentCoordinate, decode_instruction};
pub use libjs_jit::code::INLINED_CALL_SITE_BIT;
use libjs_jit::code::{ExitKind, Site};

use super::entry_table::{JIT_ENTRY_SLOT_MASK, JitEntryTable, NO_JIT_ENTRY_SLOT};
use super::executable_memory::ExecutableMemory;
use crate::bytecode::executable::{Executable, PropertyLookupCache, PropertyLookupCacheTier};
use crate::gc::defer_gc::DeferGc;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::execution_context::ExecutionContext;
use crate::layout::function_object::EcmascriptFunctionObject;
use crate::layout::object::Object;
use crate::layout::property_lookup_cache::PropertyLookupCacheEntryType;
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
    /// A compile job for the executable is running on a compile thread.
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
#[derive(Clone)]
pub struct SnapshotExecutable {
    pub executable: Gc<Executable>,
    pub function: Option<InlinedFunction>,
    /// How many sites where JIT code exited the executable had recorded when the code was compiled, which the code
    /// knew of (see `Executable::add_jit_exit_site()`). The sites are only ever appended to.
    pub known_exit_sites: usize,
    /// For the compiled executable, how many sites in inlined builtins it had recorded then.
    pub known_builtin_exit_sites: usize,
    /// The size of each property lookup cache of the executable when the code was compiled (see
    /// `speculated_cache_size()`): how many shapes the code speculated on there.
    pub property_cache_sizes: Box<[u8]>,
}

/// How many entries of a property lookup cache compiled code speculates on, or `u8::MAX` once the cache is megamorphic,
/// so that it no longer grows.
pub fn speculated_cache_size(cache: &PropertyLookupCache) -> u8 {
    if cache.tier() == PropertyLookupCacheTier::Megamorphic {
        return u8::MAX;
    }
    let entries = cache
        .entries()
        .iter()
        .filter(|entry| entry.get().entry_type != PropertyLookupCacheEntryType::Empty)
        .count();
    u8::try_from(entries).expect("caches that are not megamorphic have few entries")
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
    /// Where a frame running in the interpreter at `pc` can continue in this code (see `osr_entry()`).
    osr_entries: Vec<(u32, JitEntry)>,
    /// The loop back edges from where a frame running in the interpreter reaches one of the `osr_entries` without
    /// leaving the loop it is in.
    osr_loop_back_edges: Vec<u32>,
    /// The loop back edges in the loops around those of the `osr_entries`, from where a frame running in the
    /// interpreter may reach one of them on a later iteration.
    osr_outer_loop_back_edges: Vec<u32>,
    /// The pcs of the `Catch` of the exception handlers that have no catch entry only because they never caught an
    /// exception before the compile.
    uncaught_handlers: Vec<u32>,
    /// Identifies the code among all code the VM installed, for the dependencies it registers (see
    /// `super::dependencies`).
    id: u64,
    /// The bytes that invalidate the code at each offset of it: jumps to the exits of its `AssumeValid` nodes, which
    /// are no-ops until then.
    invalidation_patches: Vec<(u32, Vec<u8>)>,
    invalidated: Cell<bool>,
    /// See `libjs_jit::code::CompiledCode::site_index_frame_offset`.
    site_index_frame_offset: i32,
    /// See `libjs_jit::code::CompiledCode::register_dump_frame_offset`.
    register_dump_frame_offset: i32,
    /// The objects the code never allocated that the runtime created for frames running the code while they were
    /// suspended in a call, because code the call ran read them through the legacy `arguments` of a frame of an
    /// inlined call (see `translate::resolve_deferred_arguments()`). Those frames continue in the interpreter with
    /// these objects, which only they use. Exits and leaves forget a frame's objects; a frame an exception unwinds
    /// past its compiled code leaves its objects here until the code detaches. Invalidated code is entered no more,
    /// so no other frame that runs it can come to have the address of one of them.
    materialized_objects: RefCell<Vec<MaterializedObject>>,
}

/// An object created for a frame running JIT code, standing for the object its code never allocated at `allocation`
/// (see `libjs_jit::code::VirtualObjectDescriptor`).
struct MaterializedObject {
    frame: *const ExecutionContext,
    allocation: u32,
    object: Gc<Object>,
}

impl JitCode {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        memory: ExecutableMemory,
        entry_offset: u32,
        osr_entry_offsets: &[(u32, u32)],
        osr_loop_back_edges: Vec<u32>,
        sites: Vec<Site>,
        embedded_cells: Vec<Gc<CellHeader>>,
        snapshot_executables: Vec<SnapshotExecutable>,
        _defer_gc: &DeferGc<'_>,
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
            osr_loop_back_edges,
            osr_outer_loop_back_edges: Vec::new(),
            uncaught_handlers: Vec::new(),
            id: 0,
            invalidation_patches: Vec::new(),
            invalidated: Cell::new(false),
            site_index_frame_offset: 0,
            register_dump_frame_offset: 0,
            materialized_objects: RefCell::new(Vec::new()),
        }
    }

    /// Gives the code where its frames keep the index of a site and the register dump the runtime reads.
    pub fn with_frame_offsets(mut self, site_index_frame_offset: i32, register_dump_frame_offset: i32) -> Self {
        self.site_index_frame_offset = site_index_frame_offset;
        self.register_dump_frame_offset = register_dump_frame_offset;
        self
    }

    pub fn register_dump_frame_offset(&self) -> i32 {
        self.register_dump_frame_offset
    }

    /// The index of the site whose values the runtime reads from the JIT frame at `frame_pointer` (see
    /// `libjs_jit::code::CompiledCode::site_index_frame_offset`).
    pub fn site_index_of_frame(&self, frame_pointer: u64) -> u32 {
        let address = frame_pointer.wrapping_add_signed(i64::from(self.site_index_frame_offset)) as *const u32;
        // SAFETY: The JIT frame is on the stack, suspended in a slow path or call that stored the index there.
        unsafe { address.read() }
    }

    /// Stores `index` as the index of the site whose values the runtime reads from the JIT frame at `frame_pointer`.
    pub fn set_site_index_of_frame(&self, frame_pointer: u64, index: u32) {
        let address = frame_pointer.wrapping_add_signed(i64::from(self.site_index_frame_offset)) as *mut u32;
        // SAFETY: The JIT frame is on the stack, suspended in a call, and the code reads the word only where it
        //         stored it.
        unsafe { address.write(index) }
    }

    /// The object created for `frame` that stands for the object its code never allocated at `allocation`, if any.
    pub fn materialized_object(&self, frame: *const ExecutionContext, allocation: u32) -> Option<Gc<Object>> {
        self.materialized_objects
            .borrow()
            .iter()
            .find(|materialized| materialized.frame == frame && materialized.allocation == allocation)
            .map(|materialized| materialized.object)
    }

    pub fn remember_materialized_object(&self, frame: *const ExecutionContext, allocation: u32, object: Gc<Object>) {
        self.materialized_objects.borrow_mut().push(MaterializedObject {
            frame,
            allocation,
            object,
        });
    }

    /// Forgets the objects created for `frame`, which no longer runs the code.
    pub fn forget_materialized_objects(&self, frame: *const ExecutionContext) {
        self.materialized_objects
            .borrow_mut()
            .retain(|materialized| materialized.frame != frame);
    }

    /// Gives the code its id among all code the VM installed, and the bytes that invalidate it.
    pub fn with_invalidation(mut self, id: u64, invalidation_patches: Vec<(u32, Vec<u8>)>) -> Self {
        self.id = id;
        self.invalidation_patches = invalidation_patches;
        self
    }

    /// Gives the code the loop back edges in the loops around those of its on-stack replacement entries.
    pub fn with_osr_outer_loop_back_edges(mut self, osr_outer_loop_back_edges: Vec<u32>) -> Self {
        self.osr_outer_loop_back_edges = osr_outer_loop_back_edges;
        self
    }

    pub fn with_uncaught_handlers(mut self, uncaught_handlers: Vec<u32>) -> Self {
        self.uncaught_handlers = uncaught_handlers;
        self
    }

    /// Whether the handler whose `Catch` is at `pc` has no catch entry only because it never caught an exception
    /// before the compile, so that a compile now would give it one.
    pub fn compiled_before_handler_caught(&self, pc: u32) -> bool {
        self.uncaught_handlers.contains(&pc)
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

    /// The entry at `pc` for a frame whose memory holds every value live there: an on-stack replacement entry at a
    /// loop back edge, the resume entry at a continuation of a generator's suspension, or the catch entry after the
    /// `Catch` of an exception handler.
    pub fn osr_entry(&self, pc: u32) -> Option<JitEntry> {
        self.osr_entries
            .iter()
            .find(|(entry_pc, _)| *entry_pc == pc)
            .map(|(_, entry)| *entry)
    }

    /// Whether a frame running in the interpreter at the loop back edge `pc` reaches an on-stack replacement entry
    /// without leaving the loop it is in.
    pub fn reaches_osr_entry_from(&self, pc: u32) -> bool {
        self.osr_loop_back_edges.contains(&pc)
    }

    /// Whether a frame running in the interpreter at the loop back edge `pc` may reach an on-stack replacement entry
    /// on a later iteration of a loop it is in.
    pub fn may_reach_osr_entry_from(&self, pc: u32) -> bool {
        self.osr_outer_loop_back_edges.contains(&pc)
    }

    pub fn site(&self, index: u32) -> &Site {
        &self.sites[index as usize]
    }

    /// Takes the sites of code that died, so that they can be dropped elsewhere.
    pub fn take_sites(&mut self) -> Vec<Site> {
        core::mem::take(&mut self.sites)
    }

    pub fn snapshot_executable(&self, index: u32) -> &SnapshotExecutable {
        &self.snapshot_executables[index as usize]
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
        for materialized in self.materialized_objects.borrow().iter() {
            visitor.visit(materialized.object);
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
    /// Places where JIT code exited. Recompiles never repeat a speculation of the same kind at the same pc, but for
    /// shapes of a property lookup cache that learned more of them since (see `jit_exit_sites_for_recompile()`).
    exit_sites: RefCell<Vec<(u32, ExitKind)>>,
    /// For the pcs of `exit_sites` where JIT code exited with `ExitKind::BadShape` at an instruction whose property
    /// lookup cache it speculated on: the size the cache had when the last code that exited there was compiled.
    shape_exit_cache_sizes: RefCell<Vec<(u32, u8)>>,
    /// Places in builtins written in JavaScript where the executable's JIT code exited while running them inlined,
    /// by the address of the builtin's executable. Builtins are inlined into all their callers, so these are kept per
    /// caller: what failed in one caller says nothing about the others.
    builtin_exit_sites: RefCell<Vec<(u64, u32, ExitKind)>>,
    /// How often the executable's code was discarded.
    discard_count: Cell<u32>,
    /// How often in a row the executable's code was discarded without its exits having been at a site the code did not
    /// know of, so that a recompile would learn nothing from them.
    fruitless_discard_count: Cell<u32>,
    /// Whether exits of the executable's code recorded a site it did not know of since its code was last discarded.
    learned_exit_site: Cell<bool>,
    /// Whether the executable is refused once its discarded code is detached, because it was refused while frames
    /// still ran that code (see `refuse_jit_compile_once_detached()`).
    refused_once_detached: Cell<bool>,
    /// The loads and stores of bindings of the environments of the executable's closure (see `jit::snapshot`). The
    /// bytecode never changes, so snapshots find them once.
    pub(crate) captured_binding_accesses: OnceCell<CapturedBindingAccesses>,
}

/// The loads and stores of bindings of the environments of an executable's closure, which are none if its code may
/// change its lexical environment.
#[derive(Default)]
pub(crate) struct CapturedBindingAccesses {
    /// The coordinate and pc of each load of a binding.
    pub loads: Box<[(EnvironmentCoordinate, u32)]>,
    /// The coordinate of each store to a binding of a lexical environment.
    pub stores: Box<[EnvironmentCoordinate]>,
}

/// After this many exits, JIT code is discarded so that the executable can be recompiled with what the exits taught
/// it.
pub const EXIT_COUNT_BEFORE_DISCARD: u32 = 10;

/// After this many exits, JIT code is discarded sooner, once a frame runs a loop in the interpreter from where it
/// reaches no on-stack replacement entry of the code. Code that exits and leaves frames looping in the interpreter
/// costs more than code that exits as often elsewhere.
pub const EXIT_COUNT_BEFORE_DISCARD_FROM_LOOP: u32 = 5;

/// An executable whose code was discarded this often in a row without its exits teaching it anything is not compiled
/// again. Discards after exits at sites the code did not know of make progress: each site is recorded once, and
/// recompiles no longer speculate there, so code that keeps reaching new paths (like a dispatcher whose program moves
/// on to new phases) keeps being compiled.
pub const MAX_FRUITLESS_DISCARD_COUNT: u32 = 8;

/// An executable whose code was discarded this often, whatever its exits taught it, is not compiled again either: each
/// recompile compiles the whole executable, so code that keeps reaching new sites would otherwise cost compile time
/// without bound.
pub const MAX_DISCARD_COUNT: u32 = 64;

impl Executable {
    pub fn jit_compile_state(&self) -> CompileState {
        CompileState::from_u8(self.head.jit_compile_state.get())
    }

    pub fn set_jit_compile_state(&self, state: CompileState) {
        self.head.jit_compile_state.set(state as u8);
    }

    /// Whether the executable has JIT code, or a compile on its way, which is tied to the executable.
    pub fn has_jit_code_or_compile(&self) -> bool {
        matches!(
            self.jit_compile_state(),
            CompileState::Queued | CompileState::Installed | CompileState::Discarding
        )
    }

    /// Makes the executable never compile, with the tier and the tier-up budget of `other`, if `other` was refused,
    /// as replacing an executable with one of the same bytecode does not change whether it can compile.
    pub fn take_over_jit_refusal_from(&self, other: &Executable) {
        if other.jit_compile_state() != CompileState::Refused {
            return;
        }
        self.set_jit_compile_state(CompileState::Refused);
        self.set_interpreter_tier(other.interpreter_tier());
        self.head.tier_up_budget.set(other.head.tier_up_budget.get());
    }

    /// The executable's slot in the JIT entry table of `vm`, the VM whose heap the executable lives in, and the table,
    /// if it has one.
    fn jit_entry_table_slot<'vm>(&self, vm: &'vm Vm) -> Option<(&'vm JitEntryTable, u32)> {
        let slot = self.head.jit_entry_slot.get();
        if slot & JIT_ENTRY_SLOT_MASK == NO_JIT_ENTRY_SLOT {
            return None;
        }
        let table = vm
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
        let Some(table) = vm.jit.entry_table.as_ref() else {
            return;
        };
        let Some(mut code) = table.free(self.head.jit_entry_slot.get(), core::ptr::from_ref(&self.head) as u64) else {
            return;
        };
        // OPTIMIZATION: The compile threads allocated the sites, so one of them frees them, outside the collection
        //               this runs in.
        if let Some(queue) = vm.jit.started_compile_queue() {
            queue.drop_sites(code.take_sites());
        }
        drop(code);
    }

    /// The executable's attached JIT code, if any. `vm` is the VM whose heap the executable lives in.
    pub fn jit_code<'vm>(&self, vm: &'vm Vm) -> Option<&'vm JitCode> {
        let (table, slot) = self.jit_entry_table_slot(vm)?;
        table.code(slot)
    }

    /// Visits the cells the executable's attached JIT code keeps alive.
    pub fn trace_jit_code(&self, visitor: &mut Visitor) {
        if let Some(code) = self.jit_code(Vm::of_cell(self)) {
            code.trace(visitor);
        }
    }

    pub fn jit_exit_sites(&self) -> Vec<(u32, ExitKind)> {
        self.jit_state().exit_sites.borrow().clone()
    }

    /// The sites where JIT code exited that recompiles do not speculate at again (see `jit_exit_sites()`). A bad shape
    /// at an instruction whose property lookup cache grew since the code that exited there was compiled is left out:
    /// the interpreter added the shapes the code did not know to the cache, so recompiles speculate on them all. Each
    /// such exit needs a larger cache, so this ends once the cache is megamorphic.
    pub fn jit_exit_sites_for_recompile(&self) -> Vec<(u32, ExitKind)> {
        let sizes = self.jit_state().shape_exit_cache_sizes.borrow();
        self.jit_exit_sites()
            .into_iter()
            .filter(|&(pc, kind)| {
                if kind != ExitKind::BadShape {
                    return true;
                }
                let Some(&(_, size)) = sizes.iter().find(|(site, _)| *site == pc) else {
                    return true;
                };
                self.speculated_property_lookup_cache(pc)
                    .is_none_or(|cache| speculated_cache_size(cache) <= size)
            })
            .collect()
    }

    /// The property lookup cache that the instruction at `pc` speculates on (see
    /// `Instruction::speculated_property_lookup_cache()`), if it has one.
    fn speculated_property_lookup_cache(&self, pc: u32) -> Option<&PropertyLookupCache> {
        let decoded = decode_instruction(self.bytecode(), pc).ok()?;
        let index = decoded.instruction.speculated_property_lookup_cache()?;
        self.property_lookup_caches().get(index as usize)
    }

    /// Notes that JIT code compiled when the property lookup caches of the executable had `cache_sizes` exited with
    /// `ExitKind::BadShape` at `pc`. Returns whether that code speculated on more shapes there than the code that
    /// exited there before, so that a recompile learns from the exit.
    pub fn note_jit_shape_exit(&self, pc: u32, cache_sizes: &[u8]) -> bool {
        let Some(size) = decode_instruction(self.bytecode(), pc)
            .ok()
            .and_then(|decoded| decoded.instruction.speculated_property_lookup_cache())
            .and_then(|index| cache_sizes.get(index as usize).copied())
        else {
            return false;
        };
        let mut sizes = self.jit_state().shape_exit_cache_sizes.borrow_mut();
        match sizes.iter_mut().find(|(site, _)| *site == pc) {
            Some((_, recorded)) if *recorded >= size => false,
            Some((_, recorded)) => {
                *recorded = size;
                true
            }
            None => {
                sizes.push((pc, size));
                true
            }
        }
    }

    pub fn jit_exit_site_count(&self) -> usize {
        self.jit_state().exit_sites.borrow().len()
    }

    /// Records a site where JIT code exited, and returns its index among the sites.
    pub fn add_jit_exit_site(&self, site: (u32, ExitKind)) -> usize {
        let mut sites = self.jit_state().exit_sites.borrow_mut();
        if let Some(index) = sites.iter().position(|recorded| *recorded == site) {
            return index;
        }
        sites.push(site);
        sites.len() - 1
    }

    pub fn jit_builtin_exit_sites(&self) -> Vec<(u64, u32, ExitKind)> {
        self.jit_state().builtin_exit_sites.borrow().clone()
    }

    pub fn jit_builtin_exit_site_count(&self) -> usize {
        self.jit_state().builtin_exit_sites.borrow().len()
    }

    /// Records a site where JIT code exited in an inlined builtin, and returns its index among those sites.
    pub fn add_jit_builtin_exit_site(&self, site: (u64, u32, ExitKind)) -> usize {
        let mut sites = self.jit_state().builtin_exit_sites.borrow_mut();
        if let Some(index) = sites.iter().position(|recorded| *recorded == site) {
            return index;
        }
        sites.push(site);
        sites.len() - 1
    }

    pub fn jit_discard_count(&self) -> u32 {
        self.jit_state().discard_count.get()
    }

    /// Notes that an exit of the executable's installed code was at a site the code did not know of, which a
    /// recompile learns from (see `MAX_FRUITLESS_DISCARD_COUNT`).
    pub fn note_learned_jit_exit_site(&self) {
        self.jit_state().learned_exit_site.set(true);
    }

    /// Makes new frames of the executable run in its JIT code. Frames of it that run in the interpreter count down
    /// `tier_up_budget` from here, and continue in the code at an on-stack replacement entry once it runs out.
    pub fn install_jit_code(&self, vm: &Vm, code: JitCode, tier_up_budget: i32) {
        let (table, slot) = self
            .jit_entry_table_slot(vm)
            .expect("only executables with a JIT entry slot are compiled");
        table.set_entry(slot, Some(code.entry() as *const c_void));
        table.attach_code(slot, code);
        self.set_jit_compile_state(CompileState::Installed);
        self.head.tier_up_budget.set(tier_up_budget);
    }

    /// Stops entering the executable's JIT code and drops it once no live frame of the executable is run by JIT code
    /// anymore (see `detach_discarded_jit_code_if_unused()`). The executable may be compiled again later.
    pub fn discard_jit_code(&self, vm: &Vm) {
        assert_eq!(self.jit_compile_state(), CompileState::Installed);
        let (table, slot) = self.jit_entry_table_slot(vm).expect("the code is attached");
        table.set_entry(slot, None);
        self.set_jit_compile_state(CompileState::Discarding);
        if self.detach_discarded_jit_code_if_unused(vm) {
            return;
        }
        // NB: The interpreter tries again when it next considers compiling the executable.
        self.head.tier_up_budget.set(super::tier_up::recompile_budget(&vm.jit));
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

    /// Makes the executable's frames run in the interpreter for good, like `tier_up::refuse_jit_compile()`, once its
    /// discarded code, which frames still run, is detached.
    pub fn refuse_jit_compile_once_detached(&self) {
        assert_eq!(self.jit_compile_state(), CompileState::Discarding);
        self.jit_state().refused_once_detached.set(true);
    }

    /// Drops the executable's discarded code, which no frame runs anymore.
    fn detach_jit_code(&self, vm: &Vm) {
        let state = self.jit_state();
        let (table, slot) = self.jit_entry_table_slot(vm).expect("the code is attached");
        table.detach_code(slot);
        vm.jit.count_coverage("discard");
        let discard_count = state.discard_count.get() + 1;
        state.discard_count.set(discard_count);
        if vm.jit.options.log_exits {
            eprintln!(
                "JIT discard: {}, discard {discard_count}",
                super::describe_executable(self)
            );
        }
        if state.learned_exit_site.replace(false) {
            state.fruitless_discard_count.set(0);
        } else {
            state
                .fruitless_discard_count
                .set(state.fruitless_discard_count.get() + 1);
        }
        if discard_count >= MAX_DISCARD_COUNT
            || state.fruitless_discard_count.get() >= MAX_FRUITLESS_DISCARD_COUNT
            || state.refused_once_detached.get()
        {
            super::tier_up::refuse_jit_compile(vm, self);
            return;
        }
        self.set_jit_compile_state(CompileState::None);
        self.head.tier_up_budget.set(super::tier_up::recompile_budget(&vm.jit));
    }
}
