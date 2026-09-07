/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::selector_parser::RustParsedSelectorList;
use std::ffi::c_void;
use std::sync::Arc;

pub struct ScopeSelectors {
    pub(crate) start: Option<Arc<RustParsedSelectorList>>,
    pub(crate) end: Option<Arc<RustParsedSelectorList>>,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<ScopeSelectors>();
};

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_scope_selectors_retain(selectors: *const ScopeSelectors) -> *const ScopeSelectors {
    assert!(!selectors.is_null());
    unsafe { Arc::increment_strong_count(selectors) };
    selectors
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_scope_selectors_release(selectors: *const ScopeSelectors) {
    if !selectors.is_null() {
        drop(unsafe { Arc::from_raw(selectors) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_scope_selectors_start(selectors: &ScopeSelectors) -> *const c_void {
    selectors.start.as_ref().map_or(std::ptr::null(), Arc::as_ptr).cast()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_scope_selectors_end(selectors: &ScopeSelectors) -> *const c_void {
    selectors.end.as_ref().map_or(std::ptr::null(), Arc::as_ptr).cast()
}
