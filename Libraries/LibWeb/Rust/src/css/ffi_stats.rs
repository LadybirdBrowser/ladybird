/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! C++ releases that the style core defers until a complete style update ends, and, in unit tests, counts of the
//! callbacks the core makes into C++.

use std::cell::RefCell;

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
    #[cfg(test)]
    CPP_CALLBACK_COUNT.set(CPP_CALLBACK_COUNT.get() + 1);
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
