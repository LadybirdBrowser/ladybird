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
use std::hash::BuildHasherDefault;
use std::hash::Hasher;

use crate::abort_on_panic;
use crate::property_metadata::LAST_LONGHAND_PROPERTY_ID;
use crate::style_compute::expand_shorthands_with;
use crate::style_value::RetainedStyleValue;
use crate::style_value::RetainedUtf16FlyString;
use crate::style_value::StyleValueData;

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
    /// The Rust-owned data of `value`, recorded so the property computation
    /// driver can read winning declarations without any shell interaction.
    value_data: *const c_void,
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

/// A trivial multiplicative hasher for the store's small integer keys; the
/// default SipHash is measurable overhead on the per-longhand queries.
#[derive(Default)]
struct PropertyIdHasher(u64);

impl Hasher for PropertyIdHasher {
    fn finish(&self) -> u64 {
        self.0
    }

    fn write(&mut self, _bytes: &[u8]) {
        unreachable!("property identifiers hash through write_u16");
    }

    fn write_u16(&mut self, value: u16) {
        self.0 = u64::from(value).wrapping_mul(0x9E37_79B9_7F4A_7C15);
    }
}

pub struct CascadedPropertyStore {
    entries: HashMap<u16, Vec<Entry>, BuildHasherDefault<PropertyIdHasher>>,
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
            entries: HashMap::default(),
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

    #[allow(clippy::too_many_arguments)]
    fn set_property(
        &mut self,
        property_id: u16,
        value: RetainedStyleValue,
        value_data: *const c_void,
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
                entry.value_data = value_data;
                entry.important = important;
                entry.cascade_index = cascade_index;
                return entry.source_slot as i64;
            }
        }

        let source_slot = self.allocate_source_slot();
        self.entries.get_mut(&property_id).unwrap().push(Entry {
            value,
            value_data,
            important,
            cascade_index,
            origin,
            layer_name,
            source_shadow_root_identity,
            source_slot,
        });
        source_slot as i64
    }

    /// The winning declaration for a property: its shell pointer, Rust-owned
    /// data, and importance.
    pub(crate) fn winning_declaration(&self, property_id: u16) -> Option<(*const c_void, *const c_void, bool)> {
        self.last_entry(property_id)
            .map(|entry| (entry.value.shell_pointer(), entry.value_data, entry.important))
    }

    /// Returns whichever of the two properties has the higher-priority winning
    /// declaration. A property with no cascaded value loses to one with any.
    pub(crate) fn property_with_higher_priority(&self, first_property_id: u16, second_property_id: u16) -> u16 {
        let Some(first_entry) = self.last_entry(first_property_id) else {
            return second_property_id;
        };
        let Some(second_entry) = self.last_entry(second_property_id) else {
            return first_property_id;
        };
        if first_entry.cascade_index >= second_entry.cascade_index {
            first_property_id
        } else {
            second_property_id
        }
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

/// Returns a borrowed pointer to the winning declaration's StyleValue shell, or null.
///
/// # Safety
/// `store` must be a valid store.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_cascaded_properties_property(
    store: *const CascadedPropertyStore,
    property_id: u16,
) -> *const c_void {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CascadedStoreQueryEntry);
    abort_on_panic(|| match unsafe { &*store }.last_entry(property_id) {
        Some(entry) => entry.value.shell_pointer(),
        None => std::ptr::null(),
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
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CascadedStoreQueryEntry);
    abort_on_panic(|| match unsafe { &*store }.last_entry(property_id) {
        Some(entry) => entry.source_slot as i64,
        None => -1,
    })
}

/// A declared property crossing into `rust_cascaded_properties_apply_property_list`: the
/// property identifier, its importance, and the borrowed value shell with its Rust-owned data.
#[repr(C)]
pub struct FfiCascadeDeclaration {
    pub property_id: u16,
    pub important: bool,
    pub shell: *const c_void,
    pub data: *const c_void,
}

/// Shell-level callbacks for applying a declaration list to the cascade. Values cross as
/// opaque C++ style value shells; the C++ side pins every value it creates until the
/// application returns.
#[repr(C)]
pub struct FfiCascadeApplicationCallbacks {
    pub context: *mut c_void,
    /// Whether the property may not be applied to the current element or pseudo-element.
    pub is_property_disallowed: unsafe extern "C" fn(context: *mut c_void, property_id: u16) -> bool,
    /// Resolves an unresolved (arbitrary-substitution) value; returns the pinned resolved shell.
    pub resolve_unresolved:
        unsafe extern "C" fn(context: *mut c_void, property_id: u16, shell: *const c_void) -> *const c_void,
    /// Returns the Rust-owned data of a C++ style value shell.
    pub data_of: unsafe extern "C" fn(context: *mut c_void, shell: *const c_void) -> *const c_void,
    /// Creates and pins a pending-substitution value wrapping the given value; returns its shell.
    pub create_pending_substitution: unsafe extern "C" fn(context: *mut c_void, shell: *const c_void) -> *const c_void,
    /// Records the current declaration source pair at the given store slot.
    pub assign_source_slot: unsafe extern "C" fn(context: *mut c_void, slot: u32),
}

