/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The cascaded property store: the per-element result of applying the CSS
//! cascade, one winning declaration list per longhand.
//!
//! This is the Rust backing for the C++ CascadedProperties shell. Entries own
//! strong references to their C++ StyleValue shells and layer name strings.
//! The GC-managed declaration sources stay on the C++ side, pinned in a slot
//! table of weak references; each entry carries its slot index and the C++
//! shell resolves a slot back to the source objects on demand.

use std::collections::HashMap;
use std::ffi::c_void;

use crate::abort_on_panic;
use crate::property_metadata::LAST_LONGHAND_PROPERTY_ID;
use crate::style_value::RetainedStyleValue;
use crate::style_value::RetainedUtf16FlyString;

/// Mirrors the C++ `enum class CascadeOrigin : u8`; the C++ side static_asserts
/// that every discriminant matches.
/// https://drafts.csswg.org/css-cascade/#origin
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum CascadeOrigin {
    Author,
    /// https://drafts.csswg.org/css-cascade/#author-presentational-hint-origin
    AuthorPresentationalHint,
    User,
    UserAgent,
    Animation,
    Transition,
}

/// A layer name is an interned fly string, so identity of the raw
/// representation is string equality. `None` is the unlayered form.
struct LayerName(Option<RetainedUtf16FlyString>);

impl LayerName {
    fn matches(&self, has_layer_name: bool, layer_name_raw: usize) -> bool {
        match &self.0 {
            Some(layer_name) => has_layer_name && layer_name.raw() == layer_name_raw,
            None => !has_layer_name,
        }
    }

    fn equals(&self, other: &LayerName) -> bool {
        match &other.0 {
            Some(layer_name) => self.matches(true, layer_name.raw()),
            None => self.matches(false, 0),
        }
    }
}

struct Entry {
    value: RetainedStyleValue,
    important: bool,
    cascade_index: u64,
    origin: CascadeOrigin,
    layer_name: LayerName,
    /// Pointer identity of the source shadow root at the time the declaration
    /// was applied, used only to match entries from the same tree context.
    source_shadow_root_identity: usize,
    /// Index into the C++ shell's table of GC-weak declaration sources.
    source_slot: u32,
}

const CONTAINED_BITMAP_WORDS: usize = (LAST_LONGHAND_PROPERTY_ID as usize + 1).div_ceil(64);

pub struct CascadedPropertyStore {
    entries: HashMap<u16, Vec<Entry>>,
    next_cascade_index: u64,
    next_source_slot: u32,
    free_source_slots: Vec<u32>,
    /// One bit per longhand property identifier, so the hot "is there any
    /// cascaded value at all" checks skip the hash map.
    contained: [u64; CONTAINED_BITMAP_WORDS],
}

impl CascadedPropertyStore {
    fn new() -> Self {
        Self {
            entries: HashMap::new(),
            next_cascade_index: 0,
            next_source_slot: 0,
            free_source_slots: Vec::new(),
            contained: [0; CONTAINED_BITMAP_WORDS],
        }
    }

    fn contains(&self, property_id: u16) -> bool {
        let index = property_id as usize;
        debug_assert!(index <= LAST_LONGHAND_PROPERTY_ID as usize);
        self.contained[index / 64] & (1 << (index % 64)) != 0
    }

    fn set_contained(&mut self, property_id: u16, contained: bool) {
        let index = property_id as usize;
        debug_assert!(index <= LAST_LONGHAND_PROPERTY_ID as usize);
        if contained {
            self.contained[index / 64] |= 1 << (index % 64);
        } else {
            self.contained[index / 64] &= !(1 << (index % 64));
        }
    }

    fn last_entry(&self, property_id: u16) -> Option<&Entry> {
        if !self.contains(property_id) {
            return None;
        }
        self.entries.get(&property_id).and_then(|entries| entries.last())
    }

