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
    SelectorMatchEntry => "selectorMatchEntries",
    CascadeOriginDriverEntry => "cascadeOriginDriverEntries",
    CascadeApplyDeclarationListEntry => "cascadeApplyDeclarationListEntries",
    CascadedStoreQueryEntry => "cascadedStoreQueryEntries",
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
    StyleGroupCloneEntry => "styleGroupCloneEntries",
    StyleGroupFreeEntry => "styleGroupFreeEntries",
    // Callbacks: Rust -> C++.
    SelectorSimpleSelectorCallback => "selectorSimpleSelectorCallbacks",
    SelectorTreeNavigationCallback => "selectorTreeNavigationCallbacks",
    SelectorMetadataCallback => "selectorMetadataCallbacks",
    CascadeStageCallback => "cascadeStageCallbacks",
    CascadePropertyDisallowedCallback => "cascadePropertyDisallowedCallbacks",
    CascadeResolveUnresolvedCallback => "cascadeResolveUnresolvedCallbacks",
    CascadeDataOfCallback => "cascadeDataOfCallbacks",
    CascadePendingSubstitutionCallback => "cascadePendingSubstitutionCallbacks",
    CascadeSourceSlotCallback => "cascadeSourceSlotCallbacks",
    ShorthandSetLonghandCallback => "shorthandSetLonghandCallbacks",
    LonghandComputeAndStoreCallback => "longhandComputeAndStoreCallbacks",
    LonghandParentValueFetchCallback => "longhandParentValueFetchCallbacks",
    LonghandIndependenceFallbackCallback => "longhandIndependenceFallbackCallbacks",
    LonghandWritingModeCallback => "longhandWritingModeCallbacks",
    CalcSerializationCallback => "calcSerializationCallbacks",
    StyleValueShellRetainCallback => "styleValueShellRetainCallbacks",
    StyleValueShellReleaseCallback => "styleValueShellReleaseCallbacks",
    StringRetainReleaseCallback => "stringRetainReleaseCallbacks",
}

static COUNTERS: [AtomicU64; FFI_OP_COUNT] = [const { AtomicU64::new(0) }; FFI_OP_COUNT];

#[inline]
pub(crate) fn bump(op: FfiOp) {
    COUNTERS[op as usize].fetch_add(1, Ordering::Relaxed);
}

#[inline]
pub(crate) fn bump_by(op: FfiOp, count: u64) {
    COUNTERS[op as usize].fetch_add(count, Ordering::Relaxed);
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
