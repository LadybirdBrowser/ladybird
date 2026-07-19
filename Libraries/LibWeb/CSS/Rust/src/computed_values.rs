/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Rust ownership of the ComputedValues style group payloads.
//!
//! Each style value group payload is a C++ struct that Rust treats as an opaque
//! blob: C++ registers a vtable per group with the payload size, alignment and
//! callbacks for default-construction, copy-construction and destruction, in
//! the same way Stylo drives Gecko's nsStyle* structs. Rust owns allocation,
//! layout and destruction; reference counting happens through an atomic header
//! placed immediately before the payload, which the C++ side reads and updates
//! inline so that sharing a payload never crosses the FFI boundary.
//!
//! Layout contract with the C++ side (StyleStructRef):
//!
//!   [ ArcHeader | payload ]
//!               ^-- pointers exchanged over FFI point at the payload
//!
//! The header occupies max(size_of::<usize>(), payload alignment) bytes so the
//! payload stays properly aligned; the reference count lives in the first
//! usize of the header. A reference count of STYLE_GROUP_STATIC_REFCOUNT marks
//! an intentionally leaked payload (the per-group defaults) that must never be
//! reference counted or freed.

use std::alloc::{Layout, alloc, dealloc};
use std::ffi::c_void;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicUsize, Ordering};

use crate::abort_on_panic;

/// Reference count value marking an intentionally leaked payload.
pub const STYLE_GROUP_STATIC_REFCOUNT: usize = usize::MAX;

/// Layout of the inherited box style value group.
///
/// This is the source of truth for the group's payload layout: C++ derives its
/// group struct from the cbindgen mirror of this type, adding the initial
/// values and typed accessors on top. The fields hold C++ `enum class : u8`
/// values that Rust stores as opaque bytes, keeping the enum definitions
/// single-sourced in C++.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct InheritedBoxValues {
    pub visibility: u8,
    pub direction: u8,
    pub writing_mode: u8,
    pub content_visibility: u8,
    pub image_rendering: u8,
}

/// Size, alignment and lifecycle callbacks for one style value group type.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StyleGroupVTable {
    pub size: usize,
    pub align: usize,
    pub default_construct: unsafe extern "C" fn(payload: *mut c_void),
    pub copy_construct: unsafe extern "C" fn(payload: *mut c_void, source: *const c_void),
    pub destruct: unsafe extern "C" fn(payload: *mut c_void),
}

// SAFETY: The function pointers are stateless C++ callbacks and the plain
// integers are immutable after registration.
unsafe impl Send for StyleGroupVTable {}
unsafe impl Sync for StyleGroupVTable {}

static REGISTRY: OnceLock<Box<[StyleGroupVTable]>> = OnceLock::new();

fn vtable(group_index: usize) -> &'static StyleGroupVTable {
    let registry = REGISTRY.get().expect("style groups used before registration");
    &registry[group_index]
}

fn header_size(align: usize) -> usize {
    align.max(size_of::<usize>())
}

fn allocation_layout(vtable: &StyleGroupVTable) -> Layout {
    let align = vtable.align.max(align_of::<usize>());
    Layout::from_size_align(header_size(vtable.align) + vtable.size, align).expect("style group layout overflow")
}

fn refcount_of(payload: *const c_void, align: usize) -> &'static AtomicUsize {
    // SAFETY: Every payload pointer handed out by this module is preceded by
    // its header, whose first usize is the reference count.
    unsafe {
        let header = (payload as *const u8).sub(header_size(align)) as *const AtomicUsize;
        &*header
    }
}

fn allocate_payload(vtable: &StyleGroupVTable, initial_refcount: usize) -> *mut c_void {
    // SAFETY: The layout is never zero-sized (the header is at least a usize).
    unsafe {
        let allocation = alloc(allocation_layout(vtable));
        if allocation.is_null() {
            std::process::abort();
        }
        let header = allocation as *mut AtomicUsize;
        (*header).store(initial_refcount, Ordering::Relaxed);
        allocation.add(header_size(vtable.align)) as *mut c_void
    }
}

