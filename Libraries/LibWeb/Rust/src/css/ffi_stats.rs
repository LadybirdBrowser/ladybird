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

use std::cell::Cell;
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
    // Callbacks: Rust -> C++.
    SelectorMetadataCallback => "selectorMetadataCallbacks",
    CascadeResolveUnresolvedCallback => "cascadeResolveUnresolvedCallbacks",
    CascadeParseSubstitutedCallback => "cascadeParseSubstitutedCallbacks",
    CascadeSourceSlotCallback => "cascadeSourceSlotCallbacks",
    CascadeCustomPropertyBatchCallback => "cascadeCustomPropertyBatchCallbacks",
    ShorthandSetLonghandCallback => "shorthandSetLonghandCallbacks",
    LonghandStoreBatchCallback => "longhandStoreBatchCallbacks",
    StringRetainReleaseCallback => "stringRetainReleaseCallbacks",
    AnimationComputeBatchCallback => "animationComputeBatchCallbacks",
    AnimatedPropertiesRetainReleaseCallback => "animatedPropertiesRetainReleaseCallbacks",
    ComputedStyleBuildCppCallback => "computedStyleBuildCppCallbacks",
    CompleteStyleUpdateCppCallback => "completeStyleUpdateCppCallbacks",
    CompleteStyleUpdateSemanticCppCallback => "completeStyleUpdateSemanticCppCallbacks",
    CompleteStyleUpdateOwnershipCppCallback => "completeStyleUpdateOwnershipCppCallbacks",
    CompleteStyleUpdateTransactionCallback => "completeStyleUpdateTransactionCallbacks",
    CompleteStyleUpdateFlatTreeCallback => "completeStyleUpdateFlatTreeCallbacks",
    CompleteStyleUpdateCascadeCallback => "completeStyleUpdateCascadeCallbacks",
    CompleteStyleUpdateLonghandCallback => "completeStyleUpdateLonghandCallbacks",
    CompleteStyleUpdateAnimationCallback => "completeStyleUpdateAnimationCallbacks",
    CompleteStyleUpdateShorthandCallback => "completeStyleUpdateShorthandCallbacks",
}

static COUNTERS: [AtomicU64; FFI_OP_COUNT] = [const { AtomicU64::new(0) }; FFI_OP_COUNT];

thread_local! {
    static COMPUTED_STYLE_BUILD_DEPTH: Cell<u32> = const { Cell::new(0) };
    static COMPLETE_STYLE_UPDATE_DEPTH: Cell<u32> = const { Cell::new(0) };
}

#[inline]
pub(crate) fn bump(op: FfiOp) {
    COUNTERS[op as usize].fetch_add(1, Ordering::Relaxed);
}

#[inline]
pub(crate) fn bump_cpp_callback(op: FfiOp) {
    bump(op);
    COMPUTED_STYLE_BUILD_DEPTH.with(|depth| {
        if depth.get() != 0 {
            bump(FfiOp::ComputedStyleBuildCppCallback);
        }
    });
    COMPLETE_STYLE_UPDATE_DEPTH.with(|depth| {
        if depth.get() == 0 {
            return;
        }

        bump(FfiOp::CompleteStyleUpdateCppCallback);
        let phase = match op {
            FfiOp::StringRetainReleaseCallback | FfiOp::AnimatedPropertiesRetainReleaseCallback => {
                bump(FfiOp::CompleteStyleUpdateOwnershipCppCallback);
                None
            }
            FfiOp::SelectorMetadataCallback
            | FfiOp::CascadeResolveUnresolvedCallback
            | FfiOp::CascadeParseSubstitutedCallback
            | FfiOp::CascadeSourceSlotCallback
            | FfiOp::CascadeCustomPropertyBatchCallback => Some(FfiOp::CompleteStyleUpdateCascadeCallback),
            FfiOp::LonghandStoreBatchCallback => Some(FfiOp::CompleteStyleUpdateLonghandCallback),
            FfiOp::AnimationComputeBatchCallback => Some(FfiOp::CompleteStyleUpdateAnimationCallback),
            FfiOp::ShorthandSetLonghandCallback => Some(FfiOp::CompleteStyleUpdateShorthandCallback),
            _ => None,
        };
        if let Some(phase) = phase {
            bump(FfiOp::CompleteStyleUpdateSemanticCppCallback);
            bump(phase);
        }
    });
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
pub extern "C" fn rust_style_ffi_complete_style_update_end() {
    COMPLETE_STYLE_UPDATE_DEPTH.with(|depth| {
        depth.set(
            depth
                .get()
                .checked_sub(1)
                .expect("unbalanced complete style update scope"),
        );
    });
}

/// Marks the complete C++-orchestrated computed-value build. This temporary
/// bracket makes every Rust-to-C++ callback inside the path measurable while
/// the build itself migrates into the Rust style engine.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_ffi_computed_style_build_begin() {
    COMPUTED_STYLE_BUILD_DEPTH.with(|depth| {
        depth.set(depth.get().checked_add(1).expect("computed style build depth overflow"));
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_ffi_computed_style_build_end() {
    COMPUTED_STYLE_BUILD_DEPTH.with(|depth| {
        depth.set(
            depth
                .get()
                .checked_sub(1)
                .expect("unbalanced computed style build scope"),
        );
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
