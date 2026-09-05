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

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::c_void;
use std::hash::BuildHasherDefault;
use std::hash::Hasher;

use crate::css::custom_properties::CustomPropertyStore;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::query_parser::FfiMediaEnvironment;
use crate::css::parser::value_parser::{
    FfiValueParsingContext, FfiValueParsingContextKind, ParseContext, ParseOutcome, parse_css_value_from_source,
};
use crate::css::property_metadata::{
    FIRST_LONGHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID, LONGHAND_WORD_COUNT, NUMBER_OF_LONGHAND_PROPERTIES,
    property_is_in_logical_group, property_logical_group,
};
use crate::css::style_compute::{expand_shorthands_with, font_family_is_monospace, initial_value_data};
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
    dependencies: Cell<Option<crate::css::style_compute::ExternalValueDependencies>>,
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

impl Entry {
    fn dependencies(&self) -> crate::css::style_compute::ExternalValueDependencies {
        if let Some(dependencies) = self.dependencies.get() {
            return dependencies;
        }
        let dependencies = crate::css::style_compute::external_value_dependencies(self.value.data());
        self.dependencies.set(Some(dependencies));
        dependencies
    }
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
    pub(crate) fn new() -> Self {
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
        let mut previous = NO_ENTRY;
        let mut current = *head;
        while current != NO_ENTRY {
            let entry_matches = {
                let entry = &arena[current as usize];
                entry.origin == origin
                    && entry.layer_name.equals(&layer_name)
                    && entry.source_shadow_root_identity == source_shadow_root_identity
            };
            if entry_matches {
                if arena[current as usize].important && !important {
                    return -1;
                }
                let previous_for_property = arena[current as usize].previous_for_property;
                let source_slot = {
                    let entry = &mut arena[current as usize];
                    entry.value = value;
                    entry.dependencies.set(None);
                    entry.has_style_sheet_context = has_style_sheet_context;
                    entry.important = important;
                    entry.cascade_index = cascade_index;
                    entry.source_slot
                };
                // This declaration was applied after the current head, so make it the newest entry
                // even when it reused the same cascade origin, layer, and shadow context.
                if previous != NO_ENTRY {
                    arena[previous as usize].previous_for_property = previous_for_property;
                    arena[current as usize].previous_for_property = *head;
                    *head = current;
                }
                return source_slot as i64;
            }
            let next = arena[current as usize].previous_for_property;
            previous = current;
            current = next;
        }

        let source_slot = free_source_slots.pop().unwrap_or_else(|| {
            let slot = *next_source_slot;
            *next_source_slot += 1;
            slot
        });
        let index = u32::try_from(arena.len()).expect("cascaded entry space exhausted");
        arena.push(Entry {
            value,
            dependencies: Cell::new(None),
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
    pub(crate) fn winning_declaration(
        &self,
        property_id: u16,
    ) -> Option<(
        *const c_void,
        bool,
        u32,
        bool,
        crate::css::style_compute::ExternalValueDependencies,
    )> {
        self.last_entry(property_id).map(|entry| {
            (
                entry.value.pointer().cast(),
                entry.important,
                entry.source_slot,
                entry.has_style_sheet_context,
                entry.dependencies(),
            )
        })
    }

    fn winning_entries(&self) -> impl Iterator<Item = (u16, &Entry)> + '_ {
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
                    .map(|entry| (property, entry))
            })
    }

