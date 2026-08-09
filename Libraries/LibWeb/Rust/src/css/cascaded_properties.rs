/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The cascaded property store: the per-element result of applying the CSS
//! cascade, one winning declaration list per longhand.
//!
//! This is the Rust backing for the C++ CascadedProperties shell. Entries own
//! strong references to Rust-owned style value data and layer name strings.
//! The GC-managed declaration sources stay on the C++ side, pinned in a slot
//! table of weak references; each entry carries its slot index and the C++
//! shell resolves a slot back to the source objects on demand.

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::c_void;
use std::hash::BuildHasherDefault;
use std::hash::Hasher;

use crate::abort_on_panic;
use crate::css::property_metadata::LAST_LONGHAND_PROPERTY_ID;
use crate::css::style_compute::expand_shorthands_with;
use crate::css::style_value::RetainedStyleValueData;
use crate::css::style_value::RetainedUtf16FlyString;
use crate::css::style_value::StyleValueData;

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
    value: RetainedStyleValueData,
    has_style_sheet_context: bool,
    important: bool,
    cascade_index: u64,
    origin: CascadeOrigin,
    layer_name: LayerName,
    /// Pointer identity of the source shadow root at the time the declaration
    /// was applied, used only to match entries from the same tree context.
    source_shadow_root_identity: usize,
    /// Index into the C++ shell's table of GC-weak declaration sources.
    source_slot: u32,
    /// The entry this property had before this one, or `NO_ENTRY`. A property keeps its entries as a
    /// chain through the one arena rather than as a vector of its own, so an element that declares a
    /// hundred properties allocates once rather than a hundred times.
    previous_for_property: u32,
}

const NO_ENTRY: u32 = u32::MAX;

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
    /// Every entry the cascade stored, in the order it stored them.
    arena: Vec<Entry>,
    /// The last entry each property has, which is the head of its chain.
    last_entry_index: HashMap<u16, u32, BuildHasherDefault<PropertyIdHasher>>,
    next_cascade_index: u64,
    next_source_slot: u32,
    free_source_slots: Vec<u32>,
    /// One bit per longhand property identifier, so the hot "is there any
    /// cascaded value at all" checks skip the hash map.
    contained: [u64; CONTAINED_BITMAP_WORDS],
    retained_seeded: [u64; CONTAINED_BITMAP_WORDS],
}

impl CascadedPropertyStore {
    fn new() -> Self {
        Self {
            arena: Vec::new(),
            last_entry_index: HashMap::default(),
            next_cascade_index: 0,
            next_source_slot: 0,
            free_source_slots: Vec::new(),
            contained: [0; CONTAINED_BITMAP_WORDS],
            retained_seeded: [0; CONTAINED_BITMAP_WORDS],
        }
    }

