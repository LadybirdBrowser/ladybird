/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Code flushing: the code of functions that nothing ran for a while goes away, and comes back through the lazy compile
//! path when the function is called again.
//!
//! Each executable counts the collections since a frame last entered it (`ExecutableHead::idle_collection_count`): the
//! interpreter's Enter, the resume of a generator and the prologue of JIT code set it to 0, and the VM's sweep callback
//! counts it up (and sets it to 0 for the executables that frames still run). The VM keeps the times of the last
//! collections, which tell how long an executable was not entered for.
//!
//! There are two layers, each with an idle time of its own:
//! - The JIT code of an executable that idled for the first one is dropped, like a discard that does not count against
//!   the executable (see `Executable::flush_idle_jit_code()`).
//! - The executable of a function that idled for the second one is dropped as a whole. While a collection marks, the
//!   shared data of the function does not visit such an executable, so it dies unless something else holds it: a frame
//!   on a stack or in a suspended generator, JIT code that inlined it or calls it directly, a compile job, a local of
//!   native code. If it dies, the shared data goes back to its lazy state. It keeps the shared data of the functions
//!   the executable creates and the executable's template objects, which the next executable of its code takes over,
//!   so that a recompile changes nothing that code can observe.
//!
//! A layer's idle time is its base shifted left by how often the function was compiled again after a flush in that
//! layer, up to a limit, and a function that was compiled again that often is not flushed in that layer anymore.
//! Nothing is flushed while a debugger is attached.
//!
//! LIBJS_CODE_FLUSH holds comma-separated options: `on`, `off`, `stress` (flush everything that nothing else holds at
//! every collection), `jit=SECONDS`, `bytecode=SECONDS`, `collections=N` (the fewest collections an executable idles
//! for), `backoff=N` (the largest shift of the idle times) and `max-recompiles=N`.

use core::cell::{Cell, RefCell};
use core::ptr::NonNull;
use std::collections::{HashSet, VecDeque};
use std::time::{Duration, Instant};

use crate::bytecode::executable::Executable;
use crate::gc::sweep::Sweep;
use crate::gc::weak::SweptAfterCollection;
use crate::interpreter::vm::Vm;
use crate::jit::code::CompileState;
use crate::layout::cell::Gc;
use crate::runtime::shared_function_instance_data::SharedFunctionInstanceData;

/// The flush layers, each with a count of recompiles on the shared data of a function.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FlushLayer {
    /// The JIT code of an executable.
    Jit = 0,
    /// The whole executable of a function.
    Bytecode = 1,
}

#[derive(Clone, Copy, Debug)]
pub struct CodeFlushingOptions {
    pub enabled: bool,
    /// Flush everything that nothing else holds at every collection, whatever its age.
    pub stress: bool,
    pub jit_idle_time: Duration,
    pub bytecode_idle_time: Duration,
    /// The fewest collections that an executable has to idle for to be flushed.
    pub min_idle_collections: u8,
    /// The largest left shift of the idle times by recompiles.
    pub max_backoff_shift: u8,
    /// After this many recompiles in a layer, a function is no longer flushed in that layer.
    pub max_recompiles: u8,
}

impl Default for CodeFlushingOptions {
    fn default() -> Self {
        Self {
            enabled: true,
            stress: false,
            jit_idle_time: Duration::from_secs(10),
            bytecode_idle_time: Duration::from_secs(20),
            min_idle_collections: 2,
            max_backoff_shift: 3,
            max_recompiles: 6,
        }
    }
}

