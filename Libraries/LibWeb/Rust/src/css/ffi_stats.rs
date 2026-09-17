/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Counters for the style system's FFI boundary crossings.
//!
//! Every entry point into the Rust style core and every callback it makes into
//! C++ bumps one counter, grouped by operation rather than by symbol. The
//! counters measure how coarse the boundary is (calls per element, per
//! declaration, per longhand) so that boundary-removal work can compare
//! before/after counts on deterministic workloads. Reading and resetting is
//! exposed to C++ for the `internals.styleFfiCounters()` test surface.
//!
//! The counters are always compiled in but disabled until reset. Each thread owns
//! a bridge counter context, including operations outside a document update.
//! Observation folds those contexts into process-wide totals; the disabled hot
//! path remains one relaxed atomic load per crossing.

use std::cell::{OnceCell, RefCell};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};

macro_rules! define_ffi_ops {
    ($($variant:ident => $name:literal,)+) => {
        /// One countable boundary operation. Entries are C++ calls into the
        /// Rust core; callbacks are calls the core makes back into C++.
        #[derive(Clone, Copy)]
        #[repr(usize)]
        pub(crate) enum FfiOp {
            $($variant,)+
        }

        const FFI_OP_COUNT: usize = 0 $(+ { let _ = FfiOp::$variant; 1 })+;

        /// Nul-terminated so the name can cross the FFI as a C string.
        static FFI_OP_NAMES: [&str; FFI_OP_COUNT] = [$(concat!($name, "\0"),)+];
    };
}

define_ffi_ops! {
    // Entries: C++ -> Rust.
    CascadeBulkEntry => "cascadeBulkEntries",
    CascadedStoreQueryEntry => "cascadedStoreQueryEntries",
    CustomPropertyStoreLifecycleEntry => "customPropertyStoreLifecycleEntries",
    LonghandDriverEntry => "longhandDriverEntries",
    LonghandDriverPhaseCallback => "longhandDriverPhaseCallbacks",
    ShorthandExpansionEntry => "shorthandExpansionEntries",
    NestedPropertyComputeEntry => "nestedPropertyComputeEntries",
    CalcOperationEntry => "calcOperationEntries",
    CalcNodeBuildEntry => "calcNodeBuildEntries",
    CalcNodeQueryEntry => "calcNodeQueryEntries",
    CalcNodeRetainReleaseEntry => "calcNodeRetainReleaseEntries",
    StyleValueCreateEntry => "styleValueCreateEntries",
    StyleValueDestroyEntry => "styleValueDestroyEntries",
    StyleValueQueryEntry => "styleValueQueryEntries",
    StyleValueSerializeEntry => "styleValueSerializeEntries",
    StyleGroupCloneEntry => "styleGroupCloneEntries",
    StyleGroupFreeEntry => "styleGroupFreeEntries",
    AnimationKeyframeLonghandEntry => "animationKeyframeLonghandEntries",
    AnimationEvaluationEntry => "animationEvaluationEntries",
    TransitionDecisionEntry => "transitionDecisionEntries",
    // Computed longhand table passes whose cost follows the table's width rather than a change.

    WinnerStoreBuilds => "winnerStoreBuilds",
    WinnerStoreValueRetains => "winnerStoreValueRetains",
    FlippedRuleVectorBuilds => "flippedRuleVectorBuilds",
    ComputedGroupIdentityLookups => "computedGroupIdentityLookups",
    LonghandTableCopiedSlots => "longhandTableCopiedSlots",
    LonghandTableCopyRetains => "longhandTableCopyRetains",
    LonghandTableStorageAllocations => "longhandTableStorageAllocations",
    LonghandTableClone => "longhandTableClones",
    LonghandTableFullHash => "longhandTableFullHashes",
    LonghandTableSlotHash => "longhandTableSlotHashes",
    SubstitutionCallbackFreeParse => "substitutionCallbackFreeParses",
    SubstitutionCallbackParseRequest => "substitutionCallbackParseRequests",
    SizesAttributeParseEntry => "sizesAttributeParseEntries",
    // Ownership callbacks: Rust -> C++.
    StringRetainReleaseCallback => "stringRetainReleaseCallbacks",
    SubstitutionOracleCallback => "substitutionOracleCallbacks",
    // CSS parser callbacks: Rust -> C++.
    EvaluateConditionCallback => "evaluateConditionCallbacks",
    MediaEnvironmentCallback => "mediaEnvironmentCallbacks",
}