    pub(crate) fn winning_declarations(
        &self,
    ) -> impl Iterator<Item = (u16, *const StyleValueData, CascadeOrigin, bool)> + '_ {
        self.winning_entries()
            .map(|(property, entry)| (property, entry.value.pointer(), entry.origin, entry.important))
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
    // A store lives exactly as long as the computation of one element's style, and a style pass
    // builds one per element. Recycling the emptied ones keeps the arena's capacity and the
    // map's buckets, so only the first few elements of a pass allocate at all. The store itself
    // is still created and destroyed as C++ asks, so nothing about its lifetime changes.
    let store = STORE_POOL
        .with_borrow_mut(Vec::pop)
        .unwrap_or_else(CascadedPropertyStore::new);
    Box::into_raw(Box::new(store))
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
    let mut store = unsafe { Box::from_raw(store) };
    STORE_POOL.with_borrow_mut(|pool| {
        if pool.len() >= STORE_POOL_CAPACITY {
            return;
        }
        store.reset();
        pool.push(*store);
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
    match unsafe { &*store }.last_entry(property_id) {
        Some(entry) => entry.value.pointer().cast(),
        None => std::ptr::null(),
    }
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
    match unsafe { &*store }.last_entry(property_id) {
        Some(entry) => entry.source_slot as i64,
        None => -1,
    }
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
    unsafe { &*store }
        .last_entry(property_id)
        .is_some_and(|entry| entry.has_style_sheet_context)
}

pub const CASCADED_ENVIRONMENT_NEEDS_DOCUMENT_BASE_URL: u8 = 1 << 0;
pub const CASCADED_ENVIRONMENT_NEEDS_STYLE_SHEET_CONTEXT: u8 = 1 << 1;
#[repr(C)]
pub struct FfiUnfixedRandomSharing {
    pub source: *const c_void,
    pub name: usize,
    pub element_shared: bool,
}

pub(crate) struct StyleComputationPlanInput<'a> {
    pub initial_computed_group_mask: u32,
    pub all_computed_groups: u32,
    pub previous_longhand_values: Option<&'a [*const c_void]>,
    pub retained_selection: Option<crate::css::style::StyleComputationSelection>,
    pub selected_transition_properties: &'a [u16],
    pub has_retained_transition_candidates: bool,
    pub has_relevant_animations: bool,
    pub has_css_defined_animations: bool,
}

#[repr(C)]
pub struct FfiStyleComputationRequirements {
    pub uses_tree_counting_function: bool,
    pub container_relative_length_unit_mask: u8,
    pub environment_requirements: u8,
    pub has_monospace_font_family: bool,
    pub computed_group_mask: u32,
    pub has_computed_property_selection: bool,
    pub computed_property_words: *const u64,
    pub computed_property_word_count: usize,
    pub unfixed_random_sharings: *const FfiUnfixedRandomSharing,
    pub unfixed_random_sharing_count: usize,
    pub storage: *mut c_void,
}

struct StyleComputationRequirementsStorage {
    computed_property_words: [u64; LONGHAND_WORD_COUNT],
    unfixed_random_sharings: Box<[FfiUnfixedRandomSharing]>,
}

fn style_values_are_equal(first: &StyleValueData, second: &StyleValueData) -> bool {
    std::ptr::eq(first, second) || first == second
}

fn longhand_is_selected(words: &[u64], property_id: u16) -> bool {
    let index = (property_id - FIRST_LONGHAND_PROPERTY_ID) as usize;
    words[index / 64] & (1 << (index % 64)) != 0
}

fn select_longhand(words: &mut [u64], property_id: u16) -> bool {
    let index = (property_id - FIRST_LONGHAND_PROPERTY_ID) as usize;
    let bit = 1 << (index % 64);
    let changed = words[index / 64] & bit == 0;
    words[index / 64] |= bit;
    changed
}

fn expand_logical_property_closure(words: &mut [u64]) {
    let mut selected_groups = 0u32;
    for property_id in FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID {
        if longhand_is_selected(words, property_id)
            && let Some(group) = property_logical_group(property_id)
        {
            selected_groups |= 1 << group;
        }
    }
    for property_id in FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID {
        if property_logical_group(property_id).is_some_and(|group| selected_groups & (1 << group) != 0) {
            select_longhand(words, property_id);
        }
    }
}

fn select_coupled_border_style_and_width_groups(words: &mut [u64]) {
    use crate::css::property_metadata::property_id as prop;

    const BORDER_STYLE_AND_WIDTH: [u16; 16] = [
        prop::BORDER_TOP_STYLE,
        prop::BORDER_RIGHT_STYLE,
        prop::BORDER_BOTTOM_STYLE,
        prop::BORDER_LEFT_STYLE,
        prop::BORDER_BLOCK_START_STYLE,
        prop::BORDER_BLOCK_END_STYLE,
        prop::BORDER_INLINE_START_STYLE,
        prop::BORDER_INLINE_END_STYLE,
        prop::BORDER_TOP_WIDTH,
        prop::BORDER_RIGHT_WIDTH,
        prop::BORDER_BOTTOM_WIDTH,
        prop::BORDER_LEFT_WIDTH,
        prop::BORDER_BLOCK_START_WIDTH,
        prop::BORDER_BLOCK_END_WIDTH,
        prop::BORDER_INLINE_START_WIDTH,
        prop::BORDER_INLINE_END_WIDTH,
    ];
    if BORDER_STYLE_AND_WIDTH
        .iter()
        .any(|&property_id| longhand_is_selected(words, property_id))
    {
        for property_id in BORDER_STYLE_AND_WIDTH {
            select_longhand(words, property_id);
        }
    }
}

fn property_has_independent_computed_closure(property_id: u16) -> bool {
    use crate::css::property_metadata::property_id as prop;

    property_is_in_logical_group(property_id)
        || matches!(
            property_id,
            prop::ASPECT_RATIO
                | prop::BACKDROP_FILTER
                | prop::BACKGROUND_COLOR
                | prop::BOX_SHADOW
                | prop::CLIP_PATH
                | prop::CX
                | prop::CY
                | prop::FILL
                | prop::FILTER
                | prop::ISOLATION
                | prop::MIX_BLEND_MODE
                | prop::OBJECT_FIT
                | prop::OBJECT_POSITION
                | prop::OPACITY
                | prop::PERSPECTIVE
                | prop::PERSPECTIVE_ORIGIN
                | prop::R
                | prop::ROTATE
                | prop::RX
                | prop::RY
                | prop::SCALE
                | prop::STROKE
                | prop::TRANSFORM
                | prop::TRANSFORM_ORIGIN
                | prop::TRANSLATE
                | prop::VISIBILITY
                | prop::WILL_CHANGE
                | prop::X
                | prop::Y
                | prop::Z_INDEX
        )
}

unsafe fn plan_style_computation(
    store: &CascadedPropertyStore,
    input: Option<&StyleComputationPlanInput<'_>>,
) -> (bool, u32, bool, [u64; LONGHAND_WORD_COUNT]) {
    use crate::css::property_metadata::property_id as prop;

    let has_monospace_font_family = store
        .last_entry(prop::FONT_FAMILY)
        .is_some_and(|entry| font_family_is_monospace(entry.value.data()));
    let Some(input) = input else {
        return (has_monospace_font_family, 0, false, [0; LONGHAND_WORD_COUNT]);
    };
    let previous_values = input.previous_longhand_values;
    if let Some(previous_values) = previous_values {
        assert_eq!(previous_values.len(), NUMBER_OF_LONGHAND_PROPERTIES);
    }
    let retained_transition_candidates = input.has_retained_transition_candidates;
    let transition_properties = [
        prop::TRANSITION_PROPERTY,
        prop::TRANSITION_DURATION,
        prop::TRANSITION_TIMING_FUNCTION,
        prop::TRANSITION_DELAY,
        prop::TRANSITION_BEHAVIOR,
    ];
    let has_transition_definition = retained_transition_candidates
        || transition_properties.iter().any(|&property_id| {
            store.last_entry(property_id).is_some_and(|entry| {
                let initial = unsafe { &*initial_value_data(property_id) };
                !style_values_are_equal(entry.value.data(), initial)
            })
        });
    let transition_definition_changed = previous_values.is_some_and(|previous_values| {
        has_transition_definition
            && transition_properties.iter().any(|&property_id| {
                let current = store
                    .last_entry(property_id)
                    .map(|entry| entry.value.data())
                    .unwrap_or_else(|| unsafe { &*initial_value_data(property_id) });
                let index = (property_id - FIRST_LONGHAND_PROPERTY_ID) as usize;
                let previous = previous_values[index].cast::<StyleValueData>();
                let previous = if previous.is_null() {
                    unsafe { &*initial_value_data(property_id) }
                } else {
                    unsafe { &*previous }
                };
                !style_values_are_equal(current, previous)
            })
    });
    let must_compute_all_properties = previous_values.is_none()
        || has_monospace_font_family
        || input.has_relevant_animations
        || input.has_css_defined_animations
        || transition_definition_changed;
    let mut computed_group_mask = input.initial_computed_group_mask;
    if must_compute_all_properties || retained_transition_candidates {
        computed_group_mask = input.all_computed_groups;
    }

    let mut computed_property_words = [0; LONGHAND_WORD_COUNT];
    let mut has_computed_property_selection = false;
    if !must_compute_all_properties
        && (computed_group_mask != input.all_computed_groups || retained_transition_candidates)
        && let Some(retained_selection) = input.retained_selection
    {
        computed_property_words.copy_from_slice(&retained_selection.computed_property_words);
        for &property_id in input.selected_transition_properties {
            select_longhand(&mut computed_property_words, property_id);
        }
        expand_logical_property_closure(&mut computed_property_words);
        select_coupled_border_style_and_width_groups(&mut computed_property_words);
        let only_independent_properties_changed =
            (FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID).all(|property_id| {
                !longhand_is_selected(&computed_property_words, property_id)
                    || property_has_independent_computed_closure(property_id)
            });
        // A full initial mask can mean the retained selection could not represent an inherited
        // change. Independent transition properties do not make that missing input safe to skip.
        // An empty initial mask means neither cascade winners nor inherited groups changed and is
        // normalized above only because the driver cannot process an empty group selection.
        has_computed_property_selection = retained_selection.computed_property_closure_is_exact
            || (input.initial_computed_group_mask != input.all_computed_groups && only_independent_properties_changed);
    }
    (
        has_monospace_font_family,
        computed_group_mask,
        has_computed_property_selection,
        computed_property_words,
    )
}

pub(crate) unsafe fn collect_style_computation_requirements(
    store: *const CascadedPropertyStore,
    plan_input: Option<&StyleComputationPlanInput<'_>>,
) -> FfiStyleComputationRequirements {
    let store = unsafe { &*store };
    let mut uses_tree_counting_function = false;
    let mut container_relative_length_unit_mask = 0;
    let mut environment_requirements = 0;
    let mut sharings = Vec::new();
    for (_, entry) in store.winning_entries() {
        let dependencies = entry.dependencies();
        uses_tree_counting_function |= dependencies.uses_tree_counting_function;
        container_relative_length_unit_mask |= dependencies.container_relative_length_unit_mask;
        if dependencies.needs_document_base_url {
            environment_requirements |= CASCADED_ENVIRONMENT_NEEDS_DOCUMENT_BASE_URL;
        }
        if entry.has_style_sheet_context {
            environment_requirements |= CASCADED_ENVIRONMENT_NEEDS_STYLE_SHEET_CONTEXT;
        }
        if dependencies.has_unfixed_random_sharing {
            crate::css::style_compute::collect_unfixed_random_sharings_in_value(entry.value.data(), &mut sharings);
        }
    }
    let unfixed_random_sharings = sharings
        .into_iter()
        .map(|source| {
            let StyleValueData::RandomValueSharing {
                has_name,
                is_auto,
                name,
                element_shared,
                ..
            } = (unsafe { &*source })
            else {
                unreachable!();
            };
            FfiUnfixedRandomSharing {
                source: source.cast(),
                name: if *has_name { name.raw() } else { 0 },
                element_shared: *element_shared || !*is_auto,
            }
        })
        .collect::<Vec<_>>()
        .into_boxed_slice();
    let (has_monospace_font_family, computed_group_mask, has_computed_property_selection, computed_property_words) =
        unsafe { plan_style_computation(store, plan_input) };
    let storage = Box::new(StyleComputationRequirementsStorage {
        computed_property_words,
        unfixed_random_sharings,
    });
    let unfixed_random_sharings = storage.unfixed_random_sharings.as_ptr();
    let unfixed_random_sharing_count = storage.unfixed_random_sharings.len();
    let computed_property_words = storage.computed_property_words.as_ptr();
    let storage = Box::into_raw(storage);
    FfiStyleComputationRequirements {
        uses_tree_counting_function,
        container_relative_length_unit_mask,
        environment_requirements,
        has_monospace_font_family,
        computed_group_mask,
        has_computed_property_selection,
        computed_property_words,
        computed_property_word_count: LONGHAND_WORD_COUNT,
        unfixed_random_sharings: unfixed_random_sharings.cast(),
        unfixed_random_sharing_count,
        storage: storage.cast(),
    }
}

pub(crate) unsafe fn destroy_style_computation_requirements(storage: *mut c_void) {
    drop(unsafe { Box::from_raw(storage.cast::<StyleComputationRequirementsStorage>()) });
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
    pub name: FfiUtf16View,
    pub important: bool,
    pub is_revert_layer: bool,
    pub data: *const c_void,
}

pub(crate) struct ResolvedStyleValue {
    pub(crate) value: RetainedStyleValueData,
    pub(crate) has_style_sheet_context: bool,
}

struct CallbackFreeParseInput {
    in_quirks_mode: bool,
    is_svg_presentation_attribute: bool,
    contains_attr_tainted_values: bool,
    is_ua_style_sheet: bool,
    document_url: Vec<u8>,
    document_base_url: Vec<u8>,
    property_id: u16,
    source: Vec<u16>,
}

pub(crate) struct CallbackFreeParseOutcome {
    pub(crate) outcome: ParseOutcome,
    pub(crate) source: Vec<u16>,
}

#[allow(clippy::arc_with_non_send_sync)]
pub(crate) fn parse_substituted_without_callbacks(
    base_context: &ParseContext,
    property_id: u16,
    source: Vec<u16>,
    contains_attr_tainted_values: bool,
) -> CallbackFreeParseOutcome {
    let input = CallbackFreeParseInput {
        in_quirks_mode: base_context.in_quirks_mode,
        is_svg_presentation_attribute: base_context.is_svg_presentation_attribute,
        contains_attr_tainted_values,
        is_ua_style_sheet: base_context.is_ua_style_sheet,
        document_url: unsafe { crate::bytes_from_raw(base_context.document_url, base_context.document_url_length) }
            .unwrap_or_default()
            .to_vec(),
        document_base_url: unsafe {
            crate::bytes_from_raw(base_context.document_base_url, base_context.document_base_url_length)
        }
        .unwrap_or_default()
        .to_vec(),
        property_id,
        source,
    };
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::SubstitutionCallbackFreeParse);
    let mut random_function_index = 0;
    let value_context = FfiValueParsingContext {
        kind: FfiValueParsingContextKind::Property,
        value: input.property_id,
        secondary_value: 0,
        name: Default::default(),
    };
    let context = ParseContext {
        in_quirks_mode: input.in_quirks_mode,
        is_svg_presentation_attribute: input.is_svg_presentation_attribute,
        is_substituted_value: true,
        contains_attr_tainted_values: input.contains_attr_tainted_values,
        is_ua_style_sheet: input.is_ua_style_sheet,
        value_contexts: &raw const value_context,
        value_context_count: 1,
        declared_namespaces: std::ptr::null(),
        declared_namespace_count: 0,
        document_url: input.document_url.as_ptr(),
        document_url_length: input.document_url.len(),
        document_base_url: input.document_base_url.as_ptr(),
        document_base_url_length: input.document_base_url.len(),
        intern_utf16_fly_string: None,
        length_resolution_context: std::ptr::null(),
        random_function_index: &raw mut random_function_index,
    };
    let outcome = match parse_css_value_from_source(&context, input.property_id, &input.source) {
        // A callback-free parse can report Invalid when an Option-returning grammar could not
        // retain a string. Report that as unhandled so the caller can retry with the callbacks.
        ParseOutcome::Invalid => ParseOutcome::NotHandled,
        outcome => outcome,
    };
    CallbackFreeParseOutcome {
        outcome,
        source: input.source,
    }
}