    /// Empties the store while keeping what it allocated to hold its entries.
    fn reset(&mut self) {
        self.arena.clear();
        self.last_entry_index.clear();
        self.free_source_slots.clear();
        self.next_cascade_index = 0;
        self.next_source_slot = 0;
        self.contained = [0; CONTAINED_BITMAP_WORDS];
        self.retained_seeded = [0; CONTAINED_BITMAP_WORDS];
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

    fn is_retained_seeded(&self, property_id: u16) -> bool {
        let index = property_id as usize;
        self.retained_seeded[index / 64] & (1 << (index % 64)) != 0
    }

    pub(crate) fn seed_retained_property(
        &mut self,
        property_id: u16,
        value: RetainedStyleValueData,
        important: bool,
        has_style_sheet_context: bool,
    ) -> u32 {
        let slot = self.set_property(
            property_id,
            value,
            has_style_sheet_context,
            important,
            CascadeOrigin::Author,
            LayerName(None),
            0,
        );
        let index = property_id as usize;
        self.retained_seeded[index / 64] |= 1 << (index % 64);
        u32::try_from(slot).expect("a fresh retained property allocates a source slot")
    }

    fn last_entry(&self, property_id: u16) -> Option<&Entry> {
        if !self.contains(property_id) {
            return None;
        }
        self.last_entry_index
            .get(&property_id)
            .and_then(|index| self.arena.get(*index as usize))
    }

    #[allow(clippy::too_many_arguments)]
    fn set_property(
        &mut self,
        property_id: u16,
        value: RetainedStyleValueData,
        has_style_sheet_context: bool,
        important: bool,
        origin: CascadeOrigin,
        layer_name: LayerName,
        source_shadow_root_identity: usize,
    ) -> i64 {
        self.set_contained(property_id, true);

        let cascade_index = self.next_cascade_index;
        self.next_cascade_index += 1;

        // The bucket is found once and then both read and appended to: a cascade applies as many
        // declarations as an element matches, and finding it twice was one of two hash lookups per
        // declaration. The source slot is taken from its own fields rather than through
        // `allocate_source_slot`, which would want the whole store while the bucket is held.
        let Self {
            arena,
            last_entry_index,
            next_source_slot,
            free_source_slots,
            ..
        } = self;
        // The chain runs newest first, so this walks it exactly as scanning a property's entries in
        // reverse did.
        let head = last_entry_index.entry(property_id).or_insert(NO_ENTRY);
        let mut current = *head;
        while current != NO_ENTRY {
            let entry = &mut arena[current as usize];
            if entry.origin == origin
                && entry.layer_name.equals(&layer_name)
                && entry.source_shadow_root_identity == source_shadow_root_identity
            {
                if entry.important && !important {
                    return -1;
                }
                entry.value = value;
                entry.has_style_sheet_context = has_style_sheet_context;
                entry.important = important;
                entry.cascade_index = cascade_index;
                return entry.source_slot as i64;
            }
            current = entry.previous_for_property;
        }

        let source_slot = free_source_slots.pop().unwrap_or_else(|| {
            let slot = *next_source_slot;
            *next_source_slot += 1;
            slot
        });
        let index = u32::try_from(arena.len()).expect("cascaded entry space exhausted");
        arena.push(Entry {
            value,
            has_style_sheet_context,
            important,
            cascade_index,
            origin,
            layer_name,
            source_shadow_root_identity,
            source_slot,
            previous_for_property: *head,
        });
        *head = index;
        source_slot as i64
    }

    /// The winning declaration for a property: its Rust-owned data, importance,
    /// and C++ declaration-source slot.
    pub(crate) fn winning_declaration(&self, property_id: u16) -> Option<(*const c_void, bool, u32, bool)> {
        self.last_entry(property_id).map(|entry| {
            (
                entry.value.pointer().cast(),
                entry.important,
                entry.source_slot,
                entry.has_style_sheet_context,
            )
        })
    }

    pub(crate) fn winning_declarations(
        &self,
    ) -> impl Iterator<Item = (u16, *const StyleValueData, CascadeOrigin)> + '_ {
        self.contained
            .iter()
            .enumerate()
            .flat_map(|(word_index, &word)| {
                let mut remaining = word;
                std::iter::from_fn(move || {
                    if remaining == 0 {
                        return None;
                    }
                    let bit = remaining.trailing_zeros() as usize;
                    remaining &= remaining - 1;
                    Some(u16::try_from(word_index * u64::BITS as usize + bit).unwrap())
                })
            })
            .filter_map(|property| {
                self.last_entry_index
                    .get(&property)
                    .and_then(|&index| self.arena.get(index as usize))
                    .map(|entry| (property, entry.value.pointer(), entry.origin))
            })
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
        let Some(&head) = self.last_entry_index.get(&property_id) else {
            return;
        };
        // The decisions are taken first because `matches` reads an entry while relinking writes one.
        let mut chain = Vec::new();
        let mut current = head;
        while current != NO_ENTRY {
            chain.push((current, matches(&self.arena[current as usize])));
            current = self.arena[current as usize].previous_for_property;
        }
        let mut new_head = NO_ENTRY;
        let mut newest_survivor = NO_ENTRY;
        for &(index, removed) in &chain {
            if removed {
                continue;
            }
            if newest_survivor == NO_ENTRY {
                new_head = index;
            } else {
                self.arena[newest_survivor as usize].previous_for_property = index;
            }
            newest_survivor = index;
        }
        if newest_survivor != NO_ENTRY {
            self.arena[newest_survivor as usize].previous_for_property = NO_ENTRY;
        }
        if new_head == NO_ENTRY {
            self.last_entry_index.remove(&property_id);
            self.set_contained(property_id, false);
        } else {
            self.last_entry_index.insert(property_id, new_head);
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
    abort_on_panic(|| {
        // A store lives exactly as long as the computation of one element's style, and a style pass
        // builds one per element. Recycling the emptied ones keeps the arena's capacity and the
        // map's buckets, so only the first few elements of a pass allocate at all. The store itself
        // is still created and destroyed as C++ asks, so nothing about its lifetime changes.
        let store = STORE_POOL
            .with_borrow_mut(Vec::pop)
            .unwrap_or_else(CascadedPropertyStore::new);
        Box::into_raw(Box::new(store))
    })
}

thread_local! {
    static STORE_POOL: RefCell<Vec<CascadedPropertyStore>> = const { RefCell::new(Vec::new()) };
}

/// How many emptied stores are kept. A pass uses one at a time, so this only has to cover the
/// nesting a computation can reach.
const STORE_POOL_CAPACITY: usize = 4;

/// # Safety
/// `store` must be a pointer returned by `rust_cascaded_properties_create` that has not been
/// destroyed yet; no references into the store may outlive this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_cascaded_properties_destroy(store: *mut CascadedPropertyStore) {
    abort_on_panic(|| {
        let mut store = unsafe { Box::from_raw(store) };
        STORE_POOL.with_borrow_mut(|pool| {
            if pool.len() >= STORE_POOL_CAPACITY {
                return;
            }
            store.reset();
            pool.push(*store);
        });
    });
}

/// Returns a borrowed pointer to the winning declaration's Rust-owned style value data, or null.
///
/// # Safety
/// `store` must be a valid store.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_cascaded_properties_property(
    store: *const CascadedPropertyStore,
    property_id: u16,
) -> *const c_void {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::CascadedStoreQueryEntry);
    abort_on_panic(|| match unsafe { &*store }.last_entry(property_id) {
        Some(entry) => entry.value.pointer().cast(),
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
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::CascadedStoreQueryEntry);
    abort_on_panic(|| match unsafe { &*store }.last_entry(property_id) {
        Some(entry) => entry.source_slot as i64,
        None => -1,
    })
}