/// Counts owned by one bridge context. Atomics permit observation from another thread;
/// updates never touch another context's counter cache lines.
struct FfiCounters {
    values: [AtomicU64; FFI_OP_COUNT],
}

impl FfiCounters {
    fn new() -> Self {
        Self {
            values: [const { AtomicU64::new(0) }; FFI_OP_COUNT],
        }
    }

    #[inline]
    fn bump(&self, op: FfiOp) {
        self.values[op as usize].fetch_add(1, Ordering::Relaxed);
    }
}

/// Process-wide observation folds live contexts and the totals of contexts that have exited.
/// The registry is locked only at observation, reset and context creation/destruction.
struct CounterRegistry {
    completed: [u64; FFI_OP_COUNT],
    live: Vec<Arc<FfiCounters>>,
}

impl CounterRegistry {
    const fn new() -> Self {
        Self {
            completed: [0; FFI_OP_COUNT],
            live: Vec::new(),
        }
    }

    fn value(&self, index: usize) -> u64 {
        self.live.iter().fold(self.completed[index], |total, counters| {
            total.wrapping_add(counters.values[index].load(Ordering::Relaxed))
        })
    }

    fn reset(&mut self) {
        self.completed.fill(0);
        for counters in &self.live {
            for value in &counters.values {
                value.store(0, Ordering::Relaxed);
            }
        }
    }
}

struct BridgeCounterContext {
    counters: Arc<FfiCounters>,
    registry: Arc<Mutex<CounterRegistry>>,
}

impl BridgeCounterContext {
    fn new(registry: Arc<Mutex<CounterRegistry>>) -> Self {
        let counters = Arc::new(FfiCounters::new());
        registry.lock().unwrap().live.push(counters.clone());
        Self { counters, registry }
    }
}

impl Drop for BridgeCounterContext {
    fn drop(&mut self) {
        let mut registry = self.registry.lock().unwrap();
        for (total, value) in registry.completed.iter_mut().zip(&self.counters.values) {
            *total = total.wrapping_add(value.load(Ordering::Relaxed));
        }
        registry.live.retain(|counters| !Arc::ptr_eq(counters, &self.counters));
    }
}

fn counter_registry() -> &'static Arc<Mutex<CounterRegistry>> {
    static REGISTRY: OnceLock<Arc<Mutex<CounterRegistry>>> = OnceLock::new();
    REGISTRY.get_or_init(|| Arc::new(Mutex::new(CounterRegistry::new())))
}

thread_local! {
    // Includes parsing and value lifetime operations outside a document's style update.
    static COUNTER_CONTEXT: OnceCell<BridgeCounterContext> = const { OnceCell::new() };
}

static COUNTERS_ENABLED: AtomicBool = AtomicBool::new(false);
#[cfg(test)]
thread_local! {
    pub(crate) static CPP_CALLBACK_COUNT: std::cell::Cell<u64> = const { std::cell::Cell::new(0) };
    pub(crate) static THREAD_UNSAFE_CPP_CALLBACK_COUNT: std::cell::Cell<u64> = const { std::cell::Cell::new(0) };
}
thread_local! {
    static COMPLETE_STYLE_UPDATE_STATE: RefCell<CompleteStyleUpdateState> = const { RefCell::new(CompleteStyleUpdateState::new()) };
}

#[derive(Default)]
struct DeferredCppReleases {
    fly_strings: Vec<usize>,
}

impl DeferredCppReleases {
    const fn new() -> Self {
        Self {
            fly_strings: Vec::new(),
        }
    }
}

struct CompleteStyleUpdateState {
    depth: u32,
    releases: DeferredCppReleases,
    has_outstanding_view: bool,
}

