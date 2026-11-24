/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which LibWeb's C++ reaches the performance timeline of a global object.

use core::ffi::c_void;

use libjs_rust::api::{Gc, Realm, Vm};

use crate::bindings::interface::{InterfaceCell, cell_into_abi};
use crate::performance_timeline::performance_entry::AnyPerformanceEntry;
use crate::performance_timeline::performance_timeline::PerformanceTimeline;

/// https://w3c.github.io/hr-time/#dom-windoworworkerglobalscope-performance
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_performance_timeline_performance(vm: &Vm, realm: Gc<Realm>) -> *mut c_void {
    cell_into_abi(PerformanceTimeline::of(vm, realm).performance(vm))
}

/// https://w3c.github.io/performance-timeline/#dfn-queue-a-performanceentry, for an entry whose relevant global object
/// is that of the realm, a Window or a WorkerGlobalScope.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_performance_timeline_queue_performance_entry(
    vm: &Vm,
    realm: Gc<Realm>,
    entry: &InterfaceCell,
) {
    let entry = AnyPerformanceEntry::from_abi(entry);
    PerformanceTimeline::of(vm, realm).queue_performance_entry(vm, &entry);
}

/// https://w3c.github.io/performance-timeline/#dfn-queue-a-performanceentry
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_performance_timeline_add_performance_entry(
    vm: &Vm,
    realm: Gc<Realm>,
    entry: &InterfaceCell,
    check_if_performance_buffer_is_full: bool,
) {
    let entry = AnyPerformanceEntry::from_abi(entry);
    PerformanceTimeline::of(vm, realm).add_performance_entry(vm, &entry, check_if_performance_buffer_is_full);
}