    fn allocate_source_slot(&mut self) -> u32 {
        if let Some(slot) = self.free_source_slots.pop() {
            return slot;
        }
        let slot = self.next_source_slot;
        self.next_source_slot += 1;
        slot
    }

    fn set_property(
        &mut self,
        property_id: u16,
        value: RetainedStyleValue,
        important: bool,
        origin: CascadeOrigin,
        layer_name: LayerName,
        source_shadow_root_identity: usize,
    ) -> i64 {
        self.set_contained(property_id, true);

        let cascade_index = self.next_cascade_index;
        self.next_cascade_index += 1;

        let entries = self.entries.entry(property_id).or_default();
        for entry in entries.iter_mut().rev() {
            if entry.origin == origin
                && entry.layer_name.equals(&layer_name)
                && entry.source_shadow_root_identity == source_shadow_root_identity
            {
                if entry.important && !important {
                    return -1;
                }
                entry.value = value;
                entry.important = important;
                entry.cascade_index = cascade_index;
                return entry.source_slot as i64;
            }
        }

        let source_slot = self.allocate_source_slot();
        self.entries.get_mut(&property_id).unwrap().push(Entry {
            value,
            important,
            cascade_index,
            origin,
            layer_name,
            source_shadow_root_identity,
            source_slot,
        });
        source_slot as i64
    }

    fn remove_matching_entries(&mut self, property_id: u16, mut matches: impl FnMut(&Entry) -> bool) {
        let Some(entries) = self.entries.get_mut(&property_id) else {
            return;
        };
        entries.retain(|entry| !matches(entry));
        if entries.is_empty() {
            self.entries.remove(&property_id);
            self.set_contained(property_id, false);
        }
    }

    fn revert_property(&mut self, property_id: u16, important: bool, origin: CascadeOrigin) {
        let mut freed_slots = Vec::new();
        self.remove_matching_entries(property_id, |entry| {
            // https://drafts.csswg.org/css-cascade-5/#author-presentational-hint-origin
            // For the purpose of cascading this author presentational hint origin is treated as an independent origin,
            // but for the purpose of the revert keyword it is considered part of the author origin.
            let origin_matches = entry.origin == origin
                || (origin == CascadeOrigin::Author && entry.origin == CascadeOrigin::AuthorPresentationalHint);
            let matches = entry.important == important && origin_matches;
            if matches {
                freed_slots.push(entry.source_slot);
            }
            matches
        });
        self.free_source_slots.extend(freed_slots);
    }

    fn revert_layer_property(
        &mut self,
        property_id: u16,
        important: bool,
        origin: CascadeOrigin,
        has_layer_name: bool,
        layer_name_raw: usize,
        source_shadow_root_identity: usize,
    ) {
        let mut freed_slots = Vec::new();
        self.remove_matching_entries(property_id, |entry| {
            let matches = entry.important == important
                && entry.origin == origin
                && entry.source_shadow_root_identity == source_shadow_root_identity
                && entry.layer_name.matches(has_layer_name, layer_name_raw);
            if matches {
                freed_slots.push(entry.source_slot);
            }
            matches
        });
        self.free_source_slots.extend(freed_slots);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_cascaded_properties_create() -> *mut CascadedPropertyStore {
    abort_on_panic(|| Box::into_raw(Box::new(CascadedPropertyStore::new())))
}

/// # Safety
/// `store` must be a pointer returned by `rust_cascaded_properties_create` that has not been
/// destroyed yet; no references into the store may outlive this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_cascaded_properties_destroy(store: *mut CascadedPropertyStore) {
    abort_on_panic(|| drop(unsafe { Box::from_raw(store) }));
}

/// Applies one declaration to the store. Takes ownership of one strong reference to `value` and,
/// when `has_layer_name` is set, of one reference to the layer name fly string.
///
/// Returns the source slot the C++ shell must (re)assign its GC-weak declaration source pair to,
/// or -1 when the declaration lost to an existing important entry and was not stored.
///
/// # Safety
/// `store` must be a valid store, `value` a leaked strong StyleValue reference, and
/// `layer_name_raw` a leaked fly string reference when `has_layer_name` is set.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_cascaded_properties_set_property(
    store: *mut CascadedPropertyStore,
    property_id: u16,
    value: *const c_void,
    important: bool,
    origin: CascadeOrigin,
    has_layer_name: bool,
    layer_name_raw: usize,
    source_shadow_root_identity: usize,
) -> i64 {
    abort_on_panic(|| {
        let value = unsafe { RetainedStyleValue::from_shell_pointer(value) };
        let layer_name = LayerName(has_layer_name.then(|| unsafe { RetainedUtf16FlyString::from_raw(layer_name_raw) }));
        unsafe { &mut *store }.set_property(
            property_id,
            value,
            important,
            origin,
            layer_name,
            source_shadow_root_identity,
        )
    })
}

