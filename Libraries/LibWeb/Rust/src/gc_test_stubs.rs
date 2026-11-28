/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Stand-ins for the parts of LibGC's C interface that tracing and weak references reach, for the unit tests, which
//! do not link LibGC.

use core::ffi::c_void;

use libgc_api::capi::{GCMarker, GCVisitor, GCWeakImpl};
use libgc_api::layout::WEAK_IMPL_POINTER_OFFSET;

#[unsafe(no_mangle)]
extern "C" fn gc_visitor_visit_cell(_visitor: *mut GCVisitor, _cell: *mut c_void) {
    unreachable!("no unit test visits cells");
}

#[unsafe(no_mangle)]
extern "C" fn gc_visitor_visit_values(_visitor: *mut GCVisitor, _values: *const u64, _count: usize) {
    unreachable!("no unit test visits values");
}

#[unsafe(no_mangle)]
extern "C" fn gc_visitor_visit_ephemeron(_visitor: *mut GCVisitor, _key: *mut c_void, _value: *const u64) {
    unreachable!("no unit test visits ephemerons");
}

#[unsafe(no_mangle)]
extern "C" fn gc_marker_visit_cell(_marker: *mut GCMarker, _cell: *mut c_void) {
    unreachable!("no unit test marks");
}

#[unsafe(no_mangle)]
extern "C" fn gc_marker_visit_member(_marker: *mut GCMarker, _slot: *const u32) {
    unreachable!("no unit test marks");
}

#[unsafe(no_mangle)]
extern "C" fn gc_marker_visit_values(_marker: *mut GCMarker, _values: *const u64, _count: usize) {
    unreachable!("no unit test marks");
}

#[unsafe(no_mangle)]
extern "C" fn gc_marker_visit_owned_member(_marker: *mut GCMarker, _slot: *const u32) {
    unreachable!("a recording visitor has no marker");
}

#[unsafe(no_mangle)]
extern "C" fn gc_marker_visit_owned_values(_marker: *mut GCMarker, _values: *const u64, _count: usize) {
    unreachable!("a recording visitor has no marker");
}

/// A GC::WeakImpl as the stand-ins make them, which lives for the rest of the process.
#[repr(C)]
struct WeakImplForTest {
    reference_count: usize,
    state: u32,
    value: *mut c_void,
}

const _: () = assert!(core::mem::offset_of!(WeakImplForTest, value) == WEAK_IMPL_POINTER_OFFSET);

/// A weak reference to the cell at `cell` whose impl is never freed, or a cleared one for null, for tests that lay
/// out the GC::Weak entries C++ keeps.
pub fn leaked_weak_impl(cell: *mut c_void) -> *mut GCWeakImpl {
    Box::into_raw(Box::new(WeakImplForTest {
        reference_count: 1,
        state: 0,
        value: cell,
    }))
    .cast()
}

#[unsafe(no_mangle)]
extern "C" fn gc_cell_create_weak_impl(cell: *mut c_void) -> *mut GCWeakImpl {
    leaked_weak_impl(cell)
}

#[unsafe(no_mangle)]
extern "C" fn gc_weak_impl_ref(weak_impl: *mut GCWeakImpl) {
    // SAFETY: The stand-ins only see the impls they made.
    unsafe { (*weak_impl.cast::<WeakImplForTest>()).reference_count += 1 };
}

#[unsafe(no_mangle)]
extern "C" fn gc_weak_impl_unref(weak_impl: *mut GCWeakImpl) {
    // SAFETY: The stand-ins only see the impls they made.
    unsafe { (*weak_impl.cast::<WeakImplForTest>()).reference_count -= 1 };
}
