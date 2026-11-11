/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PerformanceMark, https://w3c.github.io/user-timing/#performancemark

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Gc, RawNativeFunctionResult, Realm, Value};

use crate::bindings::WebIdlResult;
use crate::performance_timeline::performance_entry::CppPerformanceEntry;

unsafe extern "C" {
    fn web_user_timing_performance_mark_create(
        global_object: *mut c_void,
        mark_name: usize,
        has_detail: bool,
        detail: u64,
        has_start_time: bool,
        start_time: f64,
        entry: *mut *mut c_void,
    ) -> RawNativeFunctionResult;
}

// https://w3c.github.io/user-timing/#ref-for-dom-performancemarkoptions-1
pub(crate) struct PerformanceMarkOptions {
    pub(crate) detail: Option<Value>,
    pub(crate) start_time: Option<f64>,
}

/// https://w3c.github.io/user-timing/#dfn-performancemark-constructor
pub(crate) fn run_the_performance_mark_constructor(
    realm: Gc<Realm>,
    mark_name: &Utf16String,
    mark_options: &PerformanceMarkOptions,
) -> WebIdlResult<CppPerformanceEntry> {
    let mut entry = core::ptr::null_mut();
    // SAFETY: The global object and the detail are live, the name is lent for the call, and LibWeb writes a live entry.
    unsafe {
        web_user_timing_performance_mark_create(
            realm.global_object().as_ptr().cast(),
            mark_name.raw_identity(),
            mark_options.detail.is_some(),
            mark_options.detail.unwrap_or(Value::UNDEFINED).to_raw(),
            mark_options.start_time.is_some(),
            mark_options.start_time.unwrap_or_default(),
            &raw mut entry,
        )
    }
    .into_completion()?;
    // SAFETY: LibWeb wrote the live entry that it created.
    Ok(unsafe { CppPerformanceEntry::from_raw(entry) })
}
