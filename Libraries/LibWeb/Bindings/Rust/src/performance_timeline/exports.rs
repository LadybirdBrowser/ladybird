/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which LibWeb's C++ reaches the performance timeline of a global object.

use core::ffi::c_void;

use crate::bindings::interface::{cell_into_abi, realm_arguments};
use crate::performance_timeline::performance_entry::CppPerformanceEntry;
use crate::performance_timeline::performance_timeline::PerformanceTimeline;

/// https://w3c.github.io/hr-time/#dom-windoworworkerglobalscope-performance
///
/// # Safety
///
/// `vm` must be the live VM, and `realm` the live realm of a Window or a WorkerGlobalScope.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_performance_timeline_performance(vm: *mut c_void, realm: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    cell_into_abi(PerformanceTimeline::of(vm, realm).performance(vm))
}

/// https://w3c.github.io/performance-timeline/#dfn-queue-a-performanceentry
///
/// # Safety
///
/// `vm` must be the live VM, `realm` the live realm of a Window or a WorkerGlobalScope, and `entry` a live
/// PerformanceEntry whose relevant global object is that of the realm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_performance_timeline_queue_performance_entry(
    vm: *mut c_void,
    realm: *mut c_void,
    entry: *mut c_void,
) {
    // SAFETY: The caller passes the live VM, a live realm and a live entry.
    let ((vm, realm), entry) = unsafe { (realm_arguments(vm, realm), CppPerformanceEntry::from_raw(entry)) };
    PerformanceTimeline::of(vm, realm).queue_performance_entry(vm, &entry);
}

/// https://w3c.github.io/performance-timeline/#dfn-queue-a-performanceentry
///
/// # Safety
///
/// As for web_rust_performance_timeline_queue_performance_entry().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_performance_timeline_add_performance_entry(
    vm: *mut c_void,
    realm: *mut c_void,
    entry: *mut c_void,
    check_if_performance_buffer_is_full: bool,
) {
    // SAFETY: The caller passes the live VM, a live realm and a live entry.
    let ((vm, realm), entry) = unsafe { (realm_arguments(vm, realm), CppPerformanceEntry::from_raw(entry)) };
    PerformanceTimeline::of(vm, realm).add_performance_entry(vm, &entry, check_if_performance_buffer_is_full);
}

/// https://w3c.github.io/resource-timing/#dfn-add-a-performanceresourcetiming-entry
///
/// # Safety
///
/// As for web_rust_performance_timeline_queue_performance_entry(), with a PerformanceResourceTiming.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_performance_timeline_add_resource_timing_entry(
    vm: *mut c_void,
    realm: *mut c_void,
    entry: *mut c_void,
) {
    // SAFETY: The caller passes the live VM, a live realm and a live entry.
    let ((vm, realm), entry) = unsafe { (realm_arguments(vm, realm), CppPerformanceEntry::from_raw(entry)) };
    PerformanceTimeline::of(vm, realm).add_resource_timing_entry(vm, &entry);
}
