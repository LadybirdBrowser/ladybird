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
use crate::css::parser::value_parser::{
    FfiValueParsingContext, FfiValueParsingContextKind, ParseContext, ParseOutcome, parse_css_value_from_source,
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
        document_url: input.document_url.as_ptr(),
        document_url_length: input.document_url.len(),
        document_base_url: input.document_base_url.as_ptr(),
        document_base_url_length: input.document_base_url.len(),
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

/// What resolving substituted values inside the Rust cascade reads beyond the values themselves.
pub(crate) struct CascadeResolutionContext<'a> {
    pub(crate) inputs: crate::css::custom_properties::VarResolutionInputs<'a>,
    pub(crate) attributes: &'a [crate::css::custom_properties::SubstitutionAttribute<'a>],
    pub(crate) custom_functions: &'a [crate::css::custom_properties::SubstitutionFunctionDefinition],
    pub(crate) custom_function_scope_identity: usize,
    pub(crate) custom_function_visibilities: &'a [crate::css::custom_properties::FfiSubstitutionFunctionVisibility],
    /// Where the custom properties style queries read are kept, if anywhere.
    pub(crate) style_query_dependencies:
        Option<&'a mut Option<Box<crate::css::custom_properties::StyleQueryDependencies>>>,
}

/// What a `style()` query in a substitution resolves against, as the queried element's style
/// computes it: its lengths, and the color scheme and currentcolor its colors resolve with.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiStyleQueryInputs {
    pub length: crate::css::style_compute::FfiLengthResolutionContext,
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

/// One unresolved value submitted to the bulk substitution resolver.
struct UnresolvedStyleValue<'a> {
    property_id: u16,
    /// The custom property the value is declared for, or empty for another property.
    root_custom_property_name: &'a [u16],
    value: &'a RetainedStyleValueData,
    resolve_substitutions: bool,
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

/// Resolves the custom properties `store` declares, finalizing each registered value against
/// `finalization`. Answers the store holding them over the parent's, which transfers one strong
/// reference to the caller, or null where the element declares nothing that differs from the
/// parent and may reuse its store; and what the registered values read as they computed.
///
/// # Safety
/// `resolved_parent_store` must be null or a live raw `Arc` pointer to a `CustomPropertyStore`.
pub(crate) unsafe fn resolve_declared_custom_properties(
    store: &CustomPropertyStore,
    resolved_parent_store: *const c_void,
    reuse_resolved_parent_if_empty: bool,
    resolution_context: &mut CascadeResolutionContext,
    finalization: crate::css::custom_properties::CustomPropertyFinalization<'_>,
) -> (*const c_void, crate::css::custom_properties::RegisteredValueReads) {
    let names = &store.declared_names;
    let own_entry = |name_raw: &usize| {
        store
            .own_values
            .get(name_raw)
            .expect("declared custom property must be an own value")
    };
    let inputs: Vec<_> = names
        .iter()
        .map(|name_raw| {
            let entry = own_entry(name_raw);
            UnresolvedStyleValue {
                property_id: crate::css::property_metadata::property_id::CUSTOM,
                root_custom_property_name: &entry.name,
                value: &entry.value,
                resolve_substitutions: custom_property_needs_resolution(entry.value.data()),
            }
        })
        .collect();
    let registry = resolution_context
        .inputs
        .registry
        .unwrap_or_else(|| crate::css::custom_properties::CustomPropertyRegistry::shared_empty());
    let inherited = resolution_context.inputs.inheritance_store;
    let mut reads = crate::css::custom_properties::RegisteredValueReads::default();
    let mut finalize_component = |members: &[u32], outputs: &mut [RetainedStyleValueData]| {
        for &member in members {
            let name_raw = names[member as usize];
            let substituted = std::mem::replace(&mut outputs[member as usize], RetainedStyleValueData::none());
            let (finalized, value_reads) = registry
                .finalize_custom_property_value(
                    inherited,
                    name_raw,
                    &own_entry(&name_raw).name,
                    substituted,
                    &finalization,
                )
                .unwrap_or_else(|fallback| (fallback, Default::default()));
            reads |= value_reads;
            outputs[member as usize] = finalized;
        }
    };
    let outputs = resolve_unresolved_style_values(resolution_context, &inputs, Some(&mut finalize_component));
    // SAFETY: Guaranteed by the caller.
    let resolved_parent = unsafe { resolved_parent_store.cast::<CustomPropertyStore>().as_ref() };
    let resolved_values: Vec<_> = names
        .iter()
        .zip(outputs)
        .filter(|(name_raw, value)| {
            !resolved_parent.is_some_and(|parent| parent.value_matches(**name_raw, value.data()))
        })
        .map(|(name_raw, value)| (*name_raw, value))
        .collect();
    let rust_store = if resolved_values.is_empty() && reuse_resolved_parent_if_empty {
        std::ptr::null()
    } else {
        // SAFETY: Guaranteed by the caller.
        unsafe { store.resolved_child(resolved_parent_store, resolved_values) }
    };
    (rust_store, reads)
}

