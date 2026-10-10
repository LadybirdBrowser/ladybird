/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime side of the optimizing JIT: its options, the interpreter tiers executables move through as they get
//! warm, the tier-up policy, and the glue between the runtime and the `libjs_jit` compiler: snapshots of executables
//! for compile jobs, the compile thread, installing the code, and entering and leaving it.

pub mod calls;
pub mod code;
pub mod compile_queue;
pub mod dispatch_tables;
pub mod entry_exit;
pub mod entry_table;
pub mod executable_memory;
pub mod feedback_dump;
pub mod intrinsics;
pub mod runtime_info;
pub mod snapshot;
pub mod testing;
pub mod tier_up;
pub mod translate;

pub use libjs_jit::options;

use core::cell::{Cell, OnceCell, RefCell};
use core::ptr::NonNull;
use std::collections::{BTreeMap, HashMap};
use std::rc::Rc;

use crate::bytecode::executable::Executable;
use crate::gc::capi;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::dispatch_tables::DispatchTable;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::execution_context::ExecutionContext;
use code::EntryTrampoline;
use compile_queue::{CompileQueue, CompileResult};
use executable_memory::{CodeAllocator, ExecutableMemory};
use options::Options;

/// Whether this build has the JIT (see `disabled.rs` for builds without it).
pub const BUILT: bool = true;

/// The frame a pointer the runtime got from JIT code or the VM names.
fn frame_of<'a>(frame: NonNull<ExecutionContext>) -> &'a ExecutionContext {
    // SAFETY: Callers pass frames that are live on the interpreter stack, which stay live while the reference is used.
    unsafe { frame.as_ref() }
}

/// A field offset as JIT code takes it.
fn offset(offset: usize) -> u32 {
    u32::try_from(offset).expect("field offsets fit in u32")
}

/// The executable a frame runs.
fn executable_of(frame: &ExecutionContext) -> Gc<Executable> {
    Executable::from_head(frame.executable.get().expect("the frame runs an executable"))
}

/// The frames above `frame` on the running frame's chain of callers, innermost first, or `None` if `frame` is not on
/// it.
fn frames_above(vm: &Vm, frame: NonNull<ExecutionContext>) -> Option<Vec<NonNull<ExecutionContext>>> {
    let mut frames = Vec::new();
    let mut running = NonNull::new(vm.head.running_execution_context.get());
    while let Some(current) = running {
        if current == frame {
            return Some(frames);
        }
        frames.push(current);
        running = NonNull::new(frame_of(current).caller_frame.get());
    }
    None
}

/// What calls of an executable without JIT code enter through its slot in the JIT entry table.
pub fn not_compiled_call_entry() -> *const core::ffi::c_void {
    entry_exit::libjs_jit_not_compiled_call_entry as *const core::ffi::c_void
}

/// Which interpreter handlers an executable's frames run with. Each tier's value is the index of its dispatch table
/// in the VM's dispatch tables.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum InterpreterTier {
    /// The plain handlers, which neither collect feedback nor count the tier-up budget. Everything runs on them while
    /// the JIT is off.
    Plain = 0,
    /// The plain handlers, except that function entry, loop back edges, calls and returns use the profiling handlers,
    /// which count the tier-up budget, enter JIT code and switch to the dispatch table of the frame they continue in.
    /// New executables warm up here while the JIT is on, so code that only runs a few times never pays for profiling.
    WarmingUp,
    /// The plain handlers, except that calls and returns use the profiling handlers, for executables the JIT never
    /// compiles: their frames neither collect feedback nor count the tier-up budget, but still enter the JIT code of
    /// the functions they call.
    Unprofiled,
    /// The profiling handlers, which collect feedback for the JIT.
    Profiling,
}

impl InterpreterTier {
    pub const ALL: [InterpreterTier; 4] = [Self::Plain, Self::WarmingUp, Self::Unprofiled, Self::Profiling];
}