impl CodeFlushingOptions {
    /// The options that LIBJS_CODE_FLUSH asks for. Options it does not know are reported and ignored.
    pub fn from_environment() -> Self {
        let mut options = Self::default();
        let Ok(value) = std::env::var("LIBJS_CODE_FLUSH") else {
            return options;
        };
        for option in value.split(',').filter(|option| !option.is_empty()) {
            let (name, argument) = option.split_once('=').unwrap_or((option, ""));
            let seconds = || {
                argument
                    .parse::<f64>()
                    .ok()
                    .and_then(|seconds| Duration::try_from_secs_f64(seconds).ok())
            };
            let number = || argument.parse::<u8>().ok();
            let known = match name {
                "off" => {
                    options.enabled = false;
                    true
                }
                "on" => {
                    options.enabled = true;
                    true
                }
                "stress" => {
                    options.stress = true;
                    true
                }
                "jit" => seconds().map(|time| options.jit_idle_time = time).is_some(),
                "bytecode" => seconds().map(|time| options.bytecode_idle_time = time).is_some(),
                "collections" => number().map(|count| options.min_idle_collections = count).is_some(),
                "backoff" => number()
                    .map(|shift| options.max_backoff_shift = shift.min(16))
                    .is_some(),
                "max-recompiles" => number().map(|count| options.max_recompiles = count).is_some(),
                _ => false,
            };
            if !known {
                eprintln!("LIBJS_CODE_FLUSH: Ignoring the option '{option}'");
            }
        }
        options
    }
}

/// What code flushing did in a VM, for measurements.
#[derive(Default)]
pub struct CodeFlushingStats {
    pub passes: Cell<u64>,
    pub flushes: [Cell<u64>; 2],
    pub recompiles: [Cell<u64>; 2],
    /// Recompiles that made a function reach the most recompiles of a layer, which stops its flushes there.
    pub backoff_stops: [Cell<u64>; 2],
    /// Recompiles of functions whose idle time was longer than the base time because of earlier recompiles.
    pub backed_off_recompiles: [Cell<u64>; 2],
    /// Executables of the bytecode layer that something else still held.
    pub held_bytecode_candidates: Cell<u64>,
    pub flushed_bytecode_bytes: Cell<u64>,
    pub recompile_nanoseconds: Cell<u64>,
}

impl CodeFlushingStats {
    fn count(counter: &Cell<u64>, amount: u64) {
        counter.set(counter.get().saturating_add(amount));
    }
}

/// A flush pass, from the start of the marking of a collection to its sweep.
#[derive(Clone, Copy)]
struct Pass {
    now: Instant,
    /// The layers in which to flush whatever nothing else holds, whatever its age, by bit (see `FlushLayer`).
    forced_layers: u8,
}

impl Pass {
    fn forces(self, layer: FlushLayer) -> bool {
        self.forced_layers & (1 << layer as u8) != 0
    }
}

/// Every flush layer, as bits.
pub const ALL_FLUSH_LAYERS: u8 = 1 << FlushLayer::Jit as u8 | 1 << FlushLayer::Bytecode as u8;

/// The most collections whose times the VM keeps, one more than an idle collection count can be.
const KEPT_COLLECTION_TIMES: usize = u8::MAX as usize + 1;

/// The shared data of a function that a flush pass found, which does not keep it alive.
struct FlushCandidate(Gc<SharedFunctionInstanceData>);

// SAFETY: A pass finds its candidates while its collection marks, each in the trace of its own shared data, which is
//         live then. The sweep callback of that collection takes the bytecode candidates, and its post-GC task the JIT
//         candidates, neither of which allocates while it holds them. begin_pass() drops whatever is left before the
//         next collection marks, so no candidate ever outlives a collection.
unsafe impl SweptAfterCollection for FlushCandidate {}

/// The code flushing state of a VM.
pub struct CodeFlushing {
    pub options: CodeFlushingOptions,
    /// When the last collections swept, the latest last.
    collection_times: RefCell<VecDeque<Instant>>,
    flush_at_next_collection: Cell<u8>,
    pass: Cell<Option<Pass>>,
    /// The shared data whose executable it did not visit in the marking of this collection.
    bytecode_candidates: RefCell<Vec<FlushCandidate>>,
    /// The shared data whose executable's JIT code idled long enough in this collection.
    jit_candidates: RefCell<Vec<FlushCandidate>>,
    pub stats: CodeFlushingStats,
}