/// Sentinel passed when cascading for an element rather than a pseudo-element.
pub(crate) const NO_PSEUDO_ELEMENT: u8 = u8::MAX;

#[allow(clippy::arc_with_non_send_sync)]
fn resolve_cascade_value(
    resolution_context: &mut CascadeResolutionContext,
    resolution_environment: Option<&mut crate::css::custom_properties::VarResolutionEnvironment>,
    property_id: u16,
    root_custom_property_name: &[u16],
    unresolved: &StyleValueData,
    final_custom_properties: Option<&HashMap<Vec<u16>, RetainedStyleValueData>>,
) -> RetainedStyleValueData {
    let native_resolution = match resolution_environment {
        Some(resolution_environment) => crate::css::custom_properties::resolve_vars(
            &resolution_context.inputs,
            property_id,
            root_custom_property_name,
            unresolved,
            resolution_environment,
            resolution_context.style_query_dependencies.as_deref_mut(),
            final_custom_properties,
        ),
        None => crate::css::custom_properties::NativeVarResolution::NotHandled,
    };

    let parsed = match native_resolution {
        crate::css::custom_properties::NativeVarResolution::Resolved {
            source,
            contains_attr_tainted_values,
        } => {
            let Some(base_context) = resolution_context.inputs.parse_context else {
                return RetainedStyleValueData::from_owned(StyleValueData::GuaranteedInvalid);
            };
            let contains_attr_tainted_values = contains_attr_tainted_values
                || matches!(
                    unresolved,
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
    RetainedStyleValueData::from_arc(parsed)
}

fn custom_property_components(inputs: &[UnresolvedStyleValue]) -> (Vec<Vec<u32>>, bool) {
    let mut indices = HashMap::new();
    for (index, input) in inputs.iter().enumerate() {
        indices.insert(input.root_custom_property_name.to_vec(), index as u32);
    }

    let mut edges = vec![Vec::new(); inputs.len()];
    let mut has_own_reference = false;
    for (index, input) in inputs.iter().enumerate() {
        let value = input.value.data();
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
                if let Some(target) = indices.get(reference) {
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
                components.push(component);
            }
            walk_stack.pop();
            if let Some(&(parent, _)) = walk_stack.last() {
                lowlink[parent as usize] = lowlink[parent as usize].min(lowlink[node_index]);
            }
        }
    }
    (components, has_own_reference)
}

/// Finalizes the values of one component of custom properties, by their indices among the outputs.
type FinalizeComponent<'a> = dyn FnMut(&[u32], &mut [RetainedStyleValueData]) + 'a;

/// Resolves and parses unresolved values outside the longhand cascade, one output per input. When
/// a finalizer is supplied, custom properties are resolved in dependency order and each component
/// is finalized before later components can read it.
fn resolve_unresolved_style_values(
    resolution_context: &mut CascadeResolutionContext,
    inputs: &[UnresolvedStyleValue],
    mut finalize_component: Option<&mut FinalizeComponent>,
) -> Vec<RetainedStyleValueData> {
    let mut outputs: Vec<_> = inputs.iter().map(|_| RetainedStyleValueData::none()).collect();
    let mut resolution_environment = crate::css::custom_properties::prepare_var_resolution_environment(
        resolution_context.attributes,
        resolution_context.custom_functions,
        resolution_context.custom_function_scope_identity,
        resolution_context.custom_function_visibilities,
    );
    let (components, use_final_custom_properties) = if finalize_component.is_some() {
        custom_property_components(inputs)
    } else {
        ((0..inputs.len() as u32).map(|index| vec![index]).collect(), false)
    };
    let mut final_custom_properties = HashMap::new();
    for mut component in components {
        component.sort_unstable();
        for &member in &component {
            let input = &inputs[member as usize];
            if !input.resolve_substitutions {
                outputs[member as usize] = input.value.clone();
                continue;
            }
            outputs[member as usize] = resolve_cascade_value(
                resolution_context,
                resolution_environment.as_mut(),
                input.property_id,
                input.root_custom_property_name,
                input.value.data(),
                use_final_custom_properties.then_some(&final_custom_properties),
            );
        }
        if let Some(finalize_component) = finalize_component.as_deref_mut() {
            finalize_component(&component, &mut outputs);
            for &member in &component {
                final_custom_properties.insert(
                    inputs[member as usize].root_custom_property_name.to_vec(),
                    outputs[member as usize].clone(),
                );
            }
        }
    }
    outputs
}