#[allow(clippy::arc_with_non_send_sync)]
fn parse_substituted_with_callbacks(
    base_context: &ParseContext,
    property_id: u16,
    source: &[u16],
    contains_attr_tainted_values: bool,
) -> std::sync::Arc<StyleValueData> {
    match parse_substituted_source(base_context, property_id, source, contains_attr_tainted_values) {
        ParseOutcome::Parsed(value) => value,
        ParseOutcome::Invalid | ParseOutcome::NotHandled => std::sync::Arc::new(StyleValueData::GuaranteedInvalid),
    }
}

/// Parses a substituted source as a property's value, with the base context's callbacks: what
/// the grammar makes of it, or that the grammar is not one the Rust parser handles.
pub(crate) fn parse_substituted_source(
    base_context: &ParseContext,
    property_id: u16,
    source: &[u16],
    contains_attr_tainted_values: bool,
) -> ParseOutcome {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::SubstitutionCallbackParseRequest);
    let mut random_function_index = 0;
    let value_context = FfiValueParsingContext {
        kind: FfiValueParsingContextKind::Property,
        value: property_id,
        secondary_value: 0,
        name: Default::default(),
    };
    let mut context = *base_context;
    context.is_substituted_value = true;
    context.contains_attr_tainted_values = contains_attr_tainted_values;
    context.value_contexts = &raw const value_context;
    context.value_context_count = 1;
    context.random_function_index = &raw mut random_function_index;
    parse_css_value_from_source(&context, property_id, source)
}

