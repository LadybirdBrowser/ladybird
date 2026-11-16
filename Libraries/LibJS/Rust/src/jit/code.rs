/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Installed JIT code and the JIT state of an executable.

use core::cell::{Cell, OnceCell, RefCell};
use core::ffi::c_void;
use core::ops::ControlFlow;
use std::collections::{HashMap, HashSet};

use ak::Zone;
use libjs_jit::artifact::{Artifact, FrameView, Layout, Patch, SiteView};
use libjs_jit::bytecode::{EnvironmentCoordinate, decode_instruction};
use libjs_jit::code::ExitKind;
pub use libjs_jit::code::INLINED_CALL_SITE_BIT;
use libjs_runtime_macros::Trace;

use super::allocation::SampleSlots;
use super::data_memory::DataMemory;
use super::entry_table::{JIT_ENTRY_SLOT_MASK, JitEntryTable, NO_JIT_ENTRY_SLOT};
use super::executable_memory::ExecutableMemory;
use crate::bytecode::executable::{Executable, PropertyLookupCache, PropertyLookupCacheTier};
use crate::gc::defer_gc::DeferGc;
use crate::gc::sweep::Sweep;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::execution_context::ExecutionContext;
use crate::layout::function_object::EcmascriptFunctionObject;
use crate::layout::object::Object;
use crate::layout::property_lookup_cache::PropertyLookupCacheEntryType;
use crate::layout::realm::Realm;
use crate::layout::shape::Shape;
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
#[cfg(not(all(target_arch = "x86_64", windows)))]
pub type JitEntry = unsafe extern "C" fn(*const Vm, *mut ExecutionContext) -> JitResult;
#[cfg(all(target_arch = "x86_64", windows))]
pub type JitEntry = unsafe extern "sysv64" fn(*const Vm, *mut ExecutionContext) -> JitResult;

