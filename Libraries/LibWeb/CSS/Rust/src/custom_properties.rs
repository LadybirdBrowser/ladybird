/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Rust-owned custom-property data with the same structurally shared parent shape as the C++
//! `CustomPropertyData` shell.

use std::collections::HashMap;
use std::ffi::c_void;
use std::rc::Rc;

use crate::abort_on_panic;
use crate::style_value::RetainedStyleValue;
use crate::style_value::RetainedUtf16FlyString;

#[repr(C)]
pub struct FfiCustomPropertyStoreEntry {
    pub name_raw: usize,
    pub important: bool,
    pub shell: *const c_void,
    pub data: *const c_void,
}

struct CustomPropertyEntry {
    _name: RetainedUtf16FlyString,
    _value: RetainedStyleValue,
    important: bool,
    data: *const c_void,
}

pub struct CustomPropertyStore {
    own_values: HashMap<usize, CustomPropertyEntry>,
    parent: Option<Rc<CustomPropertyStore>>,
}

impl CustomPropertyStore {
    fn get(&self, name_raw: usize) -> Option<&CustomPropertyEntry> {
        self.own_values
            .get(&name_raw)
            .or_else(|| self.parent.as_ref()?.get(name_raw))
    }
}

/// Creates one Rust store node. Each entry name transfers one leaked fly-string reference;
/// value shells are borrowed and retained by Rust. The parent is another Rc raw pointer.
///
/// # Safety
/// `entries` must point at `entry_count` valid entries and `parent` must be null or a pointer
/// returned by this function that remains live for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_create(
    entries: *const FfiCustomPropertyStoreEntry,
    entry_count: usize,
    parent: *const c_void,
) -> *const c_void {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CustomPropertyStoreLifecycleEntry);
    abort_on_panic(|| {
        let entries = if entry_count == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(entries, entry_count) }
        };
        let parent = if parent.is_null() {
            None
        } else {
            let parent = parent.cast::<CustomPropertyStore>();
            unsafe { Rc::increment_strong_count(parent) };
            Some(unsafe { Rc::from_raw(parent) })
        };
        let own_values = entries
            .iter()
            .map(|entry| {
                (
                    entry.name_raw,
                    CustomPropertyEntry {
                        _name: unsafe { RetainedUtf16FlyString::from_leaked_raw(entry.name_raw) },
                        _value: unsafe { RetainedStyleValue::from_borrowed_shell_pointer(entry.shell) },
                        important: entry.important,
                        data: entry.data,
                    },
                )
            })
            .collect();
        Rc::into_raw(Rc::new(CustomPropertyStore { own_values, parent })).cast()
    })
}

/// Releases one store reference returned by `rust_custom_property_store_create`.
///
/// # Safety
/// `store` must be a non-null pointer returned by `rust_custom_property_store_create` that has
/// not already been released.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_destroy(store: *const c_void) {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CustomPropertyStoreLifecycleEntry);
    abort_on_panic(|| drop(unsafe { Rc::from_raw(store.cast::<CustomPropertyStore>()) }));
}

#[repr(C)]
pub struct FfiCustomPropertyStoreValue {
    pub found: bool,
    pub important: bool,
    pub shell: *const c_void,
    pub data: *const c_void,
}

/// Looks up a custom property through the structurally shared parent chain.
///
/// # Safety
/// `store` must be a live pointer returned by `rust_custom_property_store_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_get(
    store: *const c_void,
    name_raw: usize,
) -> FfiCustomPropertyStoreValue {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CustomPropertyStoreQueryEntry);
    abort_on_panic(|| {
        let store = unsafe { &*store.cast::<CustomPropertyStore>() };
        let Some(entry) = store.get(name_raw) else {
            return FfiCustomPropertyStoreValue {
                found: false,
                important: false,
                shell: std::ptr::null(),
                data: std::ptr::null(),
            };
        };
        FfiCustomPropertyStoreValue {
            found: true,
            important: entry.important,
            shell: entry._value.shell_pointer(),
            data: entry.data,
        }
    })
}
