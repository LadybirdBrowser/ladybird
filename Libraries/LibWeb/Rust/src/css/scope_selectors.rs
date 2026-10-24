/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::ffi_support::OwnedArc;
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
pub extern "C" fn rust_scope_selectors_retain(
    selectors: crate::css::ffi_support::ArcRef<'_, ScopeSelectors>,
) -> *const ScopeSelectors {
    Arc::into_raw(selectors.to_arc())
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_scope_selectors_release(selectors: OwnedArc<ScopeSelectors>) {
    drop(selectors);
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_scope_selectors_start(selectors: &ScopeSelectors) -> *const c_void {
    selectors.start.as_ref().map_or(std::ptr::null(), Arc::as_ptr).cast()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_scope_selectors_end(selectors: &ScopeSelectors) -> *const c_void {
    selectors.end.as_ref().map_or(std::ptr::null(), Arc::as_ptr).cast()
}