/// Applies one declaration block to the cascade: filters by importance and applicability,
/// resolves arbitrary-substitution values, downgrades invalid-at-computed-value-time
/// declarations to unset, expands shorthands, and routes each longhand to the store as a set,
/// revert, or revert-layer.
#[allow(clippy::too_many_arguments)]
fn apply_declaration_block(
    store: &mut CascadedPropertyStore,
    declarations: &[FfiCascadeDeclaration],
    important: bool,
    origin: CascadeOrigin,
    layer_name: Option<&RetainedUtf16FlyString>,
    source_shadow_root_identity: usize,
    unset_data: *const c_void,
    is_property_disallowed: &dyn Fn(u16) -> bool,
    resolve_value: &mut impl FnMut(u32, u16, *const c_void, bool) -> ResolvedStyleValue,
    style_engine_rule_id: u32,
    mut assign_source_slot: impl FnMut(u32),
) {
    let has_layer_name = layer_name.is_some();
    let layer_name_raw = layer_name.map_or(0, RetainedUtf16FlyString::raw);
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
        let mut resolved_value = None;

        if declared_is_unresolved {
            let resolved = resolve_value(
                style_engine_rule_id,
                declaration.property_id,
                declaration.data,
                declaration.has_style_sheet_context,
            );
            data = resolved.value.pointer().cast();
            has_style_sheet_context = resolved.has_style_sheet_context;
            resolved_value = Some(resolved.value);
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
                        RetainedStyleValueData::from_retained_pointer(crate::css::style_value::retain_style_value(
                            longhand_data.cast(),
                        ))
                    };
                    let layer_name = LayerName(layer_name.cloned());
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
        drop(resolved_value);
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

/// Main-thread services used while resolving substituted values inside the Rust cascade.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiCascadeResolutionContext {
    pub parse_context: *const c_void,
    pub media_environment: *const c_void,
    pub load_media_environment: Option<unsafe extern "C" fn(*mut c_void) -> *const c_void>,
    pub custom_property_store: *const c_void,
    pub inheritance_custom_property_store: *const c_void,
    pub custom_property_registry: *const c_void,
    pub root_custom_property_name: FfiUtf16View,
    pub attributes: *const crate::css::custom_properties::FfiSubstitutionAttribute,
    pub attribute_count: usize,
    pub attribute_names_are_ascii_case_insensitive: bool,
    pub custom_functions: *const crate::css::custom_properties::FfiSubstitutionFunctionDefinition,
    pub custom_function_count: usize,
    pub custom_function_scope_identity: usize,
    pub callback_context: *mut c_void,
    pub install_custom_properties: Option<
        unsafe extern "C" fn(*mut c_void, *const FfiCascadedCustomProperty, usize, *mut *const c_void) -> *const c_void,
    >,
    pub resolve_custom_function: Option<unsafe extern "C" fn(usize, FfiUtf16View) -> usize>,
    pub evaluate_style_query: Option<unsafe extern "C" fn(*mut c_void, FfiUtf16View) -> u8>,
    pub note_substitution: Option<unsafe extern "C" fn(*mut c_void, *const c_void)>,
}

/// One unresolved value submitted to the bulk substitution resolver.
#[repr(C)]
pub struct FfiUnresolvedStyleValue {
    pub property_id: u16,
    pub root_custom_property_name: FfiUtf16View,
    pub data: *const c_void,
    pub resolve_substitutions: bool,
}

/// One retained value produced by the bulk substitution resolver.
#[repr(C)]
pub struct FfiResolvedStyleValue {
    pub data: *const c_void,
}

#[repr(C)]
pub struct FfiCustomPropertyResolutionStats {
    pub final_value_hits: u64,
    pub final_value_misses: u64,
    pub cycle_participants: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiCustomPropertyDriveInput {
    pub store: *const c_void,
    pub resolved_parent_store: *const c_void,
    pub reuse_resolved_parent_if_empty: bool,
    pub resolution_context: *const FfiCascadeResolutionContext,
    pub finalizer_context: *mut c_void,
    pub finalize_component:
        Option<unsafe extern "C" fn(*mut c_void, *const usize, *const u32, usize, *mut FfiResolvedStyleValue)>,
}

#[repr(C)]
pub struct FfiResolvedCustomProperty {
    pub name_raw: usize,
    pub important: bool,
    /// Transfers one strong style-value reference to C++.
    pub data: *const c_void,
}

#[repr(C)]
pub struct FfiResolvedCustomProperties {
    pub properties: *const FfiResolvedCustomProperty,
    pub count: usize,
    pub did_resolve: bool,
    /// Transfers one strong custom-property store reference to C++.
    pub rust_store: *const c_void,
    pub stats: FfiCustomPropertyResolutionStats,
    pub storage: *mut c_void,
}

fn custom_property_needs_resolution(value: &StyleValueData) -> bool {
    matches!(
        value,
        StyleValueData::Unresolved {
            presence_attr: true,
            ..
        } | StyleValueData::Unresolved {
            presence_dashed_function: true,
            ..
        } | StyleValueData::Unresolved { presence_env: true, .. }
            | StyleValueData::Unresolved { presence_if: true, .. }
            | StyleValueData::Unresolved {
                presence_inherit: true,
                ..
            }
            | StyleValueData::Unresolved { presence_var: true, .. }
    )
}

struct CustomPropertyFinalizerContext<'a> {
    input: &'a FfiCustomPropertyDriveInput,
    names: &'a [usize],
}