/// Registers the style group vtables and builds the intentionally leaked
/// default payload for every group, written to `out_default_payloads`.
/// Must be called exactly once, before any other function in this module.
///
/// # Safety
/// `vtables` must point at `count` valid vtables and `out_default_payloads`
/// at space for `count` pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_registry_register(
    vtables: *const StyleGroupVTable,
    count: usize,
    out_default_payloads: *mut *const c_void,
) {
    abort_on_panic(|| unsafe {
        let tables: Box<[StyleGroupVTable]> = std::slice::from_raw_parts(vtables, count).into();
        for (index, table) in tables.iter().enumerate() {
            assert!(table.align.is_power_of_two());
            let payload = allocate_payload(table, STYLE_GROUP_STATIC_REFCOUNT);
            (table.default_construct)(payload);
            *out_default_payloads.add(index) = payload;
        }
        assert!(REGISTRY.set(tables).is_ok(), "style group registry registered twice");
    });
}

/// Allocates a new payload for `group_index` with a reference count of one,
/// copy-constructed from `source`.
///
/// # Safety
/// `source` must be a valid payload of the same group type.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_clone(group_index: usize, source: *const c_void) -> *mut c_void {
    abort_on_panic(|| unsafe {
        let table = vtable(group_index);
        let payload = allocate_payload(table, 1);
        (table.copy_construct)(payload, source);
        payload
    })
}

/// Destroys and deallocates a payload whose reference count has reached zero.
///
/// # Safety
/// `payload` must be a payload of the given group type with no remaining
/// references, and must not be a static default payload.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_free(group_index: usize, payload: *mut c_void) {
    abort_on_panic(|| unsafe {
        let table = vtable(group_index);
        debug_assert!(refcount_of(payload, table.align).load(Ordering::Relaxed) == 0);
        (table.destruct)(payload);
        let allocation = (payload as *mut u8).sub(header_size(table.align));
        dealloc(allocation, allocation_layout(table));
    });
}

/// Layout of the inherited table style value group.
///
/// The enum fields follow the opaque-byte convention; the border spacings are
/// raw CSSPixels fixed-point values.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct InheritedTableValues {
    pub border_collapse: u8,
    pub caption_side: u8,
    pub empty_cells: u8,
    pub border_spacing_horizontal: i32,
    pub border_spacing_vertical: i32,
}

/// Returns the typed view of an inherited table group payload.
///
/// # Safety
/// `payload` must be an inherited table group payload.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_as_inherited_table(payload: *const c_void) -> *const InheritedTableValues {
    payload as *const InheritedTableValues
}

/// Returns the typed view of an inherited box group payload.
///
/// This anchors the Rust-defined layout in the exported ABI so the cbindgen
/// mirror stays in the generated header; Rust-side style computation reads and
/// writes group payloads through these typed views.
///
/// # Safety
/// `payload` must be an inherited box group payload.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_as_inherited_box(payload: *const c_void) -> *const InheritedBoxValues {
    payload as *const InheritedBoxValues
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize as TestCounter;

    static LIVE: TestCounter = TestCounter::new(0);

    unsafe extern "C" fn test_default_construct(payload: *mut c_void) {
        unsafe { *(payload as *mut u64) = 7 };
        LIVE.fetch_add(1, Ordering::Relaxed);
    }
    unsafe extern "C" fn test_copy_construct(payload: *mut c_void, source: *const c_void) {
        unsafe { *(payload as *mut u64) = *(source as *const u64) };
        LIVE.fetch_add(1, Ordering::Relaxed);
    }
    unsafe extern "C" fn test_destruct(_payload: *mut c_void) {
        LIVE.fetch_sub(1, Ordering::Relaxed);
    }

    #[test]
    fn payload_lifecycle() {
        let vtables = [StyleGroupVTable {
            size: size_of::<u64>(),
            align: align_of::<u64>(),
            default_construct: test_default_construct,
            copy_construct: test_copy_construct,
            destruct: test_destruct,
        }];
        let mut defaults = [std::ptr::null::<c_void>(); 1];
        unsafe {
            rust_style_group_registry_register(vtables.as_ptr(), 1, defaults.as_mut_ptr());
            let default_payload = defaults[0];
            assert_eq!(*(default_payload as *const u64), 7);
            assert_eq!(
                refcount_of(default_payload, align_of::<u64>()).load(Ordering::Relaxed),
                STYLE_GROUP_STATIC_REFCOUNT
            );

            let clone = rust_style_group_clone(0, default_payload);
            assert_eq!(*(clone as *const u64), 7);
            let refcount = refcount_of(clone, align_of::<u64>());
            assert_eq!(refcount.load(Ordering::Relaxed), 1);

            refcount.store(0, Ordering::Relaxed);
            rust_style_group_free(0, clone);
            assert_eq!(LIVE.load(Ordering::Relaxed), 1);
        }
    }
}