/// `JitResult trampoline(VM*, ExecutionContext* frame, JitEntry entry)`: how native code calls JIT code (see
/// `libjs_jit::codegen::generate_entry_trampoline()`), which expects the VM in its pinned register.
#[cfg(not(all(target_arch = "x86_64", windows)))]
pub type EntryTrampoline = unsafe extern "C" fn(*const Vm, *mut ExecutionContext, JitEntry) -> JitResult;
// NB: JIT code uses the System V calling convention on Windows too. Native code that calls it with that convention
//     keeps the registers that are callee-saved only on Windows, and gets the result in two registers.
#[cfg(all(target_arch = "x86_64", windows))]
pub type EntryTrampoline = unsafe extern "sysv64" fn(*const Vm, *mut ExecutionContext, JitEntry) -> JitResult;

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
#[derive(Clone, Trace)]
pub struct SnapshotExecutable {
    pub executable: Gc<Executable>,
    pub function: Option<InlinedFunction>,
    /// How many sites where JIT code exited the executable had recorded when the code was compiled, which the code
    /// knew of (see `Executable::add_jit_exit_site()`). The sites are only ever appended to.
    #[gc(untraced)]
    pub known_exit_sites: usize,
    /// For the compiled executable, how many sites in inlined builtins it had recorded then.
    #[gc(untraced)]
    pub known_builtin_exit_sites: usize,
    /// The size of each property lookup cache of the executable when the code was compiled (see
    /// `speculated_cache_size()`): how many shapes the code speculated on there.
    #[gc(untraced)]
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

/// The property lookup cache that the instruction at `pc` of `bytecode` speculates on (see
/// `Instruction::speculated_property_lookup_cache()`), if it has one.
fn speculated_property_lookup_cache_index(bytecode: &[u8], pc: u32) -> Option<u32> {
    // NB: A zone takes no memory before it gives some out, and it gives out memory only for the array of an
    //     instruction, which no instruction that speculates on a property lookup cache has.
    let zone = Zone::new(DECODED_ARRAY_LIMIT, c"LibJS decoded instruction");
    decode_instruction(bytecode, pc, &zone)
        .ok()?
        .instruction
        .speculated_property_lookup_cache()
}

/// The most memory the array of one decoded instruction takes.
const DECODED_ARRAY_LIMIT: usize = 1 << 30;

/// JIT code that was detached from its executable and waits to be dropped (see
/// `JitState::parking_for_detached_code()`).
#[allow(clippy::vec_box, reason = "the code moves over from the JIT entry table in its box")]
pub type DetachedJitCode = Vec<Box<JitCode>>;

/// The function object of an inlined executable, which its materialized frames run.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Trace)]
pub enum InlinedFunction {
    Ecmascript(Gc<EcmascriptFunctionObject>),
    /// A builtin written in JavaScript, whose frames are set up like those of other builtins.
    Builtin(Gc<NativeJavaScriptBackedFunction>),
}

/// A word of the tables of installed code (see `libjs_jit::artifact`), which are in memory that the tables of other
/// code share (see `DataMemory`).
pub type TableWord = Cell<u32>;

/// A site of installed code.
pub type Site<'a> = SiteView<'a, TableWord>;

/// The interpreter state of one frame at a site of installed code.
pub type FrameState<'a> = FrameView<'a, TableWord>;

/// Installed machine code for one executable, with what exits need to rebuild interpreter frames. While JIT code runs
/// frames of its executable, it stays attached to the executable (in its slot of the JIT entry table, where
/// `libjs_jit_exit()` finds it through the frame's executable), even once discarded.
pub struct JitCode {
    exit_count: Cell<u32>,
    /// The frames of the executable running in the interpreter after the code exited into them or was installed
    /// in them, and how many more tier-up checks each may make at back edges from where it reaches no on-stack
    /// replacement entry before the code is discarded, by frame id (see Vm::frame_id_of()). Unlike frame addresses,
    /// ids are never reused by later frames.
    interpreted_frames: RefCell<HashMap<u64, u32>>,
    /// How many frames `interpreted_frames` holds before forget_inactive_interpreted_frames() looks for returned ones.
    interpreted_frame_count_to_forget_at: Cell<usize>,
    memory: ExecutableMemory,
    entry: JitEntry,
    /// The tables of the code that the runtime reads once the code is installed, which have `layout`.
    tables: DataMemory,
    layout: Layout,
    /// The cells the code compares against or uses, which keep their addresses for as long as the code lives. While a
    /// frame runs the code, the VM keeps them alive. Otherwise the code keeps a realm, a shape or an object alive only
    /// while that realm is otherwise alive (see `realm_of_embedded_cell()`), so that the code of one realm does not
    /// keep another realm that went away alive through the shapes of its objects. The code is dropped once one of
    /// them dies (see `Executable::drop_jit_code_that_lost_embedded_cells()`). The inlined functions of
    /// `snapshot_executables` are held the same way.
    embedded_cells: Vec<Gc<CellHeader>>,
    /// Cells that belong to an ECMAScript function among the embedded cells, with that function: its environments,
    /// which the code embeds too, and its shared data and executable if the code embeds them. The code keeps such a
    /// cell alive only while the function is alive, as these cells may reach the function's realm, but have no realm
    /// of their own (see `split_function_cells()`).
    function_cells: FunctionCells,
    snapshot_executables: Vec<SnapshotExecutable>,
    /// The slots of the samples the code copies closures of, whose addresses the code holds rather than those of the
    /// samples. A collection rewrites them where they are when a sample moves.
    sample_slots: SampleSlots,
    /// Identifies the code among all code the VM installed, for the dependencies it registers (see
    /// `super::dependencies`).
    id: u64,
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
/// (see `libjs_jit::artifact::ObjectView`).
struct MaterializedObject {
    frame: *const ExecutionContext,
    allocation: u32,
    object: Gc<Object>,
}

/// The entry point at `offset` of the code in `memory`.
fn entry_at(memory: &ExecutableMemory, offset: u32) -> JitEntry {
    assert!((offset as usize) < memory.size());
    // SAFETY: The code has an entry point with the JitEntry calling convention at the offset.
    unsafe { core::mem::transmute::<*const u8, JitEntry>(memory.address().add(offset as usize)) }
}

impl JitCode {
    /// Code in `memory` with its entry point at `entry_offset`, and with `tables`, which have `layout`.
    pub fn new(
        memory: ExecutableMemory,
        entry_offset: u32,
        tables: DataMemory,
        layout: Layout,
        embedded_cells: Vec<Gc<CellHeader>>,
        snapshot_executables: Vec<SnapshotExecutable>,
        defer_gc: &DeferGc<'_>,
    ) -> Self {
        let entry = entry_at(&memory, entry_offset);
        let (embedded_cells, function_cells) = split_function_cells(embedded_cells, defer_gc);
        Self {
            exit_count: Cell::new(0),
            interpreted_frames: RefCell::new(HashMap::new()),
            interpreted_frame_count_to_forget_at: Cell::new(MIN_INTERPRETED_FRAME_COUNT_TO_FORGET_AT),
            memory,
            entry,
            tables,
            layout,
            embedded_cells,
            function_cells,
            snapshot_executables,
            sample_slots: Vec::new(),
            id: 0,
            invalidated: Cell::new(false),
            site_index_frame_offset: 0,
            register_dump_frame_offset: 0,
            materialized_objects: RefCell::new(Vec::new()),
        }
    }