/// # Safety
/// `store` must be a valid store.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_cascaded_properties_revert_property(
    store: *mut CascadedPropertyStore,
    property_id: u16,
    important: bool,
    origin: CascadeOrigin,
) {
    abort_on_panic(|| unsafe { &mut *store }.revert_property(property_id, important, origin));
}

/// The layer name is borrowed for comparison only; no reference is transferred.
///
/// # Safety
/// `store` must be a valid store.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_cascaded_properties_revert_layer_property(
    store: *mut CascadedPropertyStore,
    property_id: u16,
    important: bool,
    origin: CascadeOrigin,
    has_layer_name: bool,
    layer_name_raw: usize,
    source_shadow_root_identity: usize,
) {
    abort_on_panic(|| {
        unsafe { &mut *store }.revert_layer_property(
            property_id,
            important,
            origin,
            has_layer_name,
            layer_name_raw,
            source_shadow_root_identity,
        );
    });
}

/// Returns a borrowed pointer to the winning declaration's StyleValue shell, or null.
///
/// # Safety
/// `store` must be a valid store.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_cascaded_properties_property(
    store: *const CascadedPropertyStore,
    property_id: u16,
) -> *const c_void {
    abort_on_panic(|| match unsafe { &*store }.last_entry(property_id) {
        Some(entry) => entry.value.shell_pointer(),
        None => std::ptr::null(),
    })
}

/// Like `rust_cascaded_properties_property`, but also reports the declaration's importance.
///
/// # Safety
/// `store` must be a valid store and `out_important` a valid pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_cascaded_properties_style_property(
    store: *const CascadedPropertyStore,
    property_id: u16,
    out_important: *mut bool,
) -> *const c_void {
    abort_on_panic(|| match unsafe { &*store }.last_entry(property_id) {
        Some(entry) => {
            unsafe { *out_important = entry.important };
            entry.value.shell_pointer()
        }
        None => std::ptr::null(),
    })
}

/// Returns whichever of the two properties has the higher-priority winning declaration.
/// A property with no cascaded value at all loses to one that has any.
///
/// # Safety
/// `store` must be a valid store.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_cascaded_properties_property_with_higher_priority(
    store: *const CascadedPropertyStore,
    first_property_id: u16,
    second_property_id: u16,
) -> u16 {
    abort_on_panic(|| {
        let store = unsafe { &*store };
        let Some(first_entry) = store.last_entry(first_property_id) else {
            return second_property_id;
        };
        let Some(second_entry) = store.last_entry(second_property_id) else {
            return first_property_id;
        };
        if first_entry.cascade_index >= second_entry.cascade_index {
            first_property_id
        } else {
            second_property_id
        }
    })
}

/// Returns the winning declaration's source slot, or -1 when the property has no cascaded value.
///
/// # Safety
/// `store` must be a valid store.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_cascaded_properties_source_slot(
    store: *const CascadedPropertyStore,
    property_id: u16,
) -> i64 {
    abort_on_panic(|| match unsafe { &*store }.last_entry(property_id) {
        Some(entry) => entry.source_slot as i64,
        None => -1,
    })
}