impl CompleteStyleUpdateState {
    const fn new() -> Self {
        Self {
            depth: 0,
            releases: DeferredCppReleases::new(),
            has_outstanding_view: false,
        }
    }
}

#[repr(C)]
pub struct FfiDeferredCppReleases {
    pub fly_strings: *const usize,
    pub fly_string_count: usize,
}

unsafe extern "C" {
    fn ladybird_utf16_fly_string_unref(raw: usize);
}

#[inline]
pub(crate) fn bump(op: FfiOp) {
    if COUNTERS_ENABLED.load(Ordering::Relaxed) {
        let counted = COUNTER_CONTEXT.try_with(|context| {
            context
                .get_or_init(|| BridgeCounterContext::new(counter_registry().clone()))
                .counters
                .bump(op);
        });
        // Another thread-local owner's destructor may count after this context exited.
        if counted.is_err() {
            BridgeCounterContext::new(counter_registry().clone()).counters.bump(op);
        }
    }
}

/// Counts a table ownership boundary only while diagnostics are enabled.
#[inline]
pub(crate) fn count_table_copy(values: impl FnOnce() -> (u64, u64)) {
    if !COUNTERS_ENABLED.load(Ordering::Relaxed) {
        return;
    }
    let (slots, retains) = values();
    let _ = COUNTER_CONTEXT.try_with(|context| {
        let context = context.get_or_init(|| BridgeCounterContext::new(counter_registry().clone()));
        context.counters.values[FfiOp::LonghandTableCopiedSlots as usize].fetch_add(slots, Ordering::Relaxed);
        context.counters.values[FfiOp::LonghandTableCopyRetains as usize].fetch_add(retains, Ordering::Relaxed);
    });
}

#[inline]
pub(crate) fn bump_cpp_callback(op: FfiOp) {
    #[cfg(test)]
    {
        CPP_CALLBACK_COUNT.set(CPP_CALLBACK_COUNT.get() + 1);
        if !matches!(op, FfiOp::StringRetainReleaseCallback) {
            THREAD_UNSAFE_CPP_CALLBACK_COUNT.set(THREAD_UNSAFE_CPP_CALLBACK_COUNT.get() + 1);
        }
    }
    bump(op);
}

pub(crate) fn release_utf16_fly_string(raw: usize) {
    let deferred = COMPLETE_STYLE_UPDATE_STATE.with(|state| {
        let mut state = state.borrow_mut();
        if state.depth == 0 {
            return false;
        }
        state.releases.fly_strings.push(raw);
        true
    });
    if deferred {
        return;
    }
    bump_cpp_callback(FfiOp::StringRetainReleaseCallback);
    unsafe { ladybird_utf16_fly_string_unref(raw) };
}