    fn artifact(&self) -> Artifact<'_, TableWord> {
        Artifact::new(self.tables.words(), &self.layout)
    }

    /// Gives the code where its frames keep the index of a site and the register dump the runtime reads.
    pub fn set_frame_offsets(&mut self, site_index_frame_offset: i32, register_dump_frame_offset: i32) {
        self.site_index_frame_offset = site_index_frame_offset;
        self.register_dump_frame_offset = register_dump_frame_offset;
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

    /// Gives the code its id among all code the VM installed.
    pub fn set_id(&mut self, id: u64) {
        self.id = id;
    }

    /// Gives the code the slots of the samples it copies closures of.
    pub fn set_sample_slots(&mut self, sample_slots: SampleSlots, _defer_gc: &DeferGc<'_>) {
        self.sample_slots = sample_slots;
    }

    /// Whether the handler whose `Catch` is at `pc` has no catch entry only because it never caught an exception
    /// before the compile, so that a compile now would give it one.
    pub fn compiled_before_handler_caught(&self, pc: u32) -> bool {
        self.artifact().uncaught_handlers().contains(pc)
    }

    pub fn id(&self) -> u64 {
        self.id
    }

    /// The embedded cells and the inlined functions, which the code holds only while their realms are alive (see
    /// `JitCode::embedded_cells`), without the cells of functions.
    fn cells_held_by_realm(&self) -> impl Iterator<Item = Gc<CellHeader>> {
        let functions = self
            .snapshot_executables
            .iter()
            .filter_map(|snapshot| snapshot.function)
            .map(|function| match function {
                InlinedFunction::Ecmascript(function) => function.upcast(),
                InlinedFunction::Builtin(function) => function.upcast(),
            });
        self.embedded_cells.iter().copied().chain(functions)
    }

    /// Keeps the embedded cells and the inlined functions alive, for code that a frame runs.
    fn trace_embedded_cells(&self, visitor: &mut Visitor) {
        for cell in self.cells_held_by_realm() {
            visitor.visit(cell);
        }
        for &(_, cell) in &self.function_cells {
            visitor.visit(cell);
        }
    }

    /// Whether a cell the code embeds or an inlined function died in the collection.
    fn lost_embedded_cell(&self, sweep: &Sweep<'_>) -> bool {
        self.cells_held_by_realm().any(|cell| sweep.is_dead(cell))
            || self.function_cells.iter().any(|&(_, cell)| sweep.is_dead(cell))
            || self
                .snapshot_executables
                .iter()
                .any(|snapshot| sweep.is_dead(snapshot.executable))
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
        // NB: The patches are jumps to the exits of the `AssumeValid` nodes of the code, which are no-ops until then.
        let patches = self.artifact().invalidation_patches().collect::<Vec<Patch>>();
        self.memory.patch(&patches);
    }

    pub fn entry(&self) -> JitEntry {
        self.entry
    }

    /// The entry at `pc` for a frame whose memory holds every value live there: an on-stack replacement entry at a
    /// loop back edge, the resume entry at a continuation of a generator's suspension, or the catch entry after the
    /// `Catch` of an exception handler.
    pub fn osr_entry(&self, pc: u32) -> Option<JitEntry> {
        self.artifact()
            .osr_entry(pc)
            .map(|offset| entry_at(&self.memory, offset))
    }

    /// Whether a frame running in the interpreter at the loop back edge `pc` reaches an on-stack replacement entry
    /// without leaving the loop it is in.
    pub fn reaches_osr_entry_from(&self, pc: u32) -> bool {
        self.artifact().osr_loop_back_edges().contains(pc)
    }

    /// Whether a frame running in the interpreter at the loop back edge `pc` may reach an on-stack replacement entry
    /// on a later iteration of a loop it is in.
    pub fn may_reach_osr_entry_from(&self, pc: u32) -> bool {
        self.artifact().osr_outer_loop_back_edges().contains(pc)
    }

    pub fn site(&self, index: u32) -> Site<'_> {
        self.artifact().site(index)
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

    /// Notes that `frame`, a frame of the code's executable, runs in the interpreter after the code exited into it or
    /// was installed in it, and may make `checks` tier-up checks at back edges from where it reaches no on-stack
    /// replacement entry before the code is discarded.
    pub fn note_interpreted_frame(&self, vm: &Vm, frame: &ExecutionContext, checks: u32) {
        self.interpreted_frames
            .borrow_mut()
            .insert(vm.frame_id_of(frame), checks);
    }

    /// Forgets frames that returned, so that a long-lived callee's code cannot accumulate the ids of frames
    /// materialized by exits from many different inlined callers. Finding the live frames walks the stack, so it
    /// only happens once the code remembers twice as many frames as were live the last time: unwinding a deep
    /// recursion exits in every frame, and a walk at each exit made that quadratic in the depth.
    pub fn forget_inactive_interpreted_frames(&self, vm: &Vm) {
        let mut frames = self.interpreted_frames.borrow_mut();
        if frames.len() < self.interpreted_frame_count_to_forget_at.get() {
            return;
        }
        let mut live_frames = HashMap::new();
        vm.for_each_live_execution_context(|frame| {
            if let Some(checks) = frames.remove(&frame.frame_id.get()) {
                live_frames.insert(frame.frame_id.get(), checks);
            }
            if frames.is_empty() {
                ControlFlow::Break(())
            } else {
                ControlFlow::Continue(())
            }
        });
        *frames = live_frames;
        self.interpreted_frame_count_to_forget_at
            .set((frames.len() * 2).max(MIN_INTERPRETED_FRAME_COUNT_TO_FORGET_AT));
    }

    /// Counts a tier-up check of `frame` at a back edge from where it reaches no on-stack replacement entry, and
    /// returns whether the frame made all it may. Other frames that run in the interpreter finish there like before.
    pub fn count_fruitless_loop_check(&self, frame: &ExecutionContext) -> bool {
        let mut frames = self.interpreted_frames.borrow_mut();
        // NB: Frames without an id were never noted.
        let Some(left) = frames.get_mut(&frame.frame_id.get()) else {
            return false;
        };
        *left = left.saturating_sub(1);
        *left == 0
    }
}

/// Cells that belong to ECMAScript functions, each with its function (see `JitCode::function_cells`).
type FunctionCells = Vec<(Gc<CellHeader>, Gc<CellHeader>)>;

/// Splits the cells that belong to an ECMAScript function among `embedded_cells` off them (see
/// `JitCode::function_cells`), returning the other embedded cells and those cells, with their functions.
fn split_function_cells(
    mut embedded_cells: Vec<Gc<CellHeader>>,
    _defer_gc: &DeferGc<'_>,
) -> (Vec<Gc<CellHeader>>, FunctionCells) {
    let embedded: HashSet<usize> = embedded_cells.iter().map(|cell| cell.as_ptr().addr()).collect();
    let mut function_cells = Vec::new();
    for function in embedded_cells
        .iter()
        .filter_map(|cell| cell.downcast::<EcmascriptFunctionObject>())
    {
        // NB: Code that embeds an ECMAScript function may also embed the words its frames start with (see
        //     `FunctionFrameFields`), whose environment the code then keeps from moving like the cells it lists.
        let environment = function.environment().map(Gc::upcast);
        let shared_data = function.shared_data();
        let embedded_code = [
            Some(shared_data.upcast::<CellHeader>()),
            shared_data.executable().map(Gc::upcast),
        ]
        .into_iter()
        .flatten()
        .filter(|cell| embedded.contains(&cell.as_ptr().addr()));
        for cell in environment.into_iter().chain(embedded_code) {
            function_cells.push((function.upcast(), cell));
        }
    }
    let function_owned: HashSet<usize> = function_cells.iter().map(|&(_, cell)| cell.as_ptr().addr()).collect();
    embedded_cells.retain(|cell| !function_owned.contains(&cell.as_ptr().addr()));
    (embedded_cells, function_cells)
}

/// The realm of a realm, a shape or an object, which code that embeds the cell keeps alive only while the realm is
/// alive. Other cells, such as executables, strings and environments, belong to no realm, and the code keeps them
/// alive.
fn realm_of_embedded_cell(cell: Gc<CellHeader>) -> Option<Gc<Realm>> {
    if let Some(realm) = cell.downcast::<Realm>() {
        return Some(realm);
    }
    if let Some(shape) = cell.downcast::<Shape>() {
        return Some(shape.realm());
    }
    cell.downcast::<Object>().map(|object| object.shape().realm())
}

// SAFETY: Visits what the code keeps alive. The realms, shapes and objects it embeds and its inlined functions stay
// where they are, and are kept alive only while their realms are. The code is dropped before any of them that died is
// swept (see `JitCode::embedded_cells`).
unsafe impl Trace for JitCode {
    fn trace(&self, visitor: &mut Visitor) {
        for cell in self.cells_held_by_realm() {
            match realm_of_embedded_cell(cell) {
                Some(realm) => visitor.visit_ephemeron_cell(realm, cell),
                None => visitor.visit(cell),
            }
        }
        for &(function, cell) in &self.function_cells {
            visitor.visit_ephemeron_cell(function, cell);
        }
        for materialized in self.materialized_objects.borrow().iter() {
            visitor.visit(materialized.object);
        }
        // NB: The executables of inlined calls are held like the cells of their functions.
        for snapshot in &self.snapshot_executables {
            match snapshot.function {
                Some(InlinedFunction::Ecmascript(function)) => {
                    visitor.visit_ephemeron_cell(function, snapshot.executable);
                }
                Some(InlinedFunction::Builtin(function)) => {
                    visitor.visit_ephemeron_cell(function, snapshot.executable);
                }
                None => visitor.visit(snapshot.executable),
            }
        }
        for slot in &self.sample_slots {
            // SAFETY: Nothing writes the slots, and only dropping the code frees them, which a sweep callback defers until
            //         the collection is over if it moves cells (see `Executable::drop_jit_code_that_lost_embedded_cells()`).
            unsafe { visitor.visit_owned_values(core::slice::from_ref(&**slot)) };
        }
    }
}

/// A reason to discard JIT code that says nothing about the code.
#[derive(Clone, Copy)]
enum UncountedDiscard {
    /// The executable idled (see `Executable::flush_idle_jit_code()`).
    IdleFlush,
    /// A cell the code embeds died (see `Executable::drop_jit_code_that_lost_embedded_cells()`).
    LostEmbeddedCell,
}

impl UncountedDiscard {
    fn coverage_key(self) -> &'static str {
        match self {
            Self::IdleFlush => "flush",
            Self::LostEmbeddedCell => "lost-embedded-cell",
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
    /// Why the discarded code is discarded, if for a reason that says nothing about the code, which does not count as
    /// a discard.
    uncounted_discard: Cell<Option<UncountedDiscard>>,
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

/// After this many tier-up checks of the frame that JIT code last exited into, at back edges from where it reaches no
/// on-stack replacement entry, the code is discarded even if it exited fewer times: a long call that exited once into
/// a loop that never ran before the compile (like a driver running one loop after another) would otherwise run the
/// rest of the loop in the interpreter.
pub const FRUITLESS_LOOP_CHECKS_AFTER_EXIT: u32 = 8;

/// Like FRUITLESS_LOOP_CHECKS_AFTER_EXIT, for the frame whose tier-up check installed the code. It is higher: a frame
/// that moved on from the loop its compile was for may soon return, and later calls enter the code at its start
/// rather than at a loop nested in others, where the code optimizes less.
pub const FRUITLESS_LOOP_CHECKS_AFTER_INSTALL: u32 = 256;

/// How many frames JIT code remembers running in the interpreter at least before it looks for returned ones to forget.
const MIN_INTERPRETED_FRAME_COUNT_TO_FORGET_AT: usize = 64;

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
    pub fn free_jit_entry_slot(&self, vm: &Vm, sweep: &Sweep<'_>) {
        let Some(table) = vm.jit.entry_table.as_ref() else {
            return;
        };
        let Some(code) = table.free(
            self.head.jit_entry_slot.get(),
            core::ptr::from_ref(&self.head) as u64,
            sweep,
        ) else {
            return;
        };
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

    /// Keeps the cells that the executable's attached JIT code embeds alive, for an executable that a frame runs in JIT
    /// code (see `JitCode::embedded_cells`).
    pub fn trace_embedded_cells_of_jit_code(&self, vm: &Vm, visitor: &mut Visitor) {
        if let Some(code) = self.jit_code(vm) {
            code.trace_embedded_cells(visitor);
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
        let index = speculated_property_lookup_cache_index(self.bytecode(), pc)?;
        self.property_lookup_caches().get(index as usize)
    }

    /// Notes that JIT code compiled when the property lookup caches of the executable had `cache_sizes` exited with
    /// `ExitKind::BadShape` at `pc`. Returns whether that code speculated on more shapes there than the code that
    /// exited there before, so that a recompile learns from the exit.
    pub fn note_jit_shape_exit(&self, pc: u32, cache_sizes: &[u8]) -> bool {
        let Some(size) = speculated_property_lookup_cache_index(self.bytecode(), pc)
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
    pub fn install_jit_code(&self, vm: &Vm, code: JitCode, tier_up_budget: i32, defer_gc: &DeferGc<'_>) {
        let (table, slot) = self
            .jit_entry_table_slot(vm)
            .expect("only executables with a JIT entry slot are compiled");
        table.set_entry(slot, Some(code.entry() as *const c_void));
        table.attach_code(slot, code, defer_gc);
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

    /// Drops the JIT code of an executable that idled long enough (see `bytecode::flushing`), like discard_jit_code()
    /// but without counting a discard: it says nothing about the code. The executable has to get hot again to be
    /// compiled again.
    pub fn flush_idle_jit_code(&self, vm: &Vm) {
        self.discard_jit_code_without_counting(vm, UncountedDiscard::IdleFlush);
    }

    /// Discards the executable's JIT code like discard_jit_code(), without counting a discard. The executable has to
    /// get hot again to be compiled again.
    fn discard_jit_code_without_counting(&self, vm: &Vm, reason: UncountedDiscard) {
        self.jit_state().uncounted_discard.set(Some(reason));
        self.discard_jit_code(vm);
        if self.jit_compile_state() == CompileState::Discarding {
            self.head
                .tier_up_budget
                .set(super::tier_up::initial_tier_up_budget(&vm.jit));
        }
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

    /// Drops the executable's JIT code if a cell it embeds died in the collection whose sweep callback calls this. No
    /// frame runs such code, as the VM keeps the embedded cells of code that frames run alive, so it is dropped before
    /// the dead cell is swept and its memory reused.
    pub fn drop_jit_code_that_lost_embedded_cells(&self, vm: &Vm, sweep: &Sweep<'_>) {
        if !self.jit_code(vm).is_some_and(|code| code.lost_embedded_cell(sweep)) {
            return;
        }
        // NB: A collection that moves cells may still rewrite the slots that the code's trace handed it (see
        //     `JitCode::sample_slots`), so the code is only dropped once the collection is over.
        let _deferral = sweep
            .moves_cells()
            .then(|| vm.jit.defer_code_drops_until_collection_is_over(vm));
        match self.jit_compile_state() {
            CompileState::Installed => self.discard_jit_code_without_counting(vm, UncountedDiscard::LostEmbeddedCell),
            CompileState::Discarding => {
                self.detach_discarded_jit_code_if_unused(vm);
            }
            state => unreachable!("only installed or discarded code is attached, not in state {state:?}"),
        }
        assert!(
            self.jit_code(vm).is_none(),
            "no frame runs code that lost an embedded cell"
        );
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
        table.detach_code(slot, vm.jit.parking_for_detached_code());
        if let Some(reason) = state.uncounted_discard.take()
            && !state.refused_once_detached.get()
        {
            vm.jit.count_coverage(reason.coverage_key());
            self.set_jit_compile_state(CompileState::None);
            self.head
                .tier_up_budget
                .set(super::tier_up::initial_tier_up_budget(&vm.jit));
            return;
        }
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