/// Returns whether the winning declaration's original C++ facade carried
/// stylesheet resource context.
///
/// # Safety
/// `store` must be a valid store.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_cascaded_properties_has_style_sheet_context(
    store: *const CascadedPropertyStore,
    property_id: u16,
) -> bool {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::CascadedStoreQueryEntry);
    abort_on_panic(|| {
        unsafe { &*store }
            .last_entry(property_id)
            .is_some_and(|entry| entry.has_style_sheet_context)
    })
}

/// A declared property in an `FfiCascadeBlock` crossing into `rust_cascade_matched_blocks`:
/// the property identifier, its importance, and borrowed shared Rust value data.
#[repr(C)]
pub struct FfiCascadeDeclaration {
    pub property_id: u16,
    pub important: bool,
    pub has_style_sheet_context: bool,
    pub data: *const c_void,
}

/// A custom-property declaration in an `FfiCascadeBlock`. Names cross as retained raw
/// `Utf16FlyString` identities, which are also sufficient for string equality.
#[repr(C)]
pub struct FfiCustomPropertyDeclaration {
    pub name_raw: usize,
    pub important: bool,
    pub is_revert_layer: bool,
    pub data: *const c_void,
}

/// Applies one declaration block to the cascade: filters by importance and applicability,
/// resolves arbitrary-substitution values through the parser callback, downgrades
/// invalid-at-computed-value-time declarations to unset, expands shorthands, and routes
/// each longhand to the store as a set, revert, or revert-layer.
#[allow(clippy::too_many_arguments)]
fn apply_declaration_block(
    store: &mut CascadedPropertyStore,
    declarations: &[FfiCascadeDeclaration],
    important: bool,
    origin: CascadeOrigin,
    has_layer_name: bool,
    layer_name_raw: usize,
    source_shadow_root_identity: usize,
    unset_data: *const c_void,
    is_property_disallowed: &dyn Fn(u16) -> bool,
    resolve_unresolved: &dyn Fn(u16, *const c_void) -> FfiResolvedStyleValue,
    parse_substituted: &dyn Fn(u16, *const c_void, &[u8]) -> FfiResolvedStyleValue,
    custom_property_store: *const c_void,
    custom_property_registry: *const c_void,
    mut assign_source_slot: impl FnMut(u32),
) {
    let mut seen = [0u64; CONTAINED_BITMAP_WORDS];

    for declaration in declarations {
        if declaration.important != important {
            continue;
        }

        let declared_value = unsafe { &*(declaration.data as *const StyleValueData) };
        let declared_is_unresolved = matches!(declared_value, StyleValueData::Unresolved { .. });

        if is_property_disallowed(declaration.property_id) && !declared_is_unresolved {
            continue;
        }

        if matches!(declared_value, StyleValueData::PendingSubstitution { .. }) {
            continue;
        }

        let mut data = declaration.data;
        let mut has_style_sheet_context = declaration.has_style_sheet_context;

        if declared_is_unresolved {
            let native_resolution = unsafe {
                crate::css::custom_properties::resolve_vars(custom_property_store, custom_property_registry, data)
            };
            match native_resolution {
                crate::css::custom_properties::NativeVarResolution::Resolved(source) => {
                    let resolved = parse_substituted(declaration.property_id, declaration.data, &source);
                    data = resolved.data;
                    has_style_sheet_context = resolved.has_style_sheet_context;
                }
                crate::css::custom_properties::NativeVarResolution::Invalid => {
                    let resolved = parse_substituted(declaration.property_id, declaration.data, &[]);
                    data = resolved.data;
                    has_style_sheet_context = resolved.has_style_sheet_context;
                }
                crate::css::custom_properties::NativeVarResolution::NotHandled => {
                    let resolved = resolve_unresolved(declaration.property_id, data);
                    data = resolved.data;
                    has_style_sheet_context = resolved.has_style_sheet_context;
                }
            }
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
            data = unset_data;
            has_style_sheet_context = false;
        }

        let value_is_pending_substitution = matches!(
            unsafe { &*(data as *const StyleValueData) },
            StyleValueData::PendingSubstitution { .. }
        );
        expand_shorthands_with(
            declaration.property_id,
            data,
            has_style_sheet_context,
            &mut |longhand_id, longhand_data, longhand_has_style_sheet_context| {
                if store.is_retained_seeded(longhand_id) {
                    return;
                }
                if is_property_disallowed(longhand_id) {
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
                if longhand_keyword == Some(crate::css::style_compute::keyword::REVERT) {
                    store.revert_property(longhand_id, important, origin);
                } else if longhand_keyword == Some(crate::css::style_compute::keyword::REVERT_LAYER) {
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
                    let retained_value = unsafe {
                        RetainedStyleValueData::from_retained_pointer(crate::css::style_value::rust_style_value_retain(
                            longhand_data.cast(),
                        ))
                    };
                    let layer_name = LayerName(
                        has_layer_name.then(|| unsafe { RetainedUtf16FlyString::from_borrowed_raw(layer_name_raw) }),
                    );
                    let slot = store.set_property(
                        longhand_id,
                        retained_value,
                        longhand_has_style_sheet_context,
                        important,
                        origin,
                        layer_name,
                        source_shadow_root_identity,
                    );
                    if slot >= 0 {
                        assign_source_slot(slot as u32);
                    }
                }
            },
        );
    }
}

/// One matched declaration block for the bulk cascade: its origin, position in
/// the author context and layer structure, and its declaration list. Blocks
/// arrive grouped by context and layer in collection order; the core derives
/// the css-cascade-5 application sequence from the indices.
#[repr(C)]
pub struct FfiCascadeBlock {
    pub origin: CascadeOrigin,
    /// The author shadow context this block belongs to; author blocks only.
    pub author_context_index: u32,
    /// The layer within the context; author rule blocks only.
    pub layer_index: u32,
    pub is_inline_style: bool,
    /// Inline style may carry properties the pseudo-element whitelist would
    /// reject, since engines use it to style element-backed pseudo-elements.
    pub bypass_pseudo_element_property_whitelist: bool,
    pub has_layer_name: bool,
    /// Borrowed; live for the call.
    pub layer_name_raw: usize,
    pub source_shadow_root_identity: usize,
    /// Index into the C++ side's per-block source table.
    pub source_id: u32,
    /// StyleEngine rule identity, or zero for an element-attached block.
    pub style_engine_rule_id: u32,
    pub declarations: *const FfiCascadeDeclaration,
    pub declaration_count: usize,
    pub custom_property_declarations: *const FfiCustomPropertyDeclaration,
    pub custom_property_declaration_count: usize,
}

/// One winning store slot and the block source that supplied it, reported in
/// bulk after the cascade.
#[repr(C)]
pub struct FfiSourceSlotAssignment {
    pub slot: u32,
    pub source_id: u32,
}

/// One winning custom-property declaration, reported in first-declaration order.
#[repr(C)]
pub struct FfiCascadedCustomProperty {
    pub name_raw: usize,
    pub important: bool,
    pub data: *const c_void,
}

/// Callbacks for the parser-dependent parts of the bulk cascade. Values cross
/// as shared Rust data; the C++ side pins every value it creates until the
/// cascade returns.
#[repr(C)]
pub struct FfiBulkCascadeCallbacks {
    pub context: *mut c_void,
    /// Resolves borrowed Rust value data and returns pinned Rust-owned data.
    pub resolve_unresolved:
        unsafe extern "C" fn(context: *mut c_void, property_id: u16, data: *const c_void) -> FfiResolvedStyleValue,
    /// Parses a substituted token stream and returns pinned Rust-owned data.
    pub parse_substituted: unsafe extern "C" fn(
        context: *mut c_void,
        style_engine_rule_id: u32,
        property_id: u16,
        unresolved_data: *const c_void,
        source: *const u8,
        source_length: usize,
    ) -> FfiResolvedStyleValue,
    /// Receives every winning slot's source assignment in one batch.
    pub assign_source_slots:
        unsafe extern "C" fn(context: *mut c_void, assignments: *const FfiSourceSlotAssignment, count: usize),
    /// Installs the complete custom-property cascade before unresolved longhands are resolved.
    pub set_custom_properties: unsafe extern "C" fn(
        context: *mut c_void,
        properties: *const FfiCascadedCustomProperty,
        count: usize,
    ) -> *const c_void,
}

#[repr(C)]
pub struct FfiResolvedStyleValue {
    pub data: *const c_void,
    pub has_style_sheet_context: bool,
}

/// Sentinel passed when cascading for an element rather than a pseudo-element.
pub(crate) const NO_PSEUDO_ELEMENT: u8 = u8::MAX;

/// Runs the whole cascade for one element in css-cascade-5 origin order over
/// the matched declaration blocks:
///
/// https://drafts.csswg.org/css-cascade-5/#cascade-origin
/// Declarations are applied lowest priority first, so that later
/// applications overwrite earlier ones: normal user agent, normal user,
/// author presentational hints (an
/// independent origin for cascading, part of the author origin for revert),
/// normal author with inner shadow contexts first and layers in declaration
/// order, important author with outer contexts first and layers reversed,
/// important user, and important user agent declarations. Inline style
/// applies within its author context, after the context's layered rules.
///
/// # Safety
/// `store` must be a valid store, `blocks` must point at `block_count` valid
/// blocks whose declaration lists and layer names stay live for the call,
/// and `callbacks` must be a valid callback table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_cascade_matched_blocks(
    store: *mut CascadedPropertyStore,
    blocks: *const FfiCascadeBlock,
    block_count: usize,
    author_context_count: u32,
    pseudo_element: u8,
    custom_property_registry: *const c_void,
    unset_data: *const c_void,
    callbacks: *const FfiBulkCascadeCallbacks,
) {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::CascadeBulkEntry);
    abort_on_panic(|| {
        let store = unsafe { &mut *store };
        let callbacks = unsafe { &*callbacks };
        let context = callbacks.context;
        let blocks = if block_count == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(blocks, block_count) }
        };

        // StyleEngine supplied rule blocks in specificity and source order within each context and
        // layer, and C++ preserved that order while appending element-attached blocks. Partition by
        // the remaining cascade components without re-sorting inside those groups.
        let mut user_agent_blocks = Vec::new();
        let mut user_blocks = Vec::new();
        let mut presentational_hint_blocks = Vec::new();
        let mut author_layer_blocks: Vec<Vec<usize>> = vec![Vec::new(); author_context_count as usize];
        let mut author_inline_blocks: Vec<Option<usize>> = vec![None; author_context_count as usize];
        for (index, block) in blocks.iter().enumerate() {
            match block.origin {
                CascadeOrigin::UserAgent => user_agent_blocks.push(index),
                CascadeOrigin::User => user_blocks.push(index),
                CascadeOrigin::AuthorPresentationalHint => presentational_hint_blocks.push(index),
                CascadeOrigin::Author => {
                    let context_index = block.author_context_index as usize;
                    if block.is_inline_style {
                        author_inline_blocks[context_index] = Some(index);
                    } else {
                        author_layer_blocks[context_index].push(index);
                    }
                }
                _ => {}
            }
        }

        let mut application_order: Vec<(usize, bool, bool)> = Vec::new();

        // Normal user agent, user, and presentational hint declarations.
        for &index in &user_agent_blocks {
            application_order.push((index, false, false));
        }
        for &index in &user_blocks {
            application_order.push((index, false, false));
        }
        for &index in &presentational_hint_blocks {
            application_order.push((index, false, false));
        }

        // Normal author declarations, with inner contexts first so outer contexts win,
        // layers in declaration order, and inline style after its context's layers.
        for context_index in (0..author_context_count as usize).rev() {
            for &index in &author_layer_blocks[context_index] {
                application_order.push((index, false, true));
            }
            if let Some(index) = author_inline_blocks[context_index] {
                application_order.push((index, false, false));
            }
        }

        // Important author declarations, with outer contexts first so inner contexts
        // win and layers reversed; layer names do not apply in the important pass.
        for context_index in 0..author_context_count as usize {
            let layer_blocks = &author_layer_blocks[context_index];
            let mut boundaries: Vec<(u32, usize, usize)> = Vec::new();
            for (position, &index) in layer_blocks.iter().enumerate() {
                let layer = blocks[index].layer_index;
                match boundaries.last_mut() {
                    Some((last_layer, _, end)) if *last_layer == layer => *end = position + 1,
                    _ => boundaries.push((layer, position, position + 1)),
                }
            }
            for &(_, start, end) in boundaries.iter().rev() {
                for &index in &layer_blocks[start..end] {
                    application_order.push((index, true, false));
                }
            }
            if let Some(index) = author_inline_blocks[context_index] {
                application_order.push((index, true, false));
            }
        }

        // Important user and user agent declarations.
        for &index in &user_blocks {
            application_order.push((index, true, false));
        }
        for &index in &user_agent_blocks {
            application_order.push((index, true, false));
        }

        let has_pseudo_element = pseudo_element != NO_PSEUDO_ELEMENT;
        let cascade_custom_properties = !has_pseudo_element
            || crate::css::property_metadata::pseudo_element_supports_property(
                pseudo_element,
                crate::css::property_metadata::property_id::CUSTOM,
            );
        let mut custom_property_store = std::ptr::null();
        if cascade_custom_properties {
            let mut custom_property_indices = HashMap::new();
            let mut custom_properties: Vec<FfiCascadedCustomProperty> = Vec::new();
            for &(block_index, important, _) in &application_order {
                let block = &blocks[block_index];
                let declarations = if block.custom_property_declaration_count == 0 {
                    &[]
                } else {
                    unsafe {
                        std::slice::from_raw_parts(
                            block.custom_property_declarations,
                            block.custom_property_declaration_count,
                        )
                    }
                };
                for declaration in declarations {
                    if declaration.important != important || declaration.is_revert_layer {
                        continue;
                    }
                    let property = FfiCascadedCustomProperty {
                        name_raw: declaration.name_raw,
                        important,
                        data: declaration.data,
                    };
                    if let Some(index) = custom_property_indices.get(&declaration.name_raw) {
                        custom_properties[*index] = property;
                    } else {
                        custom_property_indices.insert(declaration.name_raw, custom_properties.len());
                        custom_properties.push(property);
                    }
                }
            }
            crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::CascadeCustomPropertyBatchCallback);
            unsafe {
                custom_property_store =
                    (callbacks.set_custom_properties)(context, custom_properties.as_ptr(), custom_properties.len());
            }
        }

        let mut source_slot_assignments: Vec<FfiSourceSlotAssignment> = Vec::new();

        let mut apply = |block_index: usize, important: bool, use_layer_name: bool| {
            let block = &blocks[block_index];
            let declarations = if block.declaration_count == 0 {
                &[]
            } else {
                unsafe { std::slice::from_raw_parts(block.declarations, block.declaration_count) }
            };
            let is_property_disallowed = |property_id: u16| -> bool {
                if block.bypass_pseudo_element_property_whitelist || !has_pseudo_element {
                    return false;
                }
                !crate::css::property_metadata::pseudo_element_supports_property(pseudo_element, property_id)
            };
            apply_declaration_block(
                store,
                declarations,
                important,
                block.origin,
                use_layer_name && block.has_layer_name,
                block.layer_name_raw,
                block.source_shadow_root_identity,
                unset_data,
                &is_property_disallowed,
                &|property_id, data| {
                    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::CascadeResolveUnresolvedCallback);
                    unsafe { (callbacks.resolve_unresolved)(context, property_id, data) }
                },
                &|property_id, unresolved_data, source| {
                    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::CascadeParseSubstitutedCallback);
                    unsafe {
                        (callbacks.parse_substituted)(
                            context,
                            block.style_engine_rule_id,
                            property_id,
                            unresolved_data,
                            source.as_ptr(),
                            source.len(),
                        )
                    }
                },
                custom_property_store,
                custom_property_registry,
                |slot| {
                    source_slot_assignments.push(FfiSourceSlotAssignment {
                        slot,
                        source_id: block.source_id,
                    });
                },
            );
        };

        for &(block_index, important, use_layer_name) in &application_order {
            apply(block_index, important, use_layer_name);
        }

        if !source_slot_assignments.is_empty() {
            crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::CascadeSourceSlotCallback);
            unsafe {
                (callbacks.assign_source_slots)(
                    context,
                    source_slot_assignments.as_ptr(),
                    source_slot_assignments.len(),
                );
            };
        }
    });
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::*;

    #[test]
    fn winning_declaration_retains_rust_value_data() {
        let source_value = Arc::new(StyleValueData::Number { value: 42.0 });
        let weak_value = Arc::downgrade(&source_value);
        let retained_value = unsafe { RetainedStyleValueData::from_retained_pointer(Arc::into_raw(source_value)) };
        let mut store = CascadedPropertyStore::new();

        store.set_property(
            crate::css::property_metadata::property_id::OPACITY,
            retained_value,
            false,
            false,
            CascadeOrigin::Author,
            LayerName(None),
            0,
        );

        let (data, important, source_slot, has_style_sheet_context) = store
            .winning_declaration(crate::css::property_metadata::property_id::OPACITY)
            .expect("the declaration must be retained");
        assert!(!important);
        assert_eq!(source_slot, 0);
        assert!(!has_style_sheet_context);
        assert!(matches!(
            unsafe { &*(data as *const StyleValueData) },
            StyleValueData::Number { value } if *value == 42.0
        ));
        assert!(weak_value.upgrade().is_some());

        drop(store);
        assert!(weak_value.upgrade().is_none());
    }

    #[test]
    fn winning_declarations_are_sorted_by_property() {
        let mut store = CascadedPropertyStore::new();
        for property in [
            crate::css::property_metadata::property_id::WIDTH,
            crate::css::property_metadata::property_id::OPACITY,
        ] {
            let value = Arc::new(StyleValueData::Number { value: 42.0 });
            let retained_value = unsafe { RetainedStyleValueData::from_retained_pointer(Arc::into_raw(value)) };
            store.set_property(
                property,
                retained_value,
                false,
                false,
                CascadeOrigin::Author,
                LayerName(None),
                0,
            );
        }

        let properties: Vec<_> = store.winning_declarations().map(|(property, _, _)| property).collect();
        assert!(properties.windows(2).all(|pair| pair[0] < pair[1]));
    }
}