/// A compile job running on the compile thread. The VM keeps the executable and every cell its snapshot refers to
/// alive until the job's code is installed or the job is abandoned.
pub struct CompileJob {
    pub executable: Gc<Executable>,
    pub cells: Vec<Gc<CellHeader>>,
}

/// With the "stress-install" option, finished compile jobs wait for at most this many tier-up checks.
const MAX_STRESS_INSTALL_DELAY: u32 = 16;

/// The pseudo-random numbers the stress options make their choices with: SplitMix64, seeded from the "seed" option.
pub(crate) struct StressRandom {
    state: Cell<u64>,
}

impl StressRandom {
    fn new(seed: u64) -> Self {
        Self { state: Cell::new(seed) }
    }

    pub(crate) fn next_u64(&self) -> u64 {
        let state = self.state.get().wrapping_add(0x9e37_79b9_7f4a_7c15);
        self.state.set(state);
        let mut z = state;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        z ^ (z >> 31)
    }

    /// A number from `low` to `high`, both included.
    pub(crate) fn between(&self, low: u32, high: u32) -> u32 {
        debug_assert!(low <= high);
        let range = u64::from(high - low) + 1;
        low + (self.next_u64() % range) as u32
    }
}

/// The JIT's state on a VM.
pub struct JitState {
    pub options: Options,
    /// Where the stress options get their pseudo-random numbers.
    pub(crate) stress_random: StressRandom,
    /// With the "stress-exits" option, how many more checks JIT code runs before it exits. JIT code counts it down;
    /// `libjs_jit_exit()` sets it again after an exit it caused.
    stress_exit_countdown: Box<Cell<u32>>,
    /// With the "stress-install" option, the results of finished compile jobs that are not installed yet, with how
    /// many more tier-up checks they wait for.
    held_compile_results: RefCell<Vec<(u32, u64, CompileResult)>>,
    /// With the "coverage" option, how often each coverage key was counted.
    coverage: RefCell<BTreeMap<String, u64>>,
    /// The dispatch table of executables that warm up, see `InterpreterTier::WarmingUp`. Only built while the
    /// interpreter collects feedback.
    pub(crate) warming_up_dispatch_table: Option<Box<DispatchTable>>,
    /// The dispatch table of executables the JIT never compiles, see `InterpreterTier::Unprofiled`. Only built while
    /// the interpreter collects feedback.
    pub(crate) unprofiled_dispatch_table: Option<Box<DispatchTable>>,
    /// Where calls of executables enter, and their JIT code (see `entry_table`). Only made while the JIT is on.
    pub(crate) entry_table: Option<entry_table::JitEntryTable>,
    /// The memory the VM's JIT code lives in.
    code_allocator: Rc<RefCell<CodeAllocator>>,
    /// The compile thread, started by the first asynchronous compile job.
    compile_queue: OnceCell<CompileQueue>,
    /// The compile jobs that run on the compile thread, by id.
    jobs: RefCell<HashMap<u64, CompileJob>>,
    next_job_id: Cell<u64>,
    /// The address of the slow path of each generic opcode JIT code calls, or 0, indexed by opcode.
    slow_paths: OnceCell<Vec<u64>>,
    /// How native code enters JIT code, generated the first time it does.
    entry_trampoline: OnceCell<(ExecutableMemory, EntryTrampoline)>,
}