/// Marks a complete C++-orchestrated style update, from transaction planning
/// through consumption of every published style reaction.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_ffi_complete_style_update_begin() {
    COMPLETE_STYLE_UPDATE_STATE.with(|state| {
        let mut state = state.borrow_mut();
        assert!(
            !state.has_outstanding_view,
            "complete style update entered while deferred releases are being drained"
        );
        state.depth = state
            .depth
            .checked_add(1)
            .expect("complete style update depth overflow");
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_ffi_complete_style_update_end() -> FfiDeferredCppReleases {
    COMPLETE_STYLE_UPDATE_STATE.with(|state| {
        let mut state = state.borrow_mut();
        state.depth = state
            .depth
            .checked_sub(1)
            .expect("unbalanced complete style update scope");
        if state.depth != 0 {
            return FfiDeferredCppReleases {
                fly_strings: std::ptr::null(),
                fly_string_count: 0,
            };
        }
        assert!(!state.has_outstanding_view, "deferred release view was not cleared");
        state.has_outstanding_view = true;
        FfiDeferredCppReleases {
            fly_strings: state.releases.fly_strings.as_ptr(),
            fly_string_count: state.releases.fly_strings.len(),
        }
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_deferred_cpp_releases_clear() {
    COMPLETE_STYLE_UPDATE_STATE.with(|state| {
        let mut state = state.borrow_mut();
        if !state.has_outstanding_view {
            return;
        }
        assert_eq!(state.depth, 0, "deferred releases cleared during a style update");
        state.releases.fly_strings.clear();
        state.has_outstanding_view = false;
    });
}

/// Returns the number of FFI boundary counters.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_ffi_counter_count() -> usize {
    FFI_OP_COUNT
}

/// Returns the nul-terminated name of the counter at `index`.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_ffi_counter_name(index: usize) -> *const u8 {
    FFI_OP_NAMES[index].as_ptr()
}

/// Returns the current value of the counter at `index`.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_ffi_counter_value(index: usize) -> u64 {
    counter_registry().lock().unwrap().value(index)
}

/// Resets every counter to zero.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_ffi_counters_reset() {
    counter_registry().lock().unwrap().reset();
    COUNTERS_ENABLED.store(true, Ordering::Relaxed);
}

/// Notes the adoption of a Rust style value allocation by a C++ shell; called
/// from the C++ side where shell construction funnels through one place.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_ffi_note_style_value_created() {
    bump(FfiOp::StyleValueCreateEntry);
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_ffi_note_animation_evaluation() {
    bump(FfiOp::AnimationEvaluationEntry);
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_ffi_note_transition_decision() {
    bump(FfiOp::TransitionDecisionEntry);
}

#[cfg(test)]
mod counter_context_tests {
    use super::*;

    #[test]
    fn observation_includes_live_and_completed_bridge_contexts() {
        let registry = Arc::new(Mutex::new(CounterRegistry::new()));
        let context = BridgeCounterContext::new(registry.clone());
        context.counters.bump(FfiOp::LonghandTableSlotHash);
        let (ready, ready_rx) = std::sync::mpsc::channel();
        let (finish, finish_rx) = std::sync::mpsc::channel();
        let other_registry = registry.clone();
        let other = std::thread::spawn(move || {
            let context = BridgeCounterContext::new(other_registry);
            context.counters.bump(FfiOp::LonghandTableSlotHash);
            context.counters.bump(FfiOp::StyleValueDestroyEntry);
            ready.send(()).unwrap();
            finish_rx.recv().unwrap();
        });
        ready_rx.recv().unwrap();
        assert_eq!(registry.lock().unwrap().value(FfiOp::LonghandTableSlotHash as usize), 2);
        assert_eq!(
            registry.lock().unwrap().value(FfiOp::StyleValueDestroyEntry as usize),
            1
        );
        finish.send(()).unwrap();
        other.join().unwrap();
        assert_eq!(registry.lock().unwrap().value(FfiOp::LonghandTableSlotHash as usize), 2);
        assert_eq!(
            registry.lock().unwrap().value(FfiOp::StyleValueDestroyEntry as usize),
            1
        );
        drop(context);
        assert_eq!(registry.lock().unwrap().value(FfiOp::LonghandTableSlotHash as usize), 2);
    }

    #[test]
    fn reset_clears_completed_counts_and_contexts_continue_counting() {
        let registry = Arc::new(Mutex::new(CounterRegistry::new()));
        let context = BridgeCounterContext::new(registry.clone());
        context.counters.bump(FfiOp::LonghandTableSlotHash);
        let completed = BridgeCounterContext::new(registry.clone());
        completed.counters.bump(FfiOp::LonghandTableSlotHash);
        drop(completed);
        assert_eq!(registry.lock().unwrap().value(FfiOp::LonghandTableSlotHash as usize), 2);
        registry.lock().unwrap().reset();
        assert_eq!(registry.lock().unwrap().value(FfiOp::LonghandTableSlotHash as usize), 0);
        context.counters.bump(FfiOp::LonghandTableSlotHash);
        let later = BridgeCounterContext::new(registry.clone());
        later.counters.bump(FfiOp::LonghandTableSlotHash);
        drop(later);
        assert_eq!(registry.lock().unwrap().value(FfiOp::LonghandTableSlotHash as usize), 2);
        drop(context);
        assert_eq!(registry.lock().unwrap().value(FfiOp::LonghandTableSlotHash as usize), 2);
    }
}