unsafe extern "C" fn finalize_custom_property_component(
    context: *mut c_void,
    members: *const u32,
    member_count: usize,
    outputs: *mut FfiResolvedStyleValue,
) {
    let context = unsafe { &*context.cast::<CustomPropertyFinalizerContext>() };
    unsafe {
        (context
            .input
            .finalize_component
            .expect("custom-property drive finalizer"))(
            context.input.finalizer_context,
            context.names.as_ptr(),
            members,
            member_count,
            outputs,
        );
    }
}

/// Resolves every value declared by one custom-property store while the
/// element style computation remains in Rust.
///
/// # Safety
/// Every pointer in `input` must remain valid for the call. The finalizer must
/// replace each component output with one transferred style-value reference.
pub(crate) unsafe fn drive_custom_property_resolution(
    input: &FfiCustomPropertyDriveInput,
) -> FfiResolvedCustomProperties {
    let store = unsafe { &*input.store.cast::<CustomPropertyStore>() };
    let names = &store.declared_names;
    let mut inputs = Vec::with_capacity(names.len());
    for name_raw in names {
        let entry = store
            .own_values
            .get(name_raw)
            .expect("declared custom property must be an own value");
        inputs.push(FfiUnresolvedStyleValue {
            property_id: crate::css::property_metadata::property_id::CUSTOM,
            root_custom_property_name: FfiUtf16View {
                ascii: std::ptr::null(),
                utf16: entry.name.as_ptr(),
                length: entry.name.len(),
            },
            data: entry.value.pointer().cast(),
            resolve_substitutions: custom_property_needs_resolution(entry.value.data()),
        });
    }
    let mut outputs: Vec<FfiResolvedStyleValue> = names
        .iter()
        .map(|_| FfiResolvedStyleValue { data: std::ptr::null() })
        .collect();
    let mut finalizer_context = CustomPropertyFinalizerContext { input, names };
    let stats = unsafe {
        rust_resolve_unresolved_style_values(
            input.resolution_context,
            inputs.as_ptr(),
            inputs.len(),
            outputs.as_mut_ptr(),
            std::ptr::from_mut(&mut finalizer_context).cast(),
            Some(finalize_custom_property_component),
        )
    };
    let resolved_parent = if input.resolved_parent_store.is_null() {
        None
    } else {
        Some(unsafe { &*input.resolved_parent_store.cast::<CustomPropertyStore>() })
    };
    let mut resolved_values = Vec::with_capacity(names.len());
    let properties: Vec<FfiResolvedCustomProperty> = names
        .iter()
        .zip(outputs)
        .filter_map(|(name_raw, output)| {
            let entry = store
                .own_values
                .get(name_raw)
                .expect("declared custom property must be an own value");
            let value = unsafe { RetainedStyleValueData::from_retained_pointer(output.data.cast()) };
            if resolved_parent.is_some_and(|parent| parent.value_matches(*name_raw, value.data())) {
                return None;
            }
            let property = FfiResolvedCustomProperty {
                name_raw: *name_raw,
                important: entry.important,
                data: unsafe { crate::css::style_value::retain_style_value(value.pointer()) }.cast(),
            };
            resolved_values.push((*name_raw, value));
            Some(property)
        })
        .collect();
    let rust_store = if properties.is_empty() && input.reuse_resolved_parent_if_empty {
        std::ptr::null()
    } else {
        unsafe { store.resolved_child(input.resolved_parent_store, resolved_values) }
    };
    let properties = properties.into_boxed_slice();
    let count = properties.len();
    let storage = Box::into_raw(properties);
    FfiResolvedCustomProperties {
        properties: storage.cast::<FfiResolvedCustomProperty>(),
        count,
        did_resolve: true,
        rust_store,
        stats,
        storage: storage.cast(),
    }
}

/// # Safety
/// `storage` and `count` must identify a live custom-property result batch.
pub(crate) unsafe fn destroy_resolved_custom_properties(storage: *mut c_void, count: usize) {
    drop(unsafe {
        Box::from_raw(std::ptr::slice_from_raw_parts_mut(
            storage.cast::<FfiResolvedCustomProperty>(),
            count,
        ))
    });
}

/// Source assignments produced by a completed cascade.
#[repr(C)]
pub struct FfiCascadeResult {
    pub source_slot_assignments: *const FfiSourceSlotAssignment,
    pub source_slot_assignment_count: usize,
    pub storage: *mut c_void,
}

/// Sentinel passed when cascading for an element rather than a pseudo-element.
pub(crate) const NO_PSEUDO_ELEMENT: u8 = u8::MAX;

fn cascade_application_order(blocks: &[FfiCascadeBlock], author_context_count: u32) -> Vec<(usize, bool, bool)> {
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

    application_order
}

fn cascade_custom_properties(
    blocks: &[FfiCascadeBlock],
    author_context_count: u32,
    pseudo_element: u8,
    parent_store: *const c_void,
) -> (bool, Vec<FfiCascadedCustomProperty>, *const c_void) {
    use crate::css::style::cascade::{CascadeAttachment, CascadeOperator, CascadeStratum};
    use crate::css::style::program::CascadeLayerID;

    let applies = pseudo_element == NO_PSEUDO_ELEMENT
        || crate::css::property_metadata::pseudo_element_supports_property(
            pseudo_element,
            crate::css::property_metadata::property_id::CUSTOM,
        );
    if !applies || !blocks.iter().any(|block| block.custom_property_declaration_count != 0) {
        return (applies, Vec::new(), std::ptr::null());
    }

    let mut property_indices = HashMap::new();
    let mut candidates_by_name: Vec<Vec<(&FfiCustomPropertyDeclaration, CascadeStratum)>> = Vec::new();
    for (block_index, important, _) in cascade_application_order(blocks, author_context_count) {
        let block = &blocks[block_index];
        if block.custom_property_declaration_count == 0 {
            continue;
        }
        // NB: Bulk blocks use context-local dense layer indices rather than interned layer IDs.
        //     Keep the implicit outer layer, including inline style, above every named layer.
        let (layer, layer_rank) = if block.has_layer_name {
            (
                CascadeLayerID(block.layer_index.checked_add(1).expect("cascade layer count exhausted")),
                u64::from(block.layer_index),
            )
        } else {
            (CascadeLayerID::UNLAYERED, u64::MAX)
        };
        let stratum = CascadeStratum::new(
            block.origin,
            important,
            block.author_context_index,
            layer,
            (layer_rank, 0),
            if block.is_inline_style {
                CascadeAttachment::InlineStyle
            } else {
                CascadeAttachment::StyleSheet
            },
        );
        let declarations = unsafe {
            std::slice::from_raw_parts(
                block.custom_property_declarations,
                block.custom_property_declaration_count,
            )
        };
        for declaration in declarations {
            if declaration.important != important {
                continue;
            }
            let index = *property_indices.entry(declaration.name_raw).or_insert_with(|| {
                candidates_by_name.push(Vec::new());
                candidates_by_name.len() - 1
            });
            candidates_by_name[index].push((declaration, stratum));
        }
    }
    let mut properties = Vec::with_capacity(candidates_by_name.len());
    for candidates in candidates_by_name {
        let mut ceilings = Vec::new();
        for (declaration, stratum) in candidates.into_iter().rev() {
            if !ceilings.iter().all(|&ceiling| stratum.is_below(ceiling)) {
                continue;
            }
            let operator = if declaration.is_revert_layer {
                CascadeOperator::RevertLayer
            } else if matches!(unsafe { &*declaration.data.cast::<StyleValueData>() }, StyleValueData::Keyword { keyword } if *keyword == crate::css::style_compute::keyword::REVERT)
            {
                CascadeOperator::Revert
            } else {
                CascadeOperator::Declared
            };
            if let Some(ceiling) = stratum.ceiling(operator) {
                ceilings.push(ceiling);
                continue;
            }
            properties.push((
                FfiCascadedCustomProperty {
                    name_raw: declaration.name_raw,
                    important: declaration.important,
                    data: declaration.data,
                },
                declaration.name,
            ));
            break;
        }
    }

    let parent = if parent_store.is_null() {
        None
    } else {
        Some(unsafe { &*parent_store.cast::<CustomPropertyStore>() })
    };
    let mut store_values = Vec::with_capacity(properties.len());
    let properties: Vec<FfiCascadedCustomProperty> = properties
        .into_iter()
        .map(|(property, name)| {
            if !parent.is_some_and(|parent| parent.value_is_identical(property.name_raw, property.data)) {
                store_values.push((
                    property.name_raw,
                    unsafe { name.to_utf16() }.expect("invalid custom property name"),
                    property.important,
                    property.data,
                ));
            }
            property
        })
        .collect();
    if properties.is_empty() {
        return (applies, properties, std::ptr::null());
    }
    let rust_store = if store_values.is_empty() {
        std::ptr::null()
    } else {
        unsafe { CustomPropertyStore::cascaded_child(parent_store, store_values) }
    };
    (applies, properties, rust_store)
}