thread_local! {
    /// The code flushing state of the VM whose collection marks on this thread, while it runs a flush pass.
    static ACTIVE_PASS: Cell<Option<NonNull<CodeFlushing>>> = const { Cell::new(None) };
}

impl CodeFlushing {
    pub fn new(options: CodeFlushingOptions) -> Self {
        Self {
            options,
            collection_times: RefCell::new(VecDeque::with_capacity(KEPT_COLLECTION_TIMES)),
            flush_at_next_collection: Cell::new(0),
            pass: Cell::new(None),
            bytecode_candidates: RefCell::default(),
            jit_candidates: RefCell::default(),
            stats: CodeFlushingStats::default(),
        }
    }

    /// Makes the next collection flush whatever nothing else holds in `layers` (bits of `FlushLayer`), whatever its
    /// age. Only for tests.
    pub fn flush_at_next_collection(&self, layers: u8) {
        self.flush_at_next_collection.set(layers);
    }

    /// Starts a flush pass for the collection that is about to mark the live cells, if the VM flushes code now.
    pub fn begin_pass(&self, vm: &Vm) {
        // NB: Only a collection uses up a forced flush; other markings (such as graph dumps) leave it for the next one.
        if !vm.heap().is_marking_for_collection() {
            return;
        }
        // NB: The candidates are not traced, so none may outlive the collection that found them, flush pass or not.
        self.bytecode_candidates.borrow_mut().clear();
        self.jit_candidates.borrow_mut().clear();
        let forced_layers = self.flush_at_next_collection.replace(0);
        if !(self.options.enabled || forced_layers != 0) || vm.debugging_enabled() {
            return;
        }
        self.pass.set(Some(Pass {
            now: Instant::now(),
            forced_layers: if self.options.stress {
                ALL_FLUSH_LAYERS
            } else {
                forced_layers
            },
        }));
        CodeFlushingStats::count(&self.stats.passes, 1);
        ACTIVE_PASS.set(Some(NonNull::from(self)));
    }

    /// Whether the idle time of an executable that idled for `idle_collections` collections is long enough to flush
    /// it in `layer` in this pass, for a function that was compiled again `recompiles` times after flushes there.
    fn idled_long_enough(&self, pass: Pass, layer: FlushLayer, idle_collections: u8, recompiles: u8) -> bool {
        if recompiles >= self.options.max_recompiles {
            return false;
        }
        if pass.forces(layer) {
            return true;
        }
        if pass.forced_layers != 0 || !self.options.enabled {
            return false;
        }
        if idle_collections == 0 || idle_collections < self.options.min_idle_collections {
            return false;
        }
        let times = self.collection_times.borrow();
        let Some(&idle_since) = times
            .len()
            .checked_sub(usize::from(idle_collections))
            .and_then(|index| times.get(index))
        else {
            return false;
        };
        let base = match layer {
            FlushLayer::Jit => self.options.jit_idle_time,
            FlushLayer::Bytecode => self.options.bytecode_idle_time,
        };
        let shift = recompiles.min(self.options.max_backoff_shift);
        pass.now.saturating_duration_since(idle_since) >= base.saturating_mul(1u32 << shift)
    }

    /// For the marking of a collection: whether the shared data `shared_data` should leave its executable unvisited,
    /// as it idled long enough to be flushed. Notes the shared data for the end of the pass.
    fn consider(&self, pass: Pass, shared_data: &SharedFunctionInstanceData, executable: Gc<Executable>) -> bool {
        if !shared_data.can_flush_code() {
            return false;
        }
        let idle_collections = executable.head.idle_collection_count.get();
        if executable.jit_compile_state() == CompileState::Installed
            && self.idled_long_enough(
                pass,
                FlushLayer::Jit,
                idle_collections,
                shared_data.recompile_count(FlushLayer::Jit),
            )
        {
            self.jit_candidates
                .borrow_mut()
                .push(FlushCandidate(Gc::from_ref(shared_data)));
        }
        if !self.idled_long_enough(
            pass,
            FlushLayer::Bytecode,
            idle_collections,
            shared_data.recompile_count(FlushLayer::Bytecode),
        ) {
            return false;
        }
        self.bytecode_candidates
            .borrow_mut()
            .push(FlushCandidate(Gc::from_ref(shared_data)));
        true
    }