impl JitState {
    pub fn new(options: Options) -> Self {
        translate::RegisterDump::assert_layout();
        let warming_up_dispatch_table = options
            .collects_feedback()
            .then(dispatch_tables::warming_up_dispatch_table);
        let unprofiled_dispatch_table = options
            .collects_feedback()
            .then(dispatch_tables::unprofiled_dispatch_table);
        let entry_table = options
            .enabled
            .then(|| entry_table::JitEntryTable::new(not_compiled_call_entry()));
        let stress_random = StressRandom::new(options.seed);
        let stress_exit_countdown = Box::new(Cell::new(0));
        if options.stress_exits != 0 {
            stress_exit_countdown.set(stress_random.between(1, options.stress_exits));
        }
        Self {
            options,
            stress_random,
            stress_exit_countdown,
            held_compile_results: RefCell::default(),
            coverage: RefCell::default(),
            warming_up_dispatch_table,
            unprofiled_dispatch_table,
            entry_table,
            code_allocator: Rc::default(),
            compile_queue: OnceCell::new(),
            jobs: RefCell::default(),
            next_job_id: Cell::new(1),
            slow_paths: OnceCell::new(),
            entry_trampoline: OnceCell::new(),
        }
    }

    /// Whether the interpreter collects feedback and counts down tier-up budgets.
    pub fn collects_feedback(&self) -> bool {
        self.options.collects_feedback()
    }

    /// The address of the countdown JIT code exits at with the "stress-exits" option, or 0 without it.
    pub(crate) fn stress_exit_countdown_address(&self) -> u64 {
        if self.options.stress_exits == 0 {
            return 0;
        }
        self.stress_exit_countdown.as_ptr() as u64
    }

    /// Whether the exit JIT code is taking was caused by the "stress-exits" option. If so, starts the next
    /// countdown.
    pub(crate) fn take_stress_exit(&self) -> bool {
        if self.options.stress_exits == 0 || self.stress_exit_countdown.get() != 0 {
            return false;
        }
        self.stress_exit_countdown
            .set(self.stress_random.between(1, self.options.stress_exits));
        true
    }

    /// The results of finished compile jobs to install now, of those that just `finished` and those held back by the
    /// "stress-install" option, which holds each result back for a random number of calls.
    pub(crate) fn compile_results_to_install(&self, finished: Vec<(u64, CompileResult)>) -> Vec<(u64, CompileResult)> {
        if !self.options.stress_install {
            return finished;
        }
        let mut held = self.held_compile_results.borrow_mut();
        for (id, result) in finished {
            held.push((self.stress_random.between(0, MAX_STRESS_INSTALL_DELAY), id, result));
        }
        let mut install = Vec::new();
        let mut index = 0;
        while index < held.len() {
            if held[index].0 == 0 {
                let (_, id, result) = held.remove(index);
                install.push((id, result));
                continue;
            }
            held[index].0 -= 1;
            index += 1;
        }
        install
    }

    /// With the "coverage" option, counts `key`: what compiled code contained when it was installed (see
    /// `libjs_jit::coverage`), and what happened to it ("exit.BadShape", "discard", "osr-entry", ...).
    pub(crate) fn count_coverage(&self, key: &str) {
        if self.options.coverage.is_none() {
            return;
        }
        *self.coverage.borrow_mut().entry(key.to_string()).or_default() += 1;
    }

    /// With the "coverage" option, writes the coverage counts into a new file in the coverage directory and starts
    /// counting from zero. VMs do this when they go away; processes that never destroy theirs do it before exiting.
    pub fn flush_coverage(&self) {
        if let Some(directory) = &self.options.coverage
            && !self.coverage.borrow().is_empty()
        {
            self.write_coverage(directory);
            self.coverage.borrow_mut().clear();
        }
    }

    fn write_coverage(&self, directory: &str) {
        static WRITTEN: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
        let number = WRITTEN.fetch_add(1, core::sync::atomic::Ordering::Relaxed);
        let mut json = String::from("{");
        for (index, (key, count)) in self.coverage.borrow().iter().enumerate() {
            if index != 0 {
                json.push_str(",\n");
            }
            json.push_str(&format!("\"{}\": {count}", key.replace(['\\', '"'], "_")));
        }
        json.push_str("}\n");
        let _ = std::fs::create_dir_all(directory);
        let path = format!("{directory}/coverage-{}-{number}.json", std::process::id());
        if let Err(error) = std::fs::write(&path, json) {
            eprintln!("LIBJS_JIT: Could not write {path}: {error}");
        }
    }

