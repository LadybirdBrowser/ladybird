/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PerformanceMeasure, https://w3c.github.io/user-timing/#performancemeasure

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::Value;

use crate::performance_timeline::performance_entry::CppPerformanceEntry;

unsafe extern "C" {
    fn web_user_timing_performance_measure_create(
        measure_name: usize,
        start_time: f64,
        duration: f64,
        detail: u64,
    ) -> *mut c_void;
}

// https://w3c.github.io/user-timing/#extensions-performance-interface
#[derive(Default)]
pub(crate) struct PerformanceMeasureOptions {
    pub(crate) detail: Option<Value>,
    pub(crate) start: Option<DOMStringOrDouble>,
    pub(crate) duration: Option<f64>,
    pub(crate) end: Option<DOMStringOrDouble>,
}

/// (DOMString or DOMHighResTimeStamp)
pub(crate) enum DOMStringOrDouble {
    DOMString(Utf16String),
    Double(f64),
}

pub(crate) fn create_performance_measure(
    measure_name: &Utf16String,
    start_time: f64,
    duration: f64,
    detail: Value,
) -> CppPerformanceEntry {
    // SAFETY: The detail is live, the name is lent for the call, and LibWeb returns a live entry.
    unsafe {
        CppPerformanceEntry::from_raw(web_user_timing_performance_measure_create(
            measure_name.raw_identity(),
            start_time,
            duration,
            detail.to_raw(),
        ))
    }
}