#[allow(clippy::arc_with_non_send_sync)]
pub(crate) fn resolve_cascade_value(
    resolution_context: &FfiCascadeResolutionContext,
    resolution_environment: Option<&mut crate::css::custom_properties::VarResolutionEnvironment>,
    property_id: u16,
    unresolved_data: *const c_void,
    has_style_sheet_context: bool,
    final_custom_properties: Option<&HashMap<Vec<u16>, *const c_void>>,
) -> ResolvedStyleValue {
    let native_resolution = match resolution_environment {
        Some(resolution_environment) => unsafe {
            let parse_context = resolution_context.parse_context.cast::<ParseContext>().as_ref();
            let media_environment = resolution_context
                .media_environment
                .cast::<FfiMediaEnvironment>()
                .as_ref();
            crate::css::custom_properties::resolve_vars(
                resolution_context.custom_property_store,
                resolution_context.inheritance_custom_property_store,
                resolution_context.custom_property_registry,
                parse_context,
                media_environment,
                resolution_context.load_media_environment,
                resolution_context.root_custom_property_name,
                unresolved_data,
                resolution_environment,
                resolution_context.attribute_names_are_ascii_case_insensitive,
                resolution_context.resolve_custom_function,
                resolution_context.callback_context,
                resolution_context.evaluate_style_query,
                final_custom_properties,
            )
        },
        None => crate::css::custom_properties::NativeVarResolution::NotHandled,
    };
    if let Some(note_substitution) = resolution_context.note_substitution {
        unsafe { note_substitution(resolution_context.callback_context, unresolved_data) };
    }

    let parsed = match native_resolution {
        crate::css::custom_properties::NativeVarResolution::Resolved {
            source,
            contains_attr_tainted_values,
        } => {
            let Some(base_context) = (unsafe { resolution_context.parse_context.cast::<ParseContext>().as_ref() })
            else {
                return ResolvedStyleValue {
                    value: RetainedStyleValueData::from_owned(StyleValueData::GuaranteedInvalid),
                    has_style_sheet_context,
                };
            };
            let contains_attr_tainted_values = contains_attr_tainted_values
                || matches!(
                    unsafe { &*unresolved_data.cast::<StyleValueData>() },
                    StyleValueData::Unresolved {
                        contains_attr_tainted_values: true,
                        ..
                    }
                );
            let CallbackFreeParseOutcome { outcome, source } =
                parse_substituted_without_callbacks(base_context, property_id, source, contains_attr_tainted_values);
            match outcome {
                ParseOutcome::Parsed(value) => value,
                ParseOutcome::Invalid => std::sync::Arc::new(StyleValueData::GuaranteedInvalid),
                ParseOutcome::NotHandled => {
                    parse_substituted_with_callbacks(base_context, property_id, &source, contains_attr_tainted_values)
                }
            }
        }
        crate::css::custom_properties::NativeVarResolution::Invalid => {
            std::sync::Arc::new(StyleValueData::GuaranteedInvalid)
        }
        crate::css::custom_properties::NativeVarResolution::NotHandled => {
            std::sync::Arc::new(StyleValueData::GuaranteedInvalid)
        }
    };
    ResolvedStyleValue {
        value: unsafe { RetainedStyleValueData::from_retained_pointer(std::sync::Arc::into_raw(parsed)) },
        has_style_sheet_context,
    }
}

