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
use std::hash::BuildHasherDefault;
use std::hash::Hasher;
use std::sync::Arc;

use crate::css::custom_properties::CustomPropertyStore;
use crate::css::parser::component_value::ComponentValue;
use crate::css::parser::value_parser::{
    FfiValueParsingContext, FfiValueParsingContextKind, ParseContext, ParseOutcome, parse_css_value_with_source,
};
use crate::css::property_metadata::LAST_LONGHAND_PROPERTY_ID;
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::style_value::RetainedStyleValueData;
use crate::css::style_value::StyleValueData;

/// Mirrors the C++ `enum class CascadeOrigin : u8`.
/// https://drafts.csswg.org/css-cascade/#origin
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum CascadeOrigin {
    Author,
    /// https://drafts.csswg.org/css-cascade/#author-presentational-hint-origin
    AuthorPresentationalHint,
    User,
    UserAgent,
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

pub(crate) type WinningDeclaration<'a> = (
    &'a RetainedStyleValueData,
    bool,
    u32,
    bool,
    crate::css::style_compute::ExternalValueDependencies,
);

/// The immutable winner inputs used by longhand computation, without cascade history.
pub(crate) trait CascadedValues {
    fn winning_declaration(&self, property: u16) -> Option<WinningDeclaration<'_>>;
    fn winning_origin(&self, property: u16) -> Option<CascadeOrigin>;
    fn property_with_higher_priority(&self, first: u16, second: u16) -> u16;
}

impl CascadedValues for CascadedPropertyStore {
    fn winning_declaration(&self, property: u16) -> Option<WinningDeclaration<'_>> {
        self.winning_declaration(property)
    }
    fn winning_origin(&self, property: u16) -> Option<CascadeOrigin> {
        self.winning_origin(property)
    }
    fn property_with_higher_priority(&self, first: u16, second: u16) -> u16 {
        self.property_with_higher_priority(first, second)
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
    pub(crate) fn winning_declaration(&self, property_id: u16) -> Option<WinningDeclaration<'_>> {
        self.last_entry(property_id).map(|entry| {
            (
                &entry.value,
                entry.important,
                entry.source_slot,
                entry.has_style_sheet_context,
                entry.dependencies(),
            )
        })
    }

    pub(crate) fn winning_origin(&self, property_id: u16) -> Option<CascadeOrigin> {
        self.last_entry(property_id).map(|entry| entry.origin)
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
}

thread_local! {
    static STORE_POOL: RefCell<Vec<CascadedPropertyStore>> = const { RefCell::new(Vec::new()) };
}

pub(crate) fn parse_substituted_value(
    base_context: &ParseContext,
    property_id: u16,
    source: &[u16],
    values: &[ComponentValue],
    contains_attr_tainted_values: bool,
) -> RetainedStyleValueData {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::SubstitutionParse);
    let random_function_index = crate::css::parser::value_parser::RandomFunctionIndex::default();
    let value_context = FfiValueParsingContext {
        kind: FfiValueParsingContextKind::Property,
        value: property_id,
        secondary_value: 0,
        name: Default::default(),
    };
    let mut context = *base_context;
    context.is_substituted_value = true;
    context.contains_attr_tainted_values = contains_attr_tainted_values;
    context.value_contexts = ak::Span::new(std::slice::from_ref(&value_context));
    context.random_function_index = Some(&random_function_index);
    match parse_css_value_with_source(&context, property_id, values, source) {
        ParseOutcome::Parsed(value) => RetainedStyleValueData::from_arc(value),
        ParseOutcome::Invalid | ParseOutcome::NotHandled => {
            RetainedStyleValueData::from_owned(StyleValueData::GuaranteedInvalid)
        }
    }
}

/// What resolving substituted values inside the Rust cascade reads beyond the values themselves.
pub(crate) struct CascadeResolutionContext<'a> {
    pub(crate) inputs: crate::css::custom_properties::VarResolutionInputs<'a>,
    pub(crate) attributes: &'a [crate::css::custom_properties::SubstitutionAttribute<'a>],
    pub(crate) custom_functions: &'a crate::css::custom_properties::CustomFunctionRegistry<'a>,
    /// Where the custom properties and sibling position style queries read are kept, if anywhere.
    pub(crate) style_query_dependencies:
        Option<&'a mut Option<Box<crate::css::custom_properties::StyleQueryDependencies>>>,
}

/// Length, tree-counting and color inputs used when evaluating style queries.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiStyleQueryInputs {
    pub length: crate::css::style_compute::FfiLengthResolutionContext,
    pub tree_counting: Option<(u64, u64)>,
    /// A PreferredColorScheme code.
    pub color_scheme: u8,
    /// The packed currentcolor, as the C++ Color::value() packs it.
    pub current_color: u32,
}

impl FfiStyleQueryInputs {
    pub(crate) fn color_input(&self) -> crate::css::color_resolution::ColorResolutionInput<'_> {
        crate::css::color_resolution::ColorResolutionInput {
            scheme: Some(self.color_scheme),
            current_color: Some(crate::css::color_resolution::Rgba::from_packed(self.current_color)),
            current_color_value: None,
            length: Some(&self.length),
            channels: None,
        }
    }
}

/// Resolves the custom properties `store` declares, finalizing each registered value against
/// `finalization`. Answers the store holding them over the parent's, or none where the element
/// declares nothing that differs from the parent and may reuse its store; and what the registered
/// values read as they computed.
pub(crate) fn resolve_declared_custom_properties(
    store: &CustomPropertyStore,
    resolved_parent_store: Option<&Arc<CustomPropertyStore>>,
    reuse_resolved_parent_if_empty: bool,
    resolution_context: &mut CascadeResolutionContext,
    finalization: crate::css::custom_properties::CustomPropertyFinalization<'_>,
) -> (
    Option<Arc<CustomPropertyStore>>,
    crate::css::custom_properties::RegisteredValueReads,
) {
    let names = &store.declared_names;
    let mut environment = crate::css::custom_properties::prepare_var_resolution_environment(
        resolution_context.attributes,
        resolution_context.custom_functions,
    );
    let (outputs, reads) = crate::css::custom_properties::resolve_element_custom_properties(
        &resolution_context.inputs,
        &mut environment,
        resolution_context.style_query_dependencies.as_deref_mut(),
        finalization,
    );
    let resolved_parent = resolved_parent_store;
    let resolved_values: Vec<_> = names
        .iter()
        .zip(outputs)
        .filter(|(name_raw, value)| {
            !resolved_parent.is_some_and(|parent| parent.value_matches(**name_raw, value.data()))
        })
        .map(|(name_raw, value)| (*name_raw, value))
        .collect();
    let rust_store = if resolved_values.is_empty() && reuse_resolved_parent_if_empty {
        None
    } else {
        Some(store.resolved_child(resolved_parent_store.cloned(), resolved_values))
    };
    (rust_store, reads)
}

/// Sentinel passed when cascading for an element rather than a pseudo-element.
pub(crate) const NO_PSEUDO_ELEMENT: u8 = u8::MAX;
