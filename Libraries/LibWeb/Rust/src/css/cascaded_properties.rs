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

pub(crate) type WinningDeclaration = (
    *const c_void,
    bool,
    u32,
    bool,
    crate::css::style_compute::ExternalValueDependencies,
);

/// The immutable winner inputs used by longhand computation, without cascade history.
pub(crate) trait CascadedValues {
    fn winning_declaration(&self, property: u16) -> Option<WinningDeclaration>;
    fn winning_origin(&self, property: u16) -> Option<CascadeOrigin>;
    fn property_with_higher_priority(&self, first: u16, second: u16) -> u16;
}

impl CascadedValues for CascadedPropertyStore {
    fn winning_declaration(&self, property: u16) -> Option<WinningDeclaration> {
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
    pub custom_function_visibilities: *const crate::css::custom_properties::FfiSubstitutionFunctionVisibility,
    pub custom_function_visibility_count: usize,
    pub callback_context: *mut c_void,
    pub install_custom_properties: Option<
        unsafe extern "C" fn(*mut c_void, *const FfiCascadedCustomProperty, usize, *mut *const c_void) -> *const c_void,
    >,
    /// What a style query resolves against, or null to have `load_style_query_inputs` load it
    /// from the callback context the first time a query is evaluated.
    pub style_query_inputs: *const FfiStyleQueryInputs,
    pub load_style_query_inputs: Option<unsafe extern "C" fn(*mut c_void) -> *const FfiStyleQueryInputs>,
    /// Null, or where the custom properties style queries read are kept: null until the first,
    /// then a handle for `rust_style_query_dependencies_take`.
    pub style_query_dependencies: *mut *mut c_void,
    pub note_substitution: Option<unsafe extern "C" fn(*mut c_void, *const c_void)>,
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
    /// Whether computing a registered value resolved a viewport-relative length.
    pub depends_on_viewport_metrics: bool,
    /// Whether a registered value did not compute against the element's inputs, and took its
    /// invalid fallback instead.
    pub left_a_value_uncomputed: bool,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiCustomPropertyDriveInput {
    pub store: *const c_void,
    pub resolved_parent_store: *const c_void,
    pub reuse_resolved_parent_if_empty: bool,
    pub resolution_context: *const FfiCascadeResolutionContext,
    /// What a registered value computes against: the element's lengths as its style computes them
    /// after line-height, null where it declares no registered name; the facts its longhands are
    /// computed against, or null; and the color scheme its colors resolve with.
    pub finalization_length_resolution_context: *const crate::css::style_compute::FfiLengthResolutionContext,
    pub finalization_environment: *const crate::css::style_compute::FfiStyleComputationEnvironment,
    pub finalization_color_scheme: u8,
    /// Draws the random base value of a random caching key a registered value holds only once
    /// substituted, which the host could not draw beforehand; none where nothing may be drawn.
    pub draw_random_base_value: Option<unsafe extern "C" fn(*mut c_void, *const u16, usize, bool) -> f64>,
    pub random_base_context: *mut c_void,
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

/// What the drive finalizes each declared value with: the store declaring the values, in the
/// order of `names`, the registry and the environment inherited, and the element's finalization
/// inputs.
struct CustomPropertyFinalizerContext<'a> {
    store: &'a CustomPropertyStore,
    names: &'a [usize],
    registry: &'a crate::css::custom_properties::CustomPropertyRegistry,
    inherited: Option<&'a CustomPropertyStore>,
    finalization: crate::css::custom_properties::CustomPropertyFinalization<'a>,
    reads: Cell<crate::css::custom_properties::RegisteredValueReads>,
    left_a_value_uncomputed: Cell<bool>,
}

unsafe extern "C" fn finalize_custom_property_component(
    context: *mut c_void,
    members: *const u32,
    member_count: usize,
    outputs: *mut FfiResolvedStyleValue,
) {
    let context = unsafe { &*context.cast::<CustomPropertyFinalizerContext>() };
    for &member in unsafe { std::slice::from_raw_parts(members, member_count) } {
        let name_raw = context.names[member as usize];
        let name = &context
            .store
            .own_values
            .get(&name_raw)
            .expect("declared custom property must be an own value")
            .name;
        // SAFETY: The resolver hands each member's output over with one transferred reference,
        //         and takes one back.
        let output = unsafe { &mut *outputs.add(member as usize) };
        let substituted = unsafe { RetainedStyleValueData::from_retained_pointer(output.data.cast()) };
        let (finalized, reads) = context
            .registry
            .finalize_custom_property_value(context.inherited, name_raw, name, substituted, &context.finalization)
            .unwrap_or_else(|fallback| {
                context.left_a_value_uncomputed.set(true);
                (fallback, Default::default())
            });
        let mut all_reads = context.reads.get();
        all_reads |= reads;
        context.reads.set(all_reads);
        output.data = std::sync::Arc::into_raw(finalized.into_arc()).cast();
    }
}

/// What `drive_custom_property_resolution` resolves, finalizing against `finalization` rather than
/// what `input` says, and what the registered values read as they computed.
///
/// # Safety
/// As for `drive_custom_property_resolution`.
pub(crate) unsafe fn resolve_declared_custom_properties(
    input: &FfiCustomPropertyDriveInput,
    finalization: crate::css::custom_properties::CustomPropertyFinalization<'_>,
) -> (
    FfiResolvedCustomProperties,
    crate::css::custom_properties::RegisteredValueReads,
) {
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
    // SAFETY: The resolution context and what it names are live for the call.
    let resolution_context = unsafe { &*input.resolution_context };
    let mut finalizer_context = CustomPropertyFinalizerContext {
        store,
        names,
        registry: unsafe {
            resolution_context
                .custom_property_registry
                .cast::<crate::css::custom_properties::CustomPropertyRegistry>()
                .as_ref()
        }
        .unwrap_or_else(crate::css::custom_properties::CustomPropertyRegistry::shared_empty),
        inherited: unsafe {
            resolution_context
                .inheritance_custom_property_store
                .cast::<CustomPropertyStore>()
                .as_ref()
        },
        finalization,
        reads: Cell::default(),
        left_a_value_uncomputed: Cell::new(false),
    };
    let mut stats = unsafe {
        resolve_unresolved_style_values(
            input.resolution_context,
            inputs.as_ptr(),
            inputs.len(),
            outputs.as_mut_ptr(),
            std::ptr::from_mut(&mut finalizer_context).cast(),
            Some(finalize_custom_property_component),
        )
    };
    let reads = finalizer_context.reads.get();
    stats.depends_on_viewport_metrics = reads.viewport;
    stats.left_a_value_uncomputed = finalizer_context.left_a_value_uncomputed.get();
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
    let resolved = FfiResolvedCustomProperties {
        properties: storage.cast::<FfiResolvedCustomProperty>(),
        count,
        did_resolve: true,
        rust_store,
        stats,
        storage: storage.cast(),
    };
    (resolved, reads)
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

/// Sentinel passed when cascading for an element rather than a pseudo-element.
pub(crate) const NO_PSEUDO_ELEMENT: u8 = u8::MAX;

#[allow(clippy::arc_with_non_send_sync)]
pub(crate) fn resolve_cascade_value(
    resolution_context: &FfiCascadeResolutionContext,
    resolution_environment: Option<&mut crate::css::custom_properties::VarResolutionEnvironment>,
    property_id: u16,
    unresolved_data: *const c_void,
    final_custom_properties: Option<&HashMap<Vec<u16>, *const c_void>>,
) -> RetainedStyleValueData {
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
                property_id,
                resolution_context.root_custom_property_name,
                unresolved_data,
                resolution_environment,
                resolution_context.attribute_names_are_ascii_case_insensitive,
                resolution_context.callback_context,
                resolution_context.style_query_inputs.as_ref(),
                resolution_context.load_style_query_inputs,
                resolution_context
                    .style_query_dependencies
                    .cast::<Option<Box<crate::css::custom_properties::StyleQueryDependencies>>>()
                    .as_mut(),
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
                return RetainedStyleValueData::from_owned(StyleValueData::GuaranteedInvalid);
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
    unsafe { RetainedStyleValueData::from_retained_pointer(std::sync::Arc::into_raw(parsed)) }
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
unsafe fn resolve_unresolved_style_values(
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
            resolution_context.custom_function_visibilities,
            resolution_context.custom_function_visibility_count,
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
                use_final_custom_properties.then_some(&final_custom_properties),
            );
            outputs[member as usize].data = resolved.pointer().cast();
            std::mem::forget(resolved);
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
        depends_on_viewport_metrics: false,
        left_a_value_uncomputed: false,
    }
}