fn custom_property_components(inputs: &[FfiUnresolvedStyleValue]) -> (Vec<Vec<u32>>, u64, bool) {
    let mut indices = HashMap::new();
    for (index, input) in inputs.iter().enumerate() {
        if let Some(name) = unsafe { input.root_custom_property_name.to_utf16() } {
            indices.insert(name, index as u32);
        }
    }

    let mut edges = vec![Vec::new(); inputs.len()];
    let mut has_own_reference = false;
    for (index, input) in inputs.iter().enumerate() {
        let value = unsafe { &*input.data.cast::<StyleValueData>() };
        let Some((references, mut all_references_visible)) = crate::css::style_value::custom_property_references(value)
        else {
            continue;
        };
        if let StyleValueData::Unresolved {
            presence_attr,
            presence_dashed_function,
            presence_if,
            ..
        } = value
        {
            all_references_visible &= !presence_attr && !presence_dashed_function && !presence_if;
        }
        if all_references_visible {
            for reference in references {
                if let Some(target) = indices.get(&reference) {
                    edges[index].push(*target);
                    has_own_reference |= inputs[*target as usize].resolve_substitutions;
                }
            }
        } else {
            edges[index].extend(0..inputs.len() as u32);
        }
    }

    let unvisited = u32::MAX;
    let mut discovery_index = vec![unvisited; inputs.len()];
    let mut lowlink = vec![0; inputs.len()];
    let mut on_stack = vec![false; inputs.len()];
    let mut component_stack = Vec::new();
    let mut components = Vec::new();
    let mut cycle_participants = 0;
    let mut next_discovery_index = 0;
    let mut walk_stack: Vec<(u32, usize)> = Vec::new();

    for root in 0..inputs.len() as u32 {
        if discovery_index[root as usize] != unvisited {
            continue;
        }
        walk_stack.push((root, 0));
        while let Some(&(node, next_edge)) = walk_stack.last() {
            let node_index = node as usize;
            if next_edge == 0 {
                discovery_index[node_index] = next_discovery_index;
                lowlink[node_index] = next_discovery_index;
                next_discovery_index += 1;
                component_stack.push(node);
                on_stack[node_index] = true;
            }
            if next_edge < edges[node_index].len() {
                let target = edges[node_index][next_edge];
                walk_stack.last_mut().unwrap().1 += 1;
                if discovery_index[target as usize] == unvisited {
                    walk_stack.push((target, 0));
                } else if on_stack[target as usize] {
                    lowlink[node_index] = lowlink[node_index].min(discovery_index[target as usize]);
                }
                continue;
            }
            if lowlink[node_index] == discovery_index[node_index] {
                let mut component = Vec::new();
                loop {
                    let popped = component_stack.pop().expect("active custom-property component");
                    on_stack[popped as usize] = false;
                    component.push(popped);
                    if popped == node {
                        break;
                    }
                }
                if component.len() > 1 || edges[component[0] as usize].contains(&component[0]) {
                    cycle_participants += component.len() as u64;
                }
                components.push(component);
            }
            walk_stack.pop();
            if let Some(&(parent, _)) = walk_stack.last() {
                lowlink[parent as usize] = lowlink[parent as usize].min(lowlink[node_index]);
            }
        }
    }
    (
        components,
        if has_own_reference { cycle_participants } else { 0 },
        has_own_reference,
    )
}

/// Resolves and parses unresolved values outside the longhand cascade. When a
/// finalizer is supplied, custom properties are resolved in dependency order
/// and each component is finalized before later components can read it.
///
/// # Safety
/// Every pointer must remain valid for this call. `outputs` must have room for
/// `input_count` entries, and a finalizer must replace each component output
/// with a live style value pointer before returning.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_resolve_unresolved_style_values(
    resolution_context: *const FfiCascadeResolutionContext,
    inputs: *const FfiUnresolvedStyleValue,
    input_count: usize,
    outputs: *mut FfiResolvedStyleValue,
    finalizer_context: *mut c_void,
    finalize_component: Option<unsafe extern "C" fn(*mut c_void, *const u32, usize, *mut FfiResolvedStyleValue)>,
) -> FfiCustomPropertyResolutionStats {
    let resolution_context = unsafe { *resolution_context };
    let inputs = if input_count == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(inputs, input_count) }
    };
    let outputs = if input_count == 0 {
        &mut []
    } else {
        unsafe { std::slice::from_raw_parts_mut(outputs, input_count) }
    };
    let mut resolution_environment = unsafe {
        crate::css::custom_properties::prepare_var_resolution_environment(
            resolution_context.attributes,
            resolution_context.attribute_count,
            resolution_context.custom_functions,
            resolution_context.custom_function_count,
            resolution_context.custom_function_scope_identity,
        )
    };
    let (components, cycle_participants, use_final_custom_properties) = if finalize_component.is_some() {
        custom_property_components(inputs)
    } else {
        ((0..input_count as u32).map(|index| vec![index]).collect(), 0, false)
    };
    let mut final_custom_properties = HashMap::new();
    for mut component in components {
        component.sort_unstable();
        for &member in &component {
            let input = &inputs[member as usize];
            if !input.resolve_substitutions {
                outputs[member as usize].data =
                    unsafe { crate::css::style_value::retain_style_value(input.data.cast::<StyleValueData>()).cast() };
                continue;
            }
            let mut member_context = resolution_context;
            member_context.root_custom_property_name = input.root_custom_property_name;
            let resolved = resolve_cascade_value(
                &member_context,
                resolution_environment.as_mut(),
                input.property_id,
                input.data,
                false,
                use_final_custom_properties.then_some(&final_custom_properties),
            );
            outputs[member as usize].data = resolved.value.pointer().cast();
            std::mem::forget(resolved.value);
        }
        if let Some(finalize_component) = finalize_component {
            unsafe {
                finalize_component(
                    finalizer_context,
                    component.as_ptr(),
                    component.len(),
                    outputs.as_mut_ptr(),
                );
            };
            for &member in &component {
                let name = unsafe { inputs[member as usize].root_custom_property_name.to_utf16() }
                    .expect("custom-property resolution input name");
                final_custom_properties.insert(name, outputs[member as usize].data);
            }
        }
    }
    FfiCustomPropertyResolutionStats {
        final_value_hits: resolution_environment
            .as_ref()
            .map_or(0, |environment| environment.final_value_hits()),
        final_value_misses: resolution_environment
            .as_ref()
            .map_or(0, |environment| environment.final_value_misses()),
        cycle_participants,
    }
}

