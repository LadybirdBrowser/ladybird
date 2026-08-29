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
//! The counters are always compiled in but disabled until first read. The
//! disabled hot path is one relaxed atomic load per crossing.

use std::cell::RefCell;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

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
    CustomPropertyStoreQueryEntry => "customPropertyStoreQueryEntries",
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
    SubstitutionCallbackFreeParse => "substitutionCallbackFreeParses",
    SubstitutionCallbackParseRequest => "substitutionCallbackParseRequests",
    SizesAttributeParseEntry => "sizesAttributeParseEntries",
    // Ownership callbacks: Rust -> C++.
    StringRetainReleaseCallback => "stringRetainReleaseCallbacks",
    SubstitutionOracleCallback => "substitutionOracleCallbacks",
    // CSS parser callbacks: Rust -> C++.
    InternUtf16FlyStringCallback => "internUtf16FlyStringCallbacks",
    EvaluateConditionCallback => "evaluateConditionCallbacks",
    MediaEnvironmentCallback => "mediaEnvironmentCallbacks",
}

static COUNTERS: [AtomicU64; FFI_OP_COUNT] = [const { AtomicU64::new(0) }; FFI_OP_COUNT];
static COUNTERS_ENABLED: AtomicBool = AtomicBool::new(false);
thread_local! {
    static COMPLETE_STYLE_UPDATE_STATE: RefCell<CompleteStyleUpdateState> = const { RefCell::new(CompleteStyleUpdateState::new()) };
}

#[derive(Default)]
struct DeferredCppReleases {
    fly_strings: Vec<usize>,
    strings: Vec<usize>,
}

impl DeferredCppReleases {
    const fn new() -> Self {
        Self {
            fly_strings: Vec::new(),
            strings: Vec::new(),
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
    pub strings: *const usize,
    pub string_count: usize,
}

unsafe extern "C" {
    fn ladybird_utf16_fly_string_unref(raw: usize);
    fn ladybird_string_unref(raw: usize);
}

#[inline]
pub(crate) fn bump(op: FfiOp) {
    if COUNTERS_ENABLED.load(Ordering::Relaxed) {
        COUNTERS[op as usize].fetch_add(1, Ordering::Relaxed);
    }
}

#[inline]
pub(crate) fn bump_cpp_callback(op: FfiOp) {
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

pub(crate) fn release_string(raw: usize) {
    let deferred = COMPLETE_STYLE_UPDATE_STATE.with(|state| {
        let mut state = state.borrow_mut();
        if state.depth == 0 {
            return false;
        }
        state.releases.strings.push(raw);
        true
    });
    if deferred {
        return;
    }
    bump_cpp_callback(FfiOp::StringRetainReleaseCallback);
    unsafe { ladybird_string_unref(raw) };
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
                strings: std::ptr::null(),
                string_count: 0,
            };
        }
        assert!(!state.has_outstanding_view, "deferred release view was not cleared");
        state.has_outstanding_view = true;
        FfiDeferredCppReleases {
            fly_strings: state.releases.fly_strings.as_ptr(),
            fly_string_count: state.releases.fly_strings.len(),
            strings: state.releases.strings.as_ptr(),
            string_count: state.releases.strings.len(),
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
        state.releases.strings.clear();
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
    COUNTERS[index].load(Ordering::Relaxed)
}

/// Resets every counter to zero.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_ffi_counters_reset() {
    for counter in &COUNTERS {
        counter.store(0, Ordering::Relaxed);
    }
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