/// Applies a declaration list to the cascade: filters by importance and applicability,
/// resolves arbitrary-substitution values through the parser callback, downgrades
/// invalid-at-computed-value-time declarations to unset, expands shorthands, and routes
/// each longhand to the store as a set, revert, or revert-layer.
///
/// # Safety
/// `store` must be a valid store, `declarations` must point to `declaration_count` valid
/// entries, `callbacks` must be a valid callback table, `unset_shell`/`unset_data` must be
/// a pinned unset keyword value, and `layer_name_raw` (borrowed) must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_cascaded_properties_apply_property_list(
    store: *mut CascadedPropertyStore,
    declarations: *const FfiCascadeDeclaration,
    declaration_count: usize,
    important: bool,
    origin: CascadeOrigin,
    has_layer_name: bool,
    layer_name_raw: usize,
    source_shadow_root_identity: usize,
    unset_shell: *const c_void,
    unset_data: *const c_void,
    callbacks: *const FfiCascadeApplicationCallbacks,
) {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CascadeApplyDeclarationListEntry);
    abort_on_panic(|| {
        let store = unsafe { &mut *store };
        let callbacks = unsafe { &*callbacks };
        let context = callbacks.context;
        let declarations = if declaration_count == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(declarations, declaration_count) }
        };

        let mut seen = [0u64; CONTAINED_BITMAP_WORDS];

        for declaration in declarations {
            if declaration.important != important {
                continue;
            }

            let declared_value = unsafe { &*(declaration.data as *const StyleValueData) };
            let declared_is_unresolved = matches!(declared_value, StyleValueData::Unresolved { .. });

            crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CascadePropertyDisallowedCallback);
            if unsafe { (callbacks.is_property_disallowed)(context, declaration.property_id) }
                && !declared_is_unresolved
            {
                continue;
            }

            if matches!(declared_value, StyleValueData::PendingSubstitution { .. }) {
                continue;
            }

            let mut shell = declaration.shell;
            let mut data = declaration.data;

            if declared_is_unresolved {
                crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CascadeResolveUnresolvedCallback);
                shell = unsafe { (callbacks.resolve_unresolved)(context, declaration.property_id, shell) };
                crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CascadeDataOfCallback);
                data = unsafe { (callbacks.data_of)(context, shell) };
            }

            if matches!(
                unsafe { &*(data as *const StyleValueData) },
                StyleValueData::GuaranteedInvalid
            ) {
                // https://drafts.csswg.org/css-values-5/#invalid-at-computed-value-time
                // When substitution results in a property's value containing the guaranteed-invalid value, this makes the
                // declaration invalid at computed-value time. When this happens, the computed value is one of the
                // following depending on the property's type:

                // -> The property is a non-registered custom property
                // -> The property is a registered custom property with universal syntax
                // FIXME: Process custom properties here?
                // The computed value is the guaranteed-invalid value.

                // -> Otherwise
                // Either the property's inherited value or its initial value depending on whether the property is
                // inherited or not, respectively, as if the property's value had been specified as the unset keyword.
                shell = unset_shell;
                data = unset_data;
            }

            let value_is_pending_substitution = matches!(
                unsafe { &*(data as *const StyleValueData) },
                StyleValueData::PendingSubstitution { .. }
            );

            expand_shorthands_with(
                &|shell| {
                    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CascadeDataOfCallback);
                    unsafe { (callbacks.data_of)(context, shell) }
                },
                &|shell| {
                    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CascadePendingSubstitutionCallback);
                    unsafe { (callbacks.create_pending_substitution)(context, shell) }
                },
                declaration.property_id,
                shell,
                data,
                &mut |longhand_id, longhand_shell, longhand_data| {
                    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CascadePropertyDisallowedCallback);
                    if unsafe { (callbacks.is_property_disallowed)(context, longhand_id) } {
                        return;
                    }

                    // If we're a PSV that's already been seen, that should mean that our shorthand already got
                    // resolved and gave us a value, so we don't want to overwrite it with a PSV.
                    let seen_index = longhand_id as usize;
                    debug_assert!(seen_index <= LAST_LONGHAND_PROPERTY_ID as usize);
                    if seen[seen_index / 64] & (1 << (seen_index % 64)) != 0 && value_is_pending_substitution {
                        return;
                    }
                    seen[seen_index / 64] |= 1 << (seen_index % 64);

                    let longhand_value = unsafe { &*(longhand_data as *const StyleValueData) };
                    let longhand_keyword = match longhand_value {
                        StyleValueData::Keyword { keyword } => Some(*keyword),
                        _ => None,
                    };
                    if longhand_keyword == Some(crate::style_compute::keyword::REVERT) {
                        store.revert_property(longhand_id, important, origin);
                    } else if longhand_keyword == Some(crate::style_compute::keyword::REVERT_LAYER) {
                        store.revert_layer_property(
                            longhand_id,
                            important,
                            origin,
                            has_layer_name,
                            layer_name_raw,
                            source_shadow_root_identity,
                        );
                    } else {
                        // Track the exact shadow-root scope that supplied this winning declaration. A constructable
                        // stylesheet can be adopted into multiple scopes at once, so the declaration object alone is
                        // not specific enough.
                        let retained_value = unsafe { RetainedStyleValue::from_borrowed_shell_pointer(longhand_shell) };
                        let layer_name = LayerName(
                            has_layer_name
                                .then(|| unsafe { RetainedUtf16FlyString::from_borrowed_raw(layer_name_raw) }),
                        );
                        let slot = store.set_property(
                            longhand_id,
                            retained_value,
                            longhand_data,
                            important,
                            origin,
                            layer_name,
                            source_shadow_root_identity,
                        );
                        if slot >= 0 {
                            crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CascadeSourceSlotCallback);
                            unsafe { (callbacks.assign_source_slot)(context, slot as u32) };
                        }
                    }
                },
            );
        }
    });
}