/// Runs the longhand cascade for one element in css-cascade-5 origin order
/// over the matched declaration blocks:
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
/// blocks whose declaration lists stay live for the call and whose nonzero
/// layer names each transfer one leaked fly-string reference,
/// and `resolution_context` must point at live parser and callback state for
/// the duration of this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_cascade_matched_blocks(
    store: *mut CascadedPropertyStore,
    blocks: *const FfiCascadeBlock,
    block_count: usize,
    author_context_count: u32,
    pseudo_element: u8,
    unset_data: *const c_void,
    resolution_context: *const FfiCascadeResolutionContext,
) -> FfiCascadeResult {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::CascadeBulkEntry);
    let store = unsafe { &mut *store };
    let blocks = if block_count == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(blocks, block_count) }
    };
    let resolution_context = unsafe { &*resolution_context };

    let (custom_properties_apply, custom_properties, mut unadopted_custom_property_store) = cascade_custom_properties(
        blocks,
        author_context_count,
        pseudo_element,
        resolution_context.custom_property_store,
    );
    let mut resolution_context = *resolution_context;
    if custom_properties_apply {
        resolution_context.custom_property_store = unsafe {
            (resolution_context
                .install_custom_properties
                .expect("missing custom property installer"))(
                resolution_context.callback_context,
                custom_properties.as_ptr(),
                custom_properties.len(),
                &raw mut unadopted_custom_property_store,
            )
        };
    }
    if !unadopted_custom_property_store.is_null() {
        drop(unsafe { std::sync::Arc::from_raw(unadopted_custom_property_store.cast::<CustomPropertyStore>()) });
    }

    let mut resolution_environment = None;

    let application_order = cascade_application_order(blocks, author_context_count);
    let has_pseudo_element = pseudo_element != NO_PSEUDO_ELEMENT;
    let block_layer_names: Vec<Option<RetainedUtf16FlyString>> = blocks
        .iter()
        .map(|block| {
            block
                .has_layer_name
                .then(|| unsafe { RetainedUtf16FlyString::from_leaked_raw(block.layer_name_raw) })
        })
        .collect();

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
            if use_layer_name {
                block_layer_names[block_index].as_ref()
            } else {
                None
            },
            block.source_shadow_root_identity,
            unset_data,
            &is_property_disallowed,
            &mut |_style_engine_rule_id, property_id, unresolved_data, has_style_sheet_context| {
                let resolution_environment = resolution_environment.get_or_insert_with(|| unsafe {
                    crate::css::custom_properties::prepare_var_resolution_environment(
                        resolution_context.attributes,
                        resolution_context.attribute_count,
                        resolution_context.custom_functions,
                        resolution_context.custom_function_count,
                        resolution_context.custom_function_scope_identity,
                    )
                });
                resolve_cascade_value(
                    &resolution_context,
                    resolution_environment.as_mut(),
                    property_id,
                    unresolved_data,
                    has_style_sheet_context,
                    None,
                )
            },
            block.style_engine_rule_id,
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

    if source_slot_assignments.is_empty() {
        return FfiCascadeResult {
            source_slot_assignments: std::ptr::null(),
            source_slot_assignment_count: 0,
            storage: std::ptr::null_mut(),
        };
    }
    let source_slot_assignments = source_slot_assignments.into_boxed_slice();
    let source_slot_assignment_count = source_slot_assignments.len();
    let storage = Box::into_raw(source_slot_assignments);
    FfiCascadeResult {
        source_slot_assignments: storage.cast::<FfiSourceSlotAssignment>(),
        source_slot_assignment_count,
        storage: storage.cast(),
    }
}

/// # Safety
/// `storage` and `count` must come from `rust_cascade_matched_blocks` and must not already have
/// been released.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_cascade_result_destroy(storage: *mut c_void, count: usize) {
    if !storage.is_null() {
        drop(unsafe {
            Box::from_raw(std::ptr::slice_from_raw_parts_mut(
                storage.cast::<FfiSourceSlotAssignment>(),
                count,
            ))
        });
    }
}

#[cfg(test)]
#[allow(clippy::arc_with_non_send_sync)]
mod tests {
    use std::sync::Arc;

    use crate::css::property_metadata::property_id as prop;

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

        let (data, important, source_slot, has_style_sheet_context, _) = store
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

        let properties: Vec<_> = store
            .winning_declarations()
            .map(|(property, _, _, _)| property)
            .collect();
        assert!(properties.windows(2).all(|pair| pair[0] < pair[1]));
    }

    #[test]
    fn computation_requirements_aggregate_winning_declarations() {
        let value = Arc::new(StyleValueData::Number { value: 42.0 });
        let retained_value = unsafe { RetainedStyleValueData::from_retained_pointer(Arc::into_raw(value)) };
        let mut store = CascadedPropertyStore::new();
        let property_id = crate::css::property_metadata::property_id::WIDTH;
        store.set_property(
            property_id,
            retained_value,
            true,
            false,
            CascadeOrigin::Author,
            LayerName(None),
            0,
        );
        store.last_entry(property_id).unwrap().dependencies.set(Some(
            crate::css::style_compute::ExternalValueDependencies {
                uses_tree_counting_function: true,
                container_relative_length_unit_mask: 0b1001,
                needs_document_base_url: true,
                ..Default::default()
            },
        ));
        let other_value = Arc::new(StyleValueData::Number { value: 7.0 });
        let other_retained_value = unsafe { RetainedStyleValueData::from_retained_pointer(Arc::into_raw(other_value)) };
        let other_property_id = crate::css::property_metadata::property_id::OPACITY;
        store.set_property(
            other_property_id,
            other_retained_value,
            false,
            false,
            CascadeOrigin::Author,
            LayerName(None),
            0,
        );
        store.last_entry(other_property_id).unwrap().dependencies.set(Some(
            crate::css::style_compute::ExternalValueDependencies {
                container_relative_length_unit_mask: 0b0110,
                ..Default::default()
            },
        ));

        let requirements = unsafe { collect_style_computation_requirements(&store, None) };
        assert!(requirements.uses_tree_counting_function);
        assert_eq!(requirements.container_relative_length_unit_mask, 0b1111);
        assert_eq!(
            requirements.environment_requirements,
            CASCADED_ENVIRONMENT_NEEDS_DOCUMENT_BASE_URL | CASCADED_ENVIRONMENT_NEEDS_STYLE_SHEET_CONTEXT
        );
        assert_eq!(requirements.unfixed_random_sharing_count, 0);
        unsafe {
            destroy_style_computation_requirements(requirements.storage);
        }
    }

    #[test]
    fn logical_property_closure_reaches_the_entire_group() {
        let mut words = [0; LONGHAND_WORD_COUNT];
        select_longhand(&mut words, prop::MARGIN_TOP);

        expand_logical_property_closure(&mut words);

        for property_id in [
            prop::MARGIN_TOP,
            prop::MARGIN_RIGHT,
            prop::MARGIN_BOTTOM,
            prop::MARGIN_LEFT,
            prop::MARGIN_BLOCK_START,
            prop::MARGIN_INLINE_END,
            prop::MARGIN_BLOCK_END,
            prop::MARGIN_INLINE_START,
        ] {
            assert!(longhand_is_selected(&words, property_id));
        }
        assert!(!longhand_is_selected(&words, prop::PADDING_TOP));
    }

    #[test]
    fn border_style_and_width_groups_are_coupled() {
        let mut words = [0; LONGHAND_WORD_COUNT];
        select_longhand(&mut words, prop::BORDER_TOP_STYLE);

        expand_logical_property_closure(&mut words);
        select_coupled_border_style_and_width_groups(&mut words);

        assert!(longhand_is_selected(&words, prop::BORDER_INLINE_END_STYLE));
        assert!(longhand_is_selected(&words, prop::BORDER_INLINE_END_WIDTH));
        assert!(longhand_is_selected(&words, prop::BORDER_BOTTOM_WIDTH));
        assert!(!longhand_is_selected(&words, prop::BORDER_TOP_COLOR));
    }

    #[test]
    fn computed_property_closure_identifies_independent_properties() {
        assert!(property_has_independent_computed_closure(prop::OPACITY));
        assert!(property_has_independent_computed_closure(prop::MARGIN_BLOCK_START));
        assert!(!property_has_independent_computed_closure(prop::COLOR));
        assert!(!property_has_independent_computed_closure(prop::FONT_SIZE));
    }
}
