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
//! The counters are always compiled in: one relaxed atomic increment per
//! crossing is negligible next to the crossing itself.

use std::cell::{Cell, RefCell};
use std::ffi::c_void;
use std::sync::atomic::{AtomicU64, Ordering};

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
    CascadeCustomPropertyEntry => "cascadeCustomPropertyEntries",
    CascadeResolutionEntry => "cascadeResolutionEntries",
    CascadeNativeSubstitutionRequest => "cascadeNativeSubstitutionRequests",
    CascadeCppResolutionRequest => "cascadeCppResolutionRequests",
    CascadedStoreQueryEntry => "cascadedStoreQueryEntries",
    CustomPropertyStoreLifecycleEntry => "customPropertyStoreLifecycleEntries",
    CustomPropertyStoreQueryEntry => "customPropertyStoreQueryEntries",
    LonghandDriverEntry => "longhandDriverEntries",
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
    AnimationEvaluationEntry => "animationEvaluationEntries",
    TransitionDecisionEntry => "transitionDecisionEntries",
    // Ownership callbacks: Rust -> C++.
    StringRetainReleaseCallback => "stringRetainReleaseCallbacks",
    AnimatedPropertiesRetainReleaseCallback => "animatedPropertiesRetainReleaseCallbacks",
}

static COUNTERS: [AtomicU64; FFI_OP_COUNT] = [const { AtomicU64::new(0) }; FFI_OP_COUNT];
thread_local! {
    static COUNTERS_ENABLED: Cell<bool> = const { Cell::new(false) };
    static COMPLETE_STYLE_UPDATE_DEPTH: Cell<u32> = const { Cell::new(0) };
    static DEFERRED_CPP_RELEASES: RefCell<DeferredCppReleases> = const { RefCell::new(DeferredCppReleases::new()) };
}

#[derive(Default)]
struct DeferredCppReleases {
    fly_strings: Vec<usize>,
    strings: Vec<usize>,
    animated_properties: Vec<*const c_void>,
}

impl DeferredCppReleases {
    const fn new() -> Self {
        Self {
            fly_strings: Vec::new(),
            strings: Vec::new(),
            animated_properties: Vec::new(),
        }
    }
}

#[repr(C)]
pub struct FfiDeferredCppReleases {
    pub fly_strings: *const usize,
    pub fly_string_count: usize,
    pub strings: *const usize,
    pub string_count: usize,
    pub animated_properties: *const *const c_void,
    pub animated_property_count: usize,
    pub storage: *mut c_void,
}

unsafe extern "C" {
    fn ladybird_utf16_fly_string_unref(raw: usize);
    fn ladybird_string_unref(raw: usize);
    fn ladybird_animated_properties_unref(values: *const c_void);
}

#[inline]
pub(crate) fn bump(op: FfiOp) {
    if COUNTERS_ENABLED.with(Cell::get) {
        COUNTERS[op as usize].fetch_add(1, Ordering::Relaxed);
    }
}

#[inline]
pub(crate) fn bump_cpp_callback(op: FfiOp) {
    bump(op);
}

pub(crate) fn release_utf16_fly_string(raw: usize) {
    let deferred = COMPLETE_STYLE_UPDATE_DEPTH.with(|depth| depth.get() != 0);
    if deferred {
        DEFERRED_CPP_RELEASES.with(|releases| releases.borrow_mut().fly_strings.push(raw));
        return;
    }
    bump_cpp_callback(FfiOp::StringRetainReleaseCallback);
    unsafe { ladybird_utf16_fly_string_unref(raw) };
}

pub(crate) fn release_string(raw: usize) {
    let deferred = COMPLETE_STYLE_UPDATE_DEPTH.with(|depth| depth.get() != 0);
    if deferred {
        DEFERRED_CPP_RELEASES.with(|releases| releases.borrow_mut().strings.push(raw));
        return;
    }
    bump_cpp_callback(FfiOp::StringRetainReleaseCallback);
    unsafe { ladybird_string_unref(raw) };
}

pub(crate) fn release_animated_properties(values: *const c_void) {
    let deferred = COMPLETE_STYLE_UPDATE_DEPTH.with(|depth| depth.get() != 0);
    if deferred {
        DEFERRED_CPP_RELEASES.with(|releases| releases.borrow_mut().animated_properties.push(values));
        return;
    }
    bump_cpp_callback(FfiOp::AnimatedPropertiesRetainReleaseCallback);
    unsafe { ladybird_animated_properties_unref(values) };
}

/// Marks a complete C++-orchestrated style update, from transaction planning
/// through consumption of every published style reaction.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_ffi_complete_style_update_begin() {
    COMPLETE_STYLE_UPDATE_DEPTH.with(|depth| {
        depth.set(
            depth
                .get()
                .checked_add(1)
                .expect("complete style update depth overflow"),
        );
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_ffi_complete_style_update_end() -> FfiDeferredCppReleases {
    let depth = COMPLETE_STYLE_UPDATE_DEPTH.with(|depth| {
        let new_depth = depth
            .get()
            .checked_sub(1)
            .expect("unbalanced complete style update scope");
        depth.set(new_depth);
        new_depth
    });
    if depth != 0 {
        return FfiDeferredCppReleases {
            fly_strings: std::ptr::null(),
            fly_string_count: 0,
            strings: std::ptr::null(),
            string_count: 0,
            animated_properties: std::ptr::null(),
            animated_property_count: 0,
            storage: std::ptr::null_mut(),
        };
    }
    DEFERRED_CPP_RELEASES.with(|releases| {
        let releases = Box::new(std::mem::take(&mut *releases.borrow_mut()));
        FfiDeferredCppReleases {
            fly_strings: releases.fly_strings.as_ptr(),
            fly_string_count: releases.fly_strings.len(),
            strings: releases.strings.as_ptr(),
            string_count: releases.strings.len(),
            animated_properties: releases.animated_properties.as_ptr(),
            animated_property_count: releases.animated_properties.len(),
            storage: Box::into_raw(releases).cast(),
        }
    })
}

/// # Safety
/// `storage` must be null or a live pointer returned by
/// `rust_style_ffi_complete_style_update_end`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_deferred_cpp_releases_destroy(storage: *mut c_void) {
    if !storage.is_null() {
        drop(unsafe { Box::from_raw(storage.cast::<DeferredCppReleases>()) });
    }
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
    COUNTERS_ENABLED.with(|enabled| enabled.set(true));
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