    /// The results of finished compile jobs the "stress-install" option holds back.
    pub(crate) fn take_held_compile_results(&self) -> Vec<(u64, CompileResult)> {
        self.held_compile_results
            .take()
            .into_iter()
            .map(|(_, id, result)| (id, result))
            .collect()
    }

    pub(crate) fn code_allocator(&self) -> &Rc<RefCell<CodeAllocator>> {
        &self.code_allocator
    }

    pub(crate) fn compile_queue(&self) -> &CompileQueue {
        self.compile_queue.get_or_init(CompileQueue::start)
    }

    /// The compile thread, if a compile job started it.
    pub(crate) fn started_compile_queue(&self) -> Option<&CompileQueue> {
        self.compile_queue.get()
    }

    pub(crate) fn add_job(&self, job: CompileJob) -> u64 {
        let id = self.next_job_id.get();
        self.next_job_id.set(id + 1);
        self.jobs.borrow_mut().insert(id, job);
        id
    }

    /// Forgets every compile job, whose results are dropped when they finish.
    pub(crate) fn abandon_compile_jobs(&self) {
        self.jobs.borrow_mut().clear();
    }

    pub(crate) fn take_job(&self, id: u64) -> Option<CompileJob> {
        self.jobs.borrow_mut().remove(&id)
    }

    /// The entry trampoline (see `libjs_jit::codegen::generate_entry_trampoline()`).
    pub(crate) fn entry_trampoline(&self) -> EntryTrampoline {
        self.entry_trampoline
            .get_or_init(|| {
                let code = libjs_jit::codegen::generate_entry_trampoline::<libjs_jit::asm::MacroAssembler>()
                    .expect("the entry trampoline can be generated");
                let memory = ExecutableMemory::allocate(
                    &self.code_allocator,
                    &[code.as_slice()],
                    &["entry trampoline"],
                    self.options.perf_map,
                )
                .pop()
                .expect("one code was allocated");
                // SAFETY: The code is the entry trampoline, with its calling convention.
                let trampoline = unsafe { core::mem::transmute::<*const u8, EntryTrampoline>(memory.address()) };
                (memory, trampoline)
            })
            .1
    }

    pub(crate) fn slow_paths(&self) -> &[u64] {
        self.slow_paths.get_or_init(runtime_info::slow_path_addresses)
    }
}

impl Drop for JitState {
    fn drop(&mut self) {
        self.flush_coverage();
    }
}

unsafe impl Trace for JitState {
    fn trace(&self, visitor: &mut Visitor) {
        for job in self.jobs.borrow().values() {
            visitor.visit(job.executable);
            for cell in &job.cells {
                visitor.visit(*cell);
            }
        }
    }
}

/// Keeps the heap from collecting garbage while it lives.
pub(crate) struct DeferGc<'vm>(&'vm Vm);

impl<'vm> DeferGc<'vm> {
    pub(crate) fn new(vm: &'vm Vm) -> Self {
        // SAFETY: The VM's heap is live.
        unsafe { capi::gc_heap_defer_gc(vm.heap().raw()) };
        Self(vm)
    }
}

impl Drop for DeferGc<'_> {
    fn drop(&mut self) {
        // SAFETY: As above; this undoes the deferral of new().
        unsafe { capi::gc_heap_undefer_gc(self.0.heap().raw()) };
    }
}

/// The name of an executable in logs and perf maps, with the place of its code: "name (line:column)" of its first
/// instruction that has a source position.
pub fn describe_executable(executable: &Executable) -> String {
    let name = crate::utf16::Utf16View::of_fly_string(&executable.name()).to_utf8();
    let name = if name.is_empty() {
        "<anonymous>".to_string()
    } else {
        name
    };
    match executable.source_map.iter().find(|entry| entry.line != 0) {
        Some(entry) => format!("{name} ({}:{})", entry.line, entry.column),
        None => name,
    }
}