    /// Ends the flush pass of the collection whose marks are final: flushes the executables of the candidates that
    /// died, and has the VM flush the JIT code of the others once the collection is over.
    pub fn finish_pass(&self, vm: &Vm, sweep: &Sweep<'_>) {
        ACTIVE_PASS.set(None);
        self.note_collection();
        if self.pass.take().is_none() {
            return;
        }
        let candidates = core::mem::take(&mut *self.bytecode_candidates.borrow_mut());
        for FlushCandidate(shared_data) in candidates {
            let Some(executable) = shared_data.executable() else {
                continue;
            };
            if !sweep.is_dead(executable) {
                CodeFlushingStats::count(&self.stats.held_bytecode_candidates, 1);
                continue;
            }
            CodeFlushingStats::count(&self.stats.flushes[FlushLayer::Bytecode as usize], 1);
            CodeFlushingStats::count(&self.stats.flushed_bytecode_bytes, executable.bytecode().len() as u64);
            shared_data.flush_executable(executable, sweep);
        }
        if !self.jit_candidates.borrow().is_empty() {
            vm.enqueue_code_flushing_task();
        }
    }

    /// Notes that a collection ran now.
    fn note_collection(&self) {
        let mut times = self.collection_times.borrow_mut();
        if times.len() == KEPT_COLLECTION_TIMES {
            times.pop_front();
        }
        times.push_back(Instant::now());
    }

    /// Drops the JIT code of the executables that idled long enough in the last collection, once it is over. Frames
    /// that still run an executable on a stack keep its code.
    pub fn flush_idle_jit_code(&self, vm: &Vm) {
        let candidates = core::mem::take(&mut *self.jit_candidates.borrow_mut());
        if candidates.is_empty() {
            return;
        }
        let mut running = HashSet::new();
        vm.for_each_live_execution_context(|frame| {
            if let Some(executable) = frame.executable.get() {
                running.insert(executable.as_ptr());
            }
            core::ops::ControlFlow::Continue(())
        });
        for FlushCandidate(shared_data) in candidates {
            let Some(executable) = shared_data.executable() else {
                continue;
            };
            if executable.jit_compile_state() != CompileState::Installed
                || running.contains(&Executable::head(executable).as_ptr())
            {
                continue;
            }
            CodeFlushingStats::count(&self.stats.flushes[FlushLayer::Jit as usize], 1);
            shared_data.note_flushed_jit_code();
            executable.flush_idle_jit_code(vm);
        }
    }

    /// Notes that a function whose code was flushed in `layer` was compiled again, `recompiles` times now.
    pub fn note_recompile(&self, layer: FlushLayer, recompiles: u8) {
        CodeFlushingStats::count(&self.stats.recompiles[layer as usize], 1);
        if recompiles > 1 && self.options.max_backoff_shift != 0 {
            CodeFlushingStats::count(&self.stats.backed_off_recompiles[layer as usize], 1);
        }
        if recompiles == self.options.max_recompiles {
            CodeFlushingStats::count(&self.stats.backoff_stops[layer as usize], 1);
        }
    }
}

/// For the marking of a collection: whether `shared_data` should leave its executable unvisited, as it idled long
/// enough to be flushed. Outside of a flush pass, it never should.
pub fn bytecode_flush_candidate(shared_data: &SharedFunctionInstanceData, executable: Gc<Executable>) -> bool {
    let Some(flushing) = ACTIVE_PASS.get() else {
        return false;
    };
    // SAFETY: The VM sets the pass when its collection starts marking and clears it in the sweep callback of that
    //         collection, and the VM outlives its collections.
    let flushing = unsafe { flushing.as_ref() };
    let Some(pass) = flushing.pass.get() else {
        return false;
    };
    flushing.consider(pass, shared_data, executable)
}
