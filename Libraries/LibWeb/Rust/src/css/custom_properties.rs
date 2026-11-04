/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Rust-owned custom-property data with the same structurally shared parent shape as the C++
//! `CustomPropertyData` shell.

use crate::css::style_value::{RandomValueSharingPayload, UnresolvedPayload};
use std::borrow::Cow;
use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::collections::HashSet;
use std::ffi::c_void;
use std::rc::Rc;
use std::sync::{Arc, Weak};

use crate::css::thin_slice::ThinSlice;
use smallvec::SmallVec;

use ak::{ScopeGuard, Utf16FlyString};

use crate::css::calc::CanonicalNumericType;
use crate::css::cascaded_properties::parse_substituted_value;
use crate::css::css_enums::keyword_from_ascii_case_insensitive;
use crate::css::css_tokenizer::{CssNumberType, ParserString, ParserTokenKind, TokenizerInput, tokenize_for_parser};
use crate::css::ffi_support::{FfiUtf16View, OwnedArc};
use crate::css::function_signature::FunctionSignature;
use crate::css::parser::arbitrary_substitution::{
    arbitrary_substitution_function, arguments_are_valid, collect_arbitrary_substitution_function_presence,
    contains_arbitrary_substitution_function, declaration_value_is_valid,
};
use crate::css::parser::component_value::{
    ComponentKind, ComponentSerializationMode, ComponentValue, ComponentValueList, ComponentValueSlice,
    consume_a_counted_list_of_component_values, trim_whitespace,
};
use crate::css::parser::query_parser::{
    FeatureComparison, FfiMediaEnvironment, MatchResult, StyleFeature, StyleRangeValue as ParsedStyleRangeValue,
    parse_and_evaluate_media_if_condition, parse_and_evaluate_supports_if_condition, parse_style_range,
};
use crate::css::parser::syntax::{SyntaxNode, SyntaxType, parse_syntax_values, parse_with_syntax_component_values};
use crate::css::parser::value_parser::{
    FfiValueParsingContext, FfiValueParsingContextKind, ParseContext, RandomFunctionIndex, is_css_wide_keyword,
};
use crate::css::property_metadata::property_id;
use crate::css::serialize::{
    normalized_component_values_equal, serialize_component_values_to_utf16, serialize_resolved_style_value_to_utf16,
};
use crate::css::style_value::RetainedString;
use crate::css::style_value::RetainedStyleValueData;
use crate::css::style_value::StyleValueData;
use crate::fast_hash::FastMap;

include!(concat!(env!("OUT_DIR"), "/environment_variables_generated.rs"));

pub(crate) fn environment_variable_is_known(name: &[u16]) -> bool {
    ENVIRONMENT_VARIABLES
        .iter()
        .any(|(known_name, _, _)| name.eq_ignore_ascii_case(known_name))
}
// Keep these structural-sharing limits aligned with CustomPropertyData.cpp.
const MAX_ANCESTOR_COUNT: u8 = 32;
const ABSORB_THRESHOLD: usize = 8;

pub(crate) trait Utf16SliceExt {
    fn eq_ignore_ascii_case(&self, expected: &str) -> bool;
    fn eq_ignore_ascii_case_utf16(&self, expected: &[u16]) -> bool;
    fn starts_with_ascii(&self, expected: &str) -> bool;
}

impl Utf16SliceExt for [u16] {
    fn eq_ignore_ascii_case(&self, expected: &str) -> bool {
        self.len() == expected.len()
            && self
                .iter()
                .zip(expected.bytes())
                .all(|(&left, right)| left <= 0x7f && (left as u8).eq_ignore_ascii_case(&right))
    }

    fn starts_with_ascii(&self, expected: &str) -> bool {
        self.len() >= expected.len()
            && self[..expected.len()]
                .iter()
                .zip(expected.bytes())
                .all(|(&left, right)| left == u16::from(right))
    }

    fn eq_ignore_ascii_case_utf16(&self, expected: &[u16]) -> bool {
        self.len() == expected.len()
            && self.iter().zip(expected).all(|(&left, &right)| {
                left == right
                    || u8::try_from(left)
                        .ok()
                        .zip(u8::try_from(right).ok())
                        .is_some_and(|(left, right)| left.eq_ignore_ascii_case(&right))
            })
    }
}

#[repr(C)]
pub struct FfiCustomPropertyStoreEntry {
    pub name_raw: usize,
    pub important: bool,
    pub data: *const c_void,
}

#[derive(Clone)]
pub(crate) struct CustomPropertyEntry {
    pub(crate) name: Utf16FlyString,
    pub(crate) value: RetainedStyleValueData,
    pub(crate) important: bool,
    /// Where the store holding the entry declared it among its own declarations, from one, or none
    /// for a value it took in from the chain above it.
    pub(crate) declared: Option<std::num::NonZeroU32>,
}

const _: () = assert!(size_of::<CustomPropertyEntry>() == 24);

impl CustomPropertyEntry {
    /// The entry, as one its store took in rather than declared.
    fn taken_in(&self) -> Self {
        Self {
            declared: None,
            ..self.clone()
        }
    }
}

/// A store's own entries, sorted by the identities of their names, which a lookup binary-searches:
/// as small as the entries themselves, where a hash table holds a key beside each and room for
/// more. A store holds a few entries.
#[derive(Clone, Default)]
pub(crate) struct CustomPropertyEntries(ThinSlice<CustomPropertyEntry>);

/// The entries of a store being built, which [`Self::finished`] makes the store's.
#[derive(Default)]
struct CustomPropertyEntriesBuilder(Vec<CustomPropertyEntry>);

impl CustomPropertyEntriesBuilder {
    fn position(&self, name_raw: usize) -> Result<usize, usize> {
        self.0
            .binary_search_by_key(&name_raw, |entry| entry.name.raw_identity())
    }

    /// Enters `entry` under its name, `name_raw`, in place of the one there.
    fn insert(&mut self, name_raw: usize, entry: CustomPropertyEntry) {
        debug_assert_eq!(name_raw, entry.name.raw_identity());
        match self.position(name_raw) {
            Ok(index) => self.0[index] = entry,
            Err(index) => self.0.insert(index, entry),
        }
    }

    /// Enters `entries` in order as the store's declarations, each in place of one of its name.
    fn declare(&mut self, entries: impl IntoIterator<Item = (usize, CustomPropertyEntry)>) {
        for (position, (name_raw, mut entry)) in entries.into_iter().enumerate() {
            let position = u32::try_from(position + 1).expect("a store declares fewer than 2^32 custom properties");
            entry.declared = std::num::NonZeroU32::new(position);
            self.insert(name_raw, entry);
        }
    }

    /// Enters a copy of `entry`, as taken in, where no entry of its name is.
    fn insert_if_vacant(&mut self, entry: &CustomPropertyEntry) {
        if let Err(index) = self.position(entry.name.raw_identity()) {
            self.0.insert(index, entry.taken_in());
        }
    }

    fn finished(self) -> CustomPropertyEntries {
        CustomPropertyEntries(self.0.into_iter().collect())
    }
}

impl CustomPropertyEntries {
    fn position(&self, name_raw: usize) -> Result<usize, usize> {
        self.0
            .binary_search_by_key(&name_raw, |entry| entry.name.raw_identity())
    }

    pub(crate) fn get(&self, name_raw: &usize) -> Option<&CustomPropertyEntry> {
        self.position(*name_raw).ok().map(|index| &self.0[index])
    }

    pub(crate) fn len(&self) -> usize {
        self.0.len()
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    pub(crate) fn iter(&self) -> impl Iterator<Item = (usize, &CustomPropertyEntry)> {
        self.0.iter().map(|entry| (entry.name.raw_identity(), entry))
    }

    pub(crate) fn keys(&self) -> impl Iterator<Item = usize> + '_ {
        self.0.iter().map(|entry| entry.name.raw_identity())
    }

    pub(crate) fn values(&self) -> impl Iterator<Item = &CustomPropertyEntry> {
        self.0.iter()
    }

    /// A builder holding these entries, as taken in where `taken_in`.
    fn to_builder(&self, taken_in: bool) -> CustomPropertyEntriesBuilder {
        CustomPropertyEntriesBuilder(
            self.0
                .iter()
                .map(|entry| if taken_in { entry.taken_in() } else { entry.clone() })
                .collect(),
        )
    }
}

impl std::ops::Index<&usize> for CustomPropertyEntries {
    type Output = CustomPropertyEntry;

    fn index(&self, name_raw: &usize) -> &CustomPropertyEntry {
        self.get(name_raw).expect("a store holds the entry asked for")
    }
}

impl FromIterator<(usize, CustomPropertyEntry)> for CustomPropertyEntries {
    fn from_iter<I: IntoIterator<Item = (usize, CustomPropertyEntry)>>(entries: I) -> Self {
        let mut collected = CustomPropertyEntriesBuilder::default();
        for (name_raw, entry) in entries {
            collected.insert(name_raw, entry);
        }
        collected.finished()
    }
}

/// How many answers of [`CustomPropertyStore::hold_the_same_values`] a thread keeps.
const COMPARED_STORE_SLOTS: usize = 64;

/// What comparing two stores answered, which stands for as long as it is kept: a store is immutable,
/// and the weak references keep another store from taking the place of either without keeping
/// anything it holds.
struct ComparedStores {
    first: Weak<CustomPropertyStore>,
    second: Weak<CustomPropertyStore>,
    same: bool,
}

thread_local! {
    static COMPARED_STORES: RefCell<[Option<ComparedStores>; COMPARED_STORE_SLOTS]> =
        const { RefCell::new([const { None }; COMPARED_STORE_SLOTS]) };
}

pub struct CustomPropertyStore {
    /// The values the store holds itself: those it declares, each knowing where among its
    /// declarations, and those it took in from the chain above it.
    pub(crate) own_values: CustomPropertyEntries,
    parent: Option<Arc<CustomPropertyStore>>,
    inheritance_parent: Option<Arc<CustomPropertyStore>>,
    /// For the values an element's animations sample, the store of the environment they lie over,
    /// whose values this one copies.
    animation_overlay_base: Option<Arc<CustomPropertyStore>>,
    ancestor_count: u8,
}

// NB: The host holds a store as its custom-property environment itself, so this is all an
//     environment takes.
const _: () = assert!(size_of::<CustomPropertyStore>() == 40);

impl crate::heap_size::HeapSize for CustomPropertyStore {
    /// The store's table, its names and the stores it lies over, each counted once. A name's text
    /// belongs to the process's fly strings and is not counted.
    fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        self.own_values.0.heap_bytes()
            + self
                .own_values
                .values()
                .map(|entry| entry.value.heap_size(sizer))
                .sum::<usize>()
            + self.parent.heap_size(sizer)
            + self.inheritance_parent.heap_size(sizer)
            + self.animation_overlay_base.heap_size(sizer)
    }
}

// SAFETY: Store nodes and their entries are immutable after construction. Style workers only
// borrow the graph while resolving substitution text; the C++-owned raw Arc reference remains
// alive until every worker in the blocking batch has joined, so destruction stays on the main
// thread. The retained style values and names are likewise only read by workers.
unsafe impl Send for CustomPropertyStore {}
unsafe impl Sync for CustomPropertyStore {}

pub struct CustomPropertyRegistry {
    registrations: FastMap<Utf16FlyString, RegisteredCustomProperty>,
    document_url: Vec<u8>,
    document_base_url: Vec<u8>,
    /// What a value parses to with a registration's syntax, by registration and value identity,
    /// keeping the value alive so its identity is not reused: one declared value is parsed for
    /// what its computation reads, then again to finalize it, for every element declaring it.
    parses: std::sync::Mutex<HashMap<(usize, usize), RegisteredValueParse>>,
}

/// A value the registry has parsed, kept alive, and what it parsed to.
type RegisteredValueParse = (RetainedStyleValueData, Option<RetainedStyleValueData>);

pub(crate) enum CustomPropertyValueToFinalize<'a> {
    StyleValue(RetainedStyleValueData),
    SubstitutedComponents {
        components: ComponentValueList,
        parse_context: &'a ParseContext<'a>,
        contains_attr_tainted_values: bool,
    },
}

impl CustomPropertyValueToFinalize<'_> {
    fn to_css_wide_keyword(&self) -> Option<u16> {
        let keyword = match self {
            Self::StyleValue(value) => match value.data() {
                StyleValueData::Keyword { keyword } => *keyword,
                _ => return None,
            },
            Self::SubstitutedComponents { components, .. } => {
                keyword_from_ascii_case_insensitive(single_css_wide_keyword(components)?)?
            }
        };
        is_css_wide_keyword(keyword).then_some(keyword)
    }

    fn contains_attr_tainted_values(&self) -> bool {
        match self {
            Self::StyleValue(value) => matches!(
                value.data(),
                StyleValueData::Unresolved(unresolved) if unresolved.contains_attr_tainted_values
            ),
            Self::SubstitutedComponents {
                contains_attr_tainted_values,
                ..
            } => *contains_attr_tainted_values,
        }
    }
}

/// How many parses the registry keeps before it starts over.
const MAX_REGISTERED_VALUE_PARSES: usize = 4096;

#[derive(PartialEq)]
struct RegisteredCustomProperty {
    /// The syntax the registration's rule or call parsed, shared with it.
    syntax: Arc<SyntaxNode>,
    inherits: bool,
    initial_source: Option<RetainedString>,
    /// The initial value as it computes, against the document rather than any element: the host
    /// computes it, memoizes it on the registration for every reader, and publishes it with the
    /// registration. The guaranteed-invalid value for a registration without one.
    computed_initial: RetainedStyleValueData,
}

type CustomFunctionIdentity = u64;

pub(crate) struct PreparedCustomFunction<'a> {
    pub(crate) identity: CustomFunctionIdentity,
    pub(crate) scope_identity: usize,
    pub(crate) signature: &'a FunctionSignature,
    pub(crate) parameter_defaults: &'a [Option<ComponentValueList>],
    pub(crate) declarations: Vec<&'a SubstitutionFunctionDeclaration>,
}

pub(crate) struct CustomFunctionRegistry<'a> {
    pub(crate) caller_scope_identity: usize,
    pub(crate) definitions: Vec<PreparedCustomFunction<'a>>,
    /// Which definition a call names from each scope that sees one: the caller scope and the
    /// index of the definition its name resolves to there.
    pub(crate) visible: &'a [CustomFunctionVisibility],
}

impl CustomFunctionRegistry<'_> {
    pub(crate) const fn empty() -> Self {
        Self {
            caller_scope_identity: 0,
            definitions: Vec::new(),
            visible: &[],
        }
    }
}

#[derive(Clone)]
struct FunctionLocalRegistration<'a> {
    syntax: &'a SyntaxNode,
    initial_tokens: Option<ComponentValueList>,
    is_result: bool,
}

#[derive(Clone)]
struct FunctionLocalValue<'a> {
    tokens: Cow<'a, ComponentValueList>,
    includes_substitution: bool,
}

struct FunctionLocalScope<'a> {
    function_identity: CustomFunctionIdentity,
    resolved_value_cache: Rc<RefCell<HashMap<Utf16FlyString, ComponentValueList>>>,
    values: HashMap<Utf16FlyString, FunctionLocalValue<'a>>,
    registrations: Rc<HashMap<Utf16FlyString, FunctionLocalRegistration<'a>>>,
}

#[repr(C)]
pub struct FfiCustomPropertyRegistration<'a> {
    pub name: FfiUtf16View<'a>,
    pub syntax: *const c_void,
    pub inherits: bool,
    pub has_initial_value: bool,
    pub initial_value: FfiUtf16View<'a>,
    /// The initial value as the host computed it against the document, borrowed for the call.
    pub computed_initial_value: *const c_void,
}

#[repr(C)]
pub struct FfiCustomPropertyRegistryContext {
    pub document_url: *const u8,
    pub document_url_length: usize,
    pub document_base_url: *const u8,
    pub document_base_url_length: usize,
}

/// One of an element's attributes an `attr()` reads: its local name, and its value's text.
pub(crate) struct SubstitutionAttribute<'a> {
    pub(crate) name: &'a [u16],
    /// The host's string of the value, which the element's attribute holds.
    pub(crate) value: &'a crate::css::retained_fly_string::RetainedUtf16FlyString,
}

/// One declaration in the body of a custom function, as its conditions select it.
pub(crate) struct SubstitutionFunctionDeclaration {
    name: Utf16FlyString,
    tokens: ComponentValueList,
    includes_substitution: bool,
}

impl SubstitutionFunctionDeclaration {
    pub(crate) fn tokens(&self) -> &[ComponentValue] {
        self.tokens.as_ref()
    }
}

/// A definition visible to a caller scope, indexed in the prepared definition list.
#[derive(Clone, Copy)]
pub(crate) struct CustomFunctionVisibility {
    pub(crate) caller_scope_identity: usize,
    pub(crate) definition_index: usize,
}

/// That a call from a scope names a function: the definition its name dereferences to there, as
/// a tree-scoped reference.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SubstitutionFunctionVisibility {
    pub caller_scope_identity: usize,
    pub function_identity: u64,
}

impl CustomPropertyRegistry {
    /// The registry of a document that registers no custom property.
    fn empty() -> Self {
        Self {
            registrations: FastMap::default(),
            document_url: Vec::new(),
            document_base_url: Vec::new(),
            parses: Default::default(),
        }
    }

    /// One empty registry for every document input that names none.
    pub(crate) fn shared_empty() -> &'static Self {
        static EMPTY: std::sync::LazyLock<CustomPropertyRegistry> =
            std::sync::LazyLock::new(CustomPropertyRegistry::empty);
        &EMPTY
    }

    /// Whether any custom property is registered; an unregistered name resolves without a syntax.
    pub(crate) fn has_registrations(&self) -> bool {
        !self.registrations.is_empty()
    }

    /// Whether any registration says its name does not inherit. Such a name is in an element's own
    /// environment and in none of its descendants', which a store built over the parent's cannot
    /// say.
    pub(crate) fn has_non_inheriting_registrations(&self) -> bool {
        self.registrations.values().any(|registration| !registration.inherits)
    }

    fn registration(&self, name: &[u16]) -> Option<&RegisteredCustomProperty> {
        if self.registrations.is_empty() {
            return None;
        }
        self.registrations.get(&Utf16FlyString::from_utf16(name))
    }

    /// Whether the custom property `name` names is registered.
    pub(crate) fn is_registered(&self, name: &Utf16FlyString) -> bool {
        self.registrations.contains_key(name)
    }

    /// Whether a name has the same effective registration in two document registries.
    pub(crate) fn registration_matches(&self, other: &Self, name: &[u16]) -> bool {
        self.registration(name) == other.registration(name)
    }

    /// The names whose effective registration differs between `previous` and this registry: those
    /// registered, unregistered or registered otherwise since.
    pub(crate) fn names_registered_otherwise_than(&self, previous: &Self) -> Vec<Utf16FlyString> {
        let moved_in = |registry: &Self, other: &Self| {
            registry
                .registrations
                .iter()
                .filter(|(name, registration)| other.registrations.get(*name) != Some(*registration))
                .map(|(name, _)| name.clone())
                .collect::<Vec<_>>()
        };
        let mut names = moved_in(self, previous);
        names.extend(
            moved_in(previous, self)
                .into_iter()
                .filter(|name| !self.registrations.contains_key(name)),
        );
        names
    }

    /// Whether a custom property of this name inherits: only a registration can say it does not.
    pub(crate) fn name_inherits(&self, name: &Utf16FlyString) -> bool {
        self.registrations
            .get(name)
            .is_none_or(|registration| registration.inherits)
    }

    /// Whether a custom property of this name is registered with a syntax other than the universal
    /// one, which its values compute against.
    pub(crate) fn name_has_syntax(&self, name: &[u16]) -> bool {
        self.registration(name)
            .is_some_and(|registration| !matches!(*registration.syntax, SyntaxNode::Universal))
    }

    /// [`Self::name_has_syntax`], for the custom property `name` names.
    pub(crate) fn has_syntax(&self, name: &Utf16FlyString) -> bool {
        self.registrations
            .get(name)
            .is_some_and(|registration| !matches!(*registration.syntax, SyntaxNode::Universal))
    }

    /// Whether a registration's syntax permits smooth interpolation.
    pub(crate) fn name_has_interpolable_syntax(&self, name: &[u16]) -> bool {
        fn is_interpolable(syntax: &SyntaxNode) -> bool {
            match syntax {
                SyntaxNode::Universal
                | SyntaxNode::Ident { .. }
                | SyntaxNode::Type(
                    SyntaxType::CustomIdent | SyntaxType::Image | SyntaxType::String | SyntaxType::Url,
                ) => false,
                SyntaxNode::Type(_) => true,
                SyntaxNode::Multiplier { child, .. } => is_interpolable(child),
                SyntaxNode::Alternatives(alternatives) => alternatives.iter().any(is_interpolable),
            }
        }
        self.registration(name)
            .is_some_and(|registration| is_interpolable(&registration.syntax))
    }

    /// https://drafts.csswg.org/css-variables-2/#invalid-at-computed-value-time
    /// What a custom property computes to where it is invalid at computed-value time: as `unset`, what it
    /// inherits from `inherited` where its registration inherits, and otherwise its initial value.
    pub(crate) fn invalid_at_computed_value_time(
        &self,
        name: &crate::css::retained_fly_string::RetainedUtf16FlyString,
        inherited: Option<&CustomPropertyStore>,
    ) -> RetainedStyleValueData {
        let inherits = self
            .registration(name.to_utf16().as_ref())
            .is_none_or(|registration| registration.inherits);
        inherited
            .filter(|_| inherits)
            .and_then(|inherited| inherited.get(name.raw()))
            .map_or_else(
                || self.initial_value(name.to_utf16().as_ref()),
                |entry| entry.value.clone(),
            )
    }

    /// A custom property's initial value: what its registration's initial value computes to, and
    /// the guaranteed-invalid value for an unregistered name or a registration without one.
    pub(crate) fn initial_value(&self, name: &[u16]) -> RetainedStyleValueData {
        self.registration(name).map_or_else(
            || RetainedStyleValueData::from_owned(StyleValueData::GuaranteedInvalid),
            |registration| registration.computed_initial.clone(),
        )
    }

    pub(crate) fn parse_context<'a>(&'a self, random_function_index: &'a RandomFunctionIndex) -> ParseContext<'a> {
        ParseContext {
            in_quirks_mode: false,
            is_svg_presentation_attribute: false,
            is_substituted_value: false,
            contains_attr_tainted_values: false,
            is_ua_style_sheet: false,
            value_contexts: ak::Span::default(),
            declared_namespaces: None,
            document_url: ak::Span::new(&self.document_url),
            document_base_url: ak::Span::new(&self.document_base_url),
            length_resolution_context: None,
            random_function_index: Some(random_function_index),
        }
    }
}

/// What a custom property's substituted value is finalized against, beside the registry and the
/// environment inherited: the element's lengths as its style computes them after line-height,
/// with its query containers' sizes, its sibling count and index, the random base values drawn
/// for it, and the color scheme its colors resolve with. Only a registered name reads them.
#[derive(Clone, Copy, Default)]
pub(crate) struct CustomPropertyFinalization<'a> {
    pub(crate) length: Option<&'a crate::css::style_compute::FfiLengthResolutionContext>,
    pub(crate) tree_counting: Option<(u64, u64)>,
    pub(crate) random_base_values: &'a [crate::css::style_compute::FfiRandomBaseValue],
    /// A PreferredColorScheme code.
    pub(crate) color_scheme: u8,
    /// Draws the random base value of a key the host published none for, from its name and whether
    /// the key's element is null.
    pub(crate) draw_random_base_value: Option<&'a DrawRandomBaseValue<'a>>,
}

/// Draws the random base value of a random caching key for the element computed: its name, and
/// whether the key's element is null.
pub(crate) type DrawRandomBaseValue<'a> = dyn Fn(&[u16], bool) -> f64 + 'a;

/// What computing registered values read of their element beyond the fonts of its lengths: the
/// viewport, its place among its siblings, the container-relative units it resolved against its
/// query containers, as a `container_relative_length_unit_mask`, the root element's font, and the
/// used color scheme.
#[derive(Clone, Copy, Default)]
pub(crate) struct RegisteredValueReads {
    pub(crate) viewport: bool,
    pub(crate) sibling_position: bool,
    pub(crate) container_unit_mask: u8,
    pub(crate) root_font: crate::css::style_compute::RootFontReads,
    pub(crate) font_metrics: bool,
    pub(crate) color_scheme: bool,
}

impl std::ops::BitOrAssign for RegisteredValueReads {
    fn bitor_assign(&mut self, other: Self) {
        self.viewport |= other.viewport;
        self.sibling_position |= other.sibling_position;
        self.container_unit_mask |= other.container_unit_mask;
        self.root_font = self.root_font.max(other.root_font);
        self.font_metrics |= other.font_metrics;
        self.color_scheme |= other.color_scheme;
    }
}

impl CustomPropertyRegistry {
    /// Parse a value with a registration's syntax, as the computation of its name parses it.
    fn parse_registered_value(
        &self,
        registration: &RegisteredCustomProperty,
        values: &[ComponentValue],
    ) -> Option<StyleValueData> {
        let random_function_index = RandomFunctionIndex::default();
        let value_context = FfiValueParsingContext {
            kind: FfiValueParsingContextKind::Property,
            value: crate::css::property_metadata::property_id::CUSTOM,
            secondary_value: 0,
            name: Default::default(),
        };
        let mut context = self.parse_context(&random_function_index);
        context.value_contexts = ak::Span::new(std::slice::from_ref(&value_context));
        parse_with_syntax_component_values(&context, values, &registration.syntax, None)
    }

    /// A value parsed with a registration's syntax, once per value the registry keeps alive.
    fn parse_registered_value_memoized(
        &self,
        registration: &RegisteredCustomProperty,
        value: &RetainedStyleValueData,
    ) -> Option<RetainedStyleValueData> {
        let key = (std::ptr::from_ref(registration) as usize, value.pointer() as usize);
        let mut parses = self.parses.lock().expect("the parse memo is not poisoned");
        if let Some((_, parsed)) = parses.get(&key) {
            return parsed.clone();
        }

        let parsed = match value.data() {
            StyleValueData::Unresolved(unresolved_payload) => {
                self.parse_registered_value(registration, &unresolved_payload.components())
            }
            _ => {
                let source = serialize_resolved_style_value_to_utf16(value.data());
                parse_substitution_values(&source).and_then(|values| self.parse_registered_value(registration, &values))
            }
        };
        let parsed = parsed.map(RetainedStyleValueData::from_owned);

        if parses.len() >= MAX_REGISTERED_VALUE_PARSES {
            parses.clear();
        }
        parses.insert(key, (value.clone(), parsed.clone()));
        parsed
    }

    fn parse_custom_property_value_for_finalization(
        &self,
        registration: Option<&RegisteredCustomProperty>,
        value: CustomPropertyValueToFinalize<'_>,
    ) -> Option<RetainedStyleValueData> {
        use CustomPropertyValueToFinalize::{StyleValue, SubstitutedComponents};

        match (registration, value) {
            (_, StyleValue(value)) if matches!(value.data(), StyleValueData::GuaranteedInvalid) => None,
            (Some(registration), StyleValue(value)) => self.parse_registered_value_memoized(registration, &value),
            (None, StyleValue(value)) => Some(value),
            (Some(registration), SubstitutedComponents { components, .. }) => {
                // Substitution results have no reusable value identity, so parse their components
                // directly rather than constructing an unresolved value and retaining it in the memo.
                collect_arbitrary_substitution_function_presence(&components, &mut Default::default()).ok()?;
                if !declaration_value_is_valid(&components) {
                    return None;
                }
                self.parse_registered_value(registration, &components)
                    .map(RetainedStyleValueData::from_owned)
            }
            (
                None,
                SubstitutedComponents {
                    components,
                    parse_context,
                    contains_attr_tainted_values,
                },
            ) => Some(parse_substituted_value(
                parse_context,
                property_id::CUSTOM,
                &components,
                contains_attr_tainted_values,
            )),
        }
    }

    /// Finalize a custom property's substituted value as its element's style computes it, as
    /// `StyleComputer::finalize_custom_property_value` does: a CSS-wide keyword resolves against
    /// the registration and the environment inherited, the guaranteed-invalid value against the
    /// registration, and a registered name's value parses with its syntax and computes against
    /// the element. Also says what the computation read of the element beyond its lengths' fonts.
    /// A registered value that does not compute against the element's inputs is an error carrying
    /// the invalid fallback it takes.
    pub(crate) fn finalize_custom_property_value(
        &self,
        inherited: Option<&CustomPropertyStore>,
        name_raw: usize,
        name: &Utf16FlyString,
        value: CustomPropertyValueToFinalize<'_>,
        finalization: &CustomPropertyFinalization<'_>,
    ) -> Result<(RetainedStyleValueData, RegisteredValueReads), RetainedStyleValueData> {
        use crate::css::style_compute::keyword;
        let registration = self.registrations.get(name);
        let initial = || {
            registration.map_or_else(
                || RetainedStyleValueData::from_owned(StyleValueData::GuaranteedInvalid),
                |registration| registration.computed_initial.clone(),
            )
        };
        let inherited_value = || {
            inherited
                .and_then(|inherited| inherited.get(name_raw))
                .map_or_else(initial, |entry| entry.value.clone())
        };

        let value = match value.to_css_wide_keyword() {
            // https://drafts.csswg.org/css-mixins/#resolve-function-styles
            // On result, all CSS-wide keywords are left unresolved.
            Some(keyword) if *name != Utf16FlyString::from_utf8("result") => {
                let resolved = match keyword {
                    keyword::INITIAL => initial(),
                    keyword::INHERIT => inherited_value(),
                    // Unset is the same as inherit for an inherited property, and every unregistered
                    // custom property inherits.
                    // FIXME: Roll revert and revert-layer back to a declaration of the custom property from
                    //        a lower cascade layer or origin when there is one, instead of treating them as
                    //        unset.
                    keyword::UNSET | keyword::REVERT | keyword::REVERT_LAYER => {
                        if registration.is_some_and(|registration| !registration.inherits) {
                            initial()
                        } else {
                            inherited_value()
                        }
                    }
                    _ => unreachable!("all CSS-wide keywords are handled above"),
                };
                CustomPropertyValueToFinalize::StyleValue(resolved)
            }
            _ => value,
        };

        // https://drafts.csswg.org/css-values-5/#invalid-substitution
        // An unregistered name, or one registered with the universal syntax, computes to the
        // guaranteed-invalid value; any other to its inherited value or its initial value, as if
        // its value had been specified as unset.
        let syntax_registration =
            registration.filter(|registration| !matches!(*registration.syntax, SyntaxNode::Universal));
        let invalid_fallback = || match syntax_registration {
            Some(registration) if registration.inherits => inherited_value(),
            Some(_) => initial(),
            None => RetainedStyleValueData::from_owned(StyleValueData::GuaranteedInvalid),
        };

        let contains_attr_tainted_values = value.contains_attr_tainted_values();

        let Some(parsed) = self.parse_custom_property_value_for_finalization(syntax_registration, value) else {
            return Ok((invalid_fallback(), RegisteredValueReads::default()));
        };

        if syntax_registration.is_none() {
            return Ok((parsed, RegisteredValueReads::default()));
        }

        // Without the element's lengths the value stays as parsed, as the host leaves one it has
        // no computed style to compute against.
        let Some(length) = finalization.length else {
            return Ok((
                attr_tainted(parsed, contains_attr_tainted_values),
                RegisteredValueReads::default(),
            ));
        };
        let dependencies = crate::css::style_compute::collect_external_value_dependencies(parsed.data());
        let random_base_values = random_base_values_for_registered_value(parsed.data(), finalization);
        let context = crate::css::absolutize::AbsolutizationContext {
            length,
            scheme: Some(finalization.color_scheme),
            resolved_viewport_relative_length: Cell::new(false),
            tree_counting: finalization.tree_counting,
            random_base_values: &random_base_values,
            document_base_url: &self.document_base_url,
            style_sheet_resource_context: None,
            current_color: None,
        };
        let computed = match crate::css::absolutize::absolutize(parsed.data(), &context) {
            Some(crate::css::absolutize::Absolutized::Changed(computed)) => computed,
            Some(crate::css::absolutize::Absolutized::Unchanged) => parsed,
            None => return Err(invalid_fallback()),
        };
        let reads = RegisteredValueReads {
            viewport: context.resolved_viewport_relative_length.get(),
            sibling_position: dependencies.uses_tree_counting_function,
            container_unit_mask: dependencies.container_relative_length_unit_mask,
            root_font: dependencies.root_font_reads,
            font_metrics: dependencies.font_metric_relative_length,
            color_scheme: dependencies.color_scheme_dependent,
        };
        Ok((attr_tainted(computed, contains_attr_tainted_values), reads))
    }
}

/// A registered value an attr() substituted into, where `tainted` says it did, which stays
/// tainted as an unresolved value carrying what it computed to.
fn attr_tainted(value: RetainedStyleValueData, tainted: bool) -> RetainedStyleValueData {
    if !tainted {
        return value;
    }
    let source = crate::css::serialize::serialize_resolved_style_value_to_utf16(value.data());
    let mut wrapped = crate::css::parser::value_parser::unresolved_value_from_string(
        &source,
        crate::css::parser::arbitrary_substitution::SubstitutionFunctionsPresence::default(),
    );
    let StyleValueData::Unresolved(unresolved_payload) = &mut wrapped else {
        unreachable!("an unresolved value is built");
    };
    let UnresolvedPayload {
        contains_attr_tainted_values,
        parsed_value,
        ..
    } = unresolved_payload.payload_mut();
    *contains_attr_tainted_values = true;
    *parsed_value = value;
    RetainedStyleValueData::from_owned(wrapped)
}

/// The random base values a registered value draws from once parsed after substitution. Its random
/// sharings are new, but the host published a base for each sharing the same declaration held when
/// parsed before substitution, which agrees with it in name and kind; one that only substitution
/// brought in is drawn now.
fn random_base_values_for_registered_value(
    value: &StyleValueData,
    finalization: &CustomPropertyFinalization<'_>,
) -> Vec<crate::css::style_compute::FfiRandomBaseValue> {
    let published = finalization.random_base_values;
    let kind = |sharing: *const StyleValueData| match unsafe { &*sharing } {
        StyleValueData::RandomValueSharing(random_value_sharing_payload) => {
            let RandomValueSharingPayload {
                is_auto,
                name,
                element_shared,
                ..
            } = &**random_value_sharing_payload;
            Some((*is_auto, name, *element_shared))
        }
        _ => None,
    };
    let mut sharings = Vec::new();
    crate::css::style_compute::collect_unfixed_random_sharings_in_value(value, &mut sharings);
    sharings
        .into_iter()
        .filter_map(|sharing| {
            let wanted = kind(sharing)?;
            let value = match published.iter().find(|base| kind(base.source.cast()) == Some(wanted)) {
                Some(base) => base.value,
                None => {
                    let draw = finalization.draw_random_base_value?;
                    let (name, element_shared) = crate::css::style_compute::random_caching_key(unsafe { &*sharing });
                    draw(name, element_shared)
                }
            };
            Some(crate::css::style_compute::FfiRandomBaseValue {
                source: sharing.cast(),
                value,
            })
        })
        .collect()
}

impl CustomPropertyStore {
    /// An element's animated custom property values laid over its own environment, `base`.
    fn animation_overlay(
        animated: impl ExactSizeIterator<Item = (usize, CustomPropertyEntry)>,
        base: Option<Arc<Self>>,
    ) -> Self {
        let (mut own_values, parent, inheritance_parent, ancestor_count) = match base.as_deref() {
            Some(base) => (
                base.own_values.to_builder(true),
                base.parent.clone(),
                base.inheritance_parent.clone(),
                base.ancestor_count,
            ),
            None => (CustomPropertyEntriesBuilder::default(), None, None, 0),
        };
        own_values.declare(animated);
        Self {
            own_values: own_values.finished(),
            ancestor_count,
            parent,
            inheritance_parent,
            animation_overlay_base: base,
        }
    }

    /// This animation overlay's animated values over `base`, the custom properties a pass resolved
    /// beneath the element's animations, which the host sampled over the ones before: the names the
    /// element inherits read what the pass resolved.
    pub(crate) fn animation_overlay_over(self: &Arc<Self>, base: Option<&Arc<Self>>) -> Arc<Self> {
        let sampled_over = self.animation_overlay_base.as_ref();
        if Self::absent_or_holding_the_same_values(sampled_over, base) {
            return self.clone();
        }
        let animated: SmallVec<[(usize, CustomPropertyEntry); 8]> = self
            .own_values
            .iter()
            .filter(|(name_raw, entry)| {
                !sampled_over.is_some_and(|sampled_over| {
                    sampled_over.value_is_identical(*name_raw, entry.value.pointer().cast())
                })
            })
            .map(|(name_raw, entry)| (name_raw, entry.clone()))
            .collect();
        Arc::new(Self::animation_overlay(animated.into_iter(), base.cloned()))
    }

    pub(crate) fn parent(&self) -> Option<&Arc<CustomPropertyStore>> {
        self.parent.as_ref()
    }

    /// The names of the custom properties the store declares itself, in declaration order.
    pub(crate) fn declared_names(&self) -> SmallVec<[usize; 8]> {
        let mut declared: SmallVec<[(std::num::NonZeroU32, usize); 8]> = self
            .own_values
            .iter()
            .filter_map(|(name_raw, entry)| Some((entry.declared?, name_raw)))
            .collect();
        declared.sort_unstable();
        declared.into_iter().map(|(_, name_raw)| name_raw).collect()
    }

    /// How many custom properties the store declares itself.
    pub(crate) fn declared_count(&self) -> usize {
        self.own_values
            .values()
            .filter(|entry| entry.declared.is_some())
            .count()
    }

    /// Whether the store declares `name_raw` itself.
    pub(crate) fn declares(&self, name_raw: usize) -> bool {
        self.own_values
            .get(&name_raw)
            .is_some_and(|entry| entry.declared.is_some())
    }

    pub(crate) fn get(&self, name_raw: usize) -> Option<&CustomPropertyEntry> {
        self.own_values
            .get(&name_raw)
            .or_else(|| self.parent.as_ref()?.get(name_raw))
    }

    /// The names of the values the store, or a store it is built over, holds. A name can come more than once.
    pub(crate) fn held_names(&self) -> impl Iterator<Item = &Utf16FlyString> {
        std::iter::successors(Some(self), |store| store.parent.as_deref())
            .flat_map(|store| store.own_values.values().map(|entry| &entry.name))
    }

    /// Whether the store, or a store it is built over, holds a value of `name`.
    pub(crate) fn holds(&self, name: &Utf16FlyString) -> bool {
        self.get_by_name_with_owner(name).is_some()
    }

    /// The identity of the value `name` resolves to in this store, zero for a name it holds no
    /// value of, or none where the value says more than its identity: a lone CSS-wide keyword,
    /// which resolves against the store holding it, or a value still waiting on a substitution.
    pub(crate) fn resolved_value_identity(&self, name: &Utf16FlyString) -> Option<usize> {
        let Some((entry, _)) = self.get_by_name_with_owner(name) else {
            return Some(0);
        };
        if let Some(StyleValueData::Unresolved(unresolved)) = entry.value.optional_data()
            && (unresolved.presence().has_any() || unresolved.facts().is_lone_css_wide_keyword())
        {
            return None;
        }
        Some(entry.value.pointer() as usize)
    }

    fn get_by_name_with_owner(&self, name: &Utf16FlyString) -> Option<(&CustomPropertyEntry, &CustomPropertyStore)> {
        self.own_values
            .get(&name.raw_identity())
            .map(|entry| (entry, self))
            .or_else(|| self.parent.as_ref()?.get_by_name_with_owner(name))
    }

    fn get_own_by_name(&self, name: &Utf16FlyString) -> Option<&CustomPropertyEntry> {
        self.own_values.get(&name.raw_identity())
    }

    /// What a child inherits of this store: the store without the entries a registration keeps
    /// from inheriting, over the same chain; `None` when every entry inherits.
    pub(crate) fn inheritable(&self, registry: &CustomPropertyRegistry) -> Option<Arc<CustomPropertyStore>> {
        if self.hands_down_every_value(registry) {
            return None;
        }
        let inherits = |entry: &CustomPropertyEntry| registry.name_inherits(&entry.name);
        let own_values: CustomPropertyEntries = self
            .own_values
            .iter()
            .filter(|(_, entry)| inherits(entry))
            .map(|(name_raw, entry)| (name_raw, entry.clone()))
            .collect();
        Some(Arc::new(Self {
            own_values,
            parent: self.parent.clone(),
            inheritance_parent: self.inheritance_parent.clone(),
            animation_overlay_base: None,
            ancestor_count: self.ancestor_count,
        }))
    }

    /// Whether `other` declares the custom properties the store does, in the same order. Both hold
    /// their entries by name, so this takes no sorting, which a store of a design system's thousands
    /// of custom properties would pay for each element compared against it.
    fn declares_the_same_names_as(&self, other: &Self) -> bool {
        fn declared(store: &CustomPropertyStore) -> impl Iterator<Item = (usize, std::num::NonZeroU32)> + '_ {
            store
                .own_values
                .iter()
                .filter_map(|(name_raw, entry)| Some((name_raw, entry.declared?)))
        }
        declared(self).eq(declared(other))
    }

    /// Whether `first` and `second` hold the same values, as [`Self::holds_same_values_as`] tells.
    /// Every element below a store asks this of the same two stores, each answer taking a walk of
    /// their values, so a thread keeps the answers it found.
    pub(crate) fn hold_the_same_values(first: &Arc<Self>, second: &Arc<Self>) -> bool {
        if Arc::ptr_eq(first, second) {
            return true;
        }
        // NB: The answer is the same whichever of the two is asked.
        let (first, second) = if Arc::as_ptr(first) < Arc::as_ptr(second) {
            (first, second)
        } else {
            (second, first)
        };
        let identities = (Arc::as_ptr(first) as u64) ^ (Arc::as_ptr(second) as u64).rotate_left(32);
        let slot =
            (identities.wrapping_mul(0x9e37_79b9_7f4a_7c15) >> (u64::BITS - COMPARED_STORE_SLOTS.ilog2())) as usize;
        let known = COMPARED_STORES.with_borrow(|compared| {
            compared[slot]
                .as_ref()
                .filter(|compared| {
                    compared.first.as_ptr() == Arc::as_ptr(first) && compared.second.as_ptr() == Arc::as_ptr(second)
                })
                .map(|compared| compared.same)
        });
        if let Some(same) = known {
            return same;
        }
        let same = first.holds_same_values_as(second);
        COMPARED_STORES.with_borrow_mut(|compared| {
            compared[slot] = Some(ComparedStores {
                first: Arc::downgrade(first),
                second: Arc::downgrade(second),
                same,
            });
        });
        same
    }

    /// Whether `first` and `second` are both absent, or hold the same values.
    pub(crate) fn absent_or_holding_the_same_values(first: Option<&Arc<Self>>, second: Option<&Arc<Self>>) -> bool {
        match (first, second) {
            (None, None) => true,
            (Some(first), Some(second)) => Self::hold_the_same_values(first, second),
            _ => false,
        }
    }

    /// Whether the store holds the same values as `other`, built over the same store and chained to
    /// the same parent.
    fn holds_same_values_as(&self, other: &Self) -> bool {
        let same_inheritance = Self::absent_or_holding_the_same_values(
            self.inheritance_parent.as_ref(),
            other.inheritance_parent.as_ref(),
        );
        if !same_inheritance || !self.declares_the_same_names_as(other) {
            return false;
        }
        // NB: What a store took in from the chain above it, and which stores stand there, depends on how it was built:
        //     stores hold the same where every name reads the same, down to a store their chains share.
        fn chain(store: &CustomPropertyStore) -> impl Iterator<Item = &CustomPropertyStore> {
            std::iter::successors(Some(store), |store| store.parent.as_deref())
        }
        let reads_alike_in_both = |from: &Self, shared_with: &Self| {
            chain(from)
                .take_while(|store| !chain(shared_with).any(|shared| std::ptr::eq(*store, shared)))
                .flat_map(|store| store.own_values.iter())
                .all(|(name_raw, _)| match (self.get(name_raw), other.get(name_raw)) {
                    (Some(first), Some(second)) => {
                        first.important == second.important
                            && crate::css::style_value::retained_style_values_equal(&first.value, &second.value)
                    }
                    _ => false,
                })
        };
        reads_alike_in_both(self, other) && reads_alike_in_both(other, self)
    }

    /// Whether a child inherits every entry of this store.
    pub(crate) fn hands_down_every_value(&self, registry: &CustomPropertyRegistry) -> bool {
        self.own_values
            .values()
            .all(|entry| registry.name_inherits(&entry.name))
    }

    pub(crate) fn value_matches(&self, name_raw: usize, value: &StyleValueData) -> bool {
        self.get(name_raw).is_some_and(|entry| entry.value.data() == value)
    }

    pub(crate) fn value_is_identical(&self, name_raw: usize, value: *const c_void) -> bool {
        self.get(name_raw)
            .is_some_and(|entry| entry.value.pointer().cast() == value)
    }

    fn child(
        mut parent: Option<Arc<CustomPropertyStore>>,
        entries: Vec<(usize, CustomPropertyEntry)>,
    ) -> Arc<CustomPropertyStore> {
        let mut own_values = CustomPropertyEntriesBuilder(Vec::with_capacity(entries.len()));
        own_values.declare(entries);

        let inheritance_parent = parent.clone();
        let ancestor_count = if let Some(current_parent) = parent.clone() {
            if current_parent.ancestor_count >= MAX_ANCESTOR_COUNT - 1 {
                let mut ancestor = Some(current_parent.as_ref());
                while let Some(current) = ancestor {
                    for entry in current.own_values.values() {
                        own_values.insert_if_vacant(entry);
                    }
                    ancestor = current.parent.as_deref();
                }
                parent = None;
                0
            } else if current_parent.own_values.len() <= ABSORB_THRESHOLD {
                for entry in current_parent.own_values.values() {
                    own_values.insert_if_vacant(entry);
                }
                parent = current_parent.parent.clone();
                parent.as_ref().map_or(0, |parent| parent.ancestor_count + 1)
            } else {
                current_parent.ancestor_count + 1
            }
        } else {
            0
        };

        Arc::new(CustomPropertyStore {
            own_values: own_values.finished(),
            inheritance_parent,
            parent,
            animation_overlay_base: None,
            ancestor_count,
        })
    }

    pub(crate) fn resolved_child(
        &self,
        parent: Option<Arc<CustomPropertyStore>>,
        values: Vec<(usize, RetainedStyleValueData)>,
    ) -> Arc<CustomPropertyStore> {
        let entries = values
            .into_iter()
            .map(|(name_raw, value)| {
                let source = self
                    .own_values
                    .get(&name_raw)
                    .expect("resolved custom property must be an own value");
                (
                    name_raw,
                    CustomPropertyEntry {
                        name: source.name.clone(),
                        value,
                        important: source.important,
                        declared: None,
                    },
                )
            })
            .collect();
        Self::child(parent, entries)
    }

    /// # Safety
    /// Every name and value pointer must remain live for this call.
    pub(crate) unsafe fn cascaded_child(
        parent: Option<Arc<CustomPropertyStore>>,
        values: Vec<(usize, bool, *const c_void)>,
    ) -> Arc<CustomPropertyStore> {
        let entries = values
            .into_iter()
            .map(|(name_raw, important, value)| {
                (
                    name_raw,
                    CustomPropertyEntry {
                        name: unsafe { Utf16FlyString::from_raw_borrowed(name_raw) },
                        value: unsafe {
                            RetainedStyleValueData::from_retained_pointer(
                                crate::css::style_value::retain_style_value(value.cast()).cast(),
                            )
                        },
                        important,
                        declared: None,
                    },
                )
            })
            .collect();
        Self::child(parent, entries)
    }
}

const MAX_SUBSTITUTED_TOKEN_COUNT: usize = 16384;
const MAX_SUBSTITUTION_RECURSION_DEPTH: u32 = 64;

pub(crate) enum NativeVarResolution {
    Resolved {
        /// The substituted components.
        components: ComponentValueList,
        contains_attr_tainted_values: bool,
    },
    Invalid,
    NotHandled,
}

fn parse_substitution_values<'a>(input: impl Into<TokenizerInput<'a>>) -> Option<ComponentValueList> {
    consume_a_counted_list_of_component_values(tokenize_for_parser(input)).ok()
}

enum TokenResolution {
    Resolved(ComponentValueList),
    Invalid,
    Cyclic,
    NotHandled,
}

impl TokenResolution {
    fn from_source<'a>(source: impl Into<crate::css::css_tokenizer::TokenizerInput<'a>>) -> Self {
        parse_substitution_values(source).map_or(Self::Invalid, Self::Resolved)
    }
}

// https://drafts.csswg.org/css-values-5/#substitution-context
#[derive(PartialEq, Eq)]
enum SubstitutionContextDependency {
    Property(Utf16FlyString, Option<CustomFunctionIdentity>),
    // FIXME: Attribute substitution context equality should compare names ASCII-case-insensitively, as attribute lookup
    // does.
    Attribute(Vec<u16>),
    Function(CustomFunctionIdentity),
}

struct SubstitutionContext {
    dependency: SubstitutionContextDependency,
    is_cyclic: Cell<bool>,
}

impl SubstitutionContext {
    fn new(dependency: SubstitutionContextDependency) -> Rc<Self> {
        Rc::new(Self {
            dependency,
            is_cyclic: Cell::new(false),
        })
    }
}

#[derive(Clone, Default)]
struct GuardedSubstitutionContexts {
    contexts: Rc<RefCell<Vec<Rc<SubstitutionContext>>>>,
}

impl GuardedSubstitutionContexts {
    // https://drafts.csswg.org/css-values-5/#guarded
    fn guard(&self, context: &Rc<SubstitutionContext>) -> Option<ScopeGuard<impl FnMut() + use<>>> {
        if self.mark_cycle_if_guarded(&context.dependency) {
            context.is_cyclic.set(true);
            return None;
        }

        self.contexts.borrow_mut().push(Rc::clone(context));

        let contexts = Rc::clone(&self.contexts);
        let context = Rc::clone(context);
        Some(ScopeGuard::new(move || {
            let popped = contexts.borrow_mut().pop().expect("guarded substitution context");

            assert!(Rc::ptr_eq(&popped, &context));
        }))
    }

    fn mark_cycle_if_guarded(&self, dependency: &SubstitutionContextDependency) -> bool {
        let contexts = self.contexts.borrow();
        let Some(cycle_start) = contexts.iter().position(|context| &context.dependency == dependency) else {
            return false;
        };
        for context in &contexts[cycle_start..] {
            context.is_cyclic.set(true);
        }
        true
    }
}

#[derive(Default)]
struct ASFResolutionContext<'a> {
    guarded_contexts: GuardedSubstitutionContexts,
    attributes: Option<&'a HashMap<Vec<u16>, Vec<u16>>>,
    inheritance_store: Option<&'a CustomPropertyStore>,
    attribute_names_are_ascii_case_insensitive: bool,
    contains_attr_tainted_values: bool,
    /// Whether the substitution under way is inside a function that takes a URL.
    in_url_function: bool,
    /// Whether an attr-tainted value was substituted into a URL, or substituted one.
    uses_attr_tainted_url: bool,
    custom_functions: Option<&'a CustomFunctionRegistry<'a>>,
    parse_context: Option<&'a ParseContext<'a>>,
    media_environment: Option<&'a FfiMediaEnvironment<'a>>,
    /// What lengths in a style query resolve against: the element's last computed style.
    style_query_length_resolution_context: Option<&'a crate::css::style_compute::FfiLengthResolutionContext>,
    /// What colors in a style query resolve against, where the queried element's are known.
    style_query_color_resolution_input: Option<crate::css::color_resolution::ColorResolutionInput<'a>>,
    /// The queried element's sibling count and index, which tree-counting functions in a style
    /// query resolve against, where they are known.
    style_query_tree_counting: Option<(u64, u64)>,
    /// Where whether a style query reads the sibling position is noted.
    style_query_dependencies: Option<&'a mut Option<Box<StyleQueryDependencies>>>,
    custom_property_finalization: CustomPropertyFinalization<'a>,
    registered_value_reads: Option<&'a mut RegisteredValueReads>,
    element_store: Option<&'a CustomPropertyStore>,
    element_values: Option<&'a mut FastMap<usize, RetainedStyleValueData>>,
    substitution_limit_hits: u64,
    function_local_scopes: Vec<FunctionLocalScope<'a>>,
    token_cache: Option<&'a mut CustomPropertyTokenCache>,
    /// Where the custom properties of the element the substitution under way looks up are noted, if anywhere.
    element_property_reads: Option<&'a mut HashSet<Utf16FlyString>>,
}

impl ASFResolutionContext<'_> {
    fn note_style_query_sibling_position_dependency(&mut self) {
        if let Some(reads) = self.registered_value_reads.as_deref_mut() {
            reads.sibling_position = true;
        }
        if let Some(dependencies) = self.style_query_dependencies.as_deref_mut() {
            dependencies.get_or_insert_default().sibling_position = true;
        }
    }
}

/// What the style queries of one resolution read beside the custom properties: whether they
/// read the element's sibling position.
#[derive(Clone, Default)]
pub(crate) struct StyleQueryDependencies {
    pub(crate) sibling_position: bool,
}

/// The tokens of each value by its address, with the value where nothing else keeps it for as long as the cache.
type CustomPropertyTokenCache =
    HashMap<*const StyleValueData, (Option<RetainedStyleValueData>, Arc<ComponentValueList>)>;

pub(crate) struct VarResolutionEnvironment<'a> {
    attributes: HashMap<Vec<u16>, Vec<u16>>,
    custom_functions: &'a CustomFunctionRegistry<'a>,
    token_cache: CustomPropertyTokenCache,
}

fn source_ends_with_comment(source: crate::css::css_tokenizer::TokenizerInput<'_>) -> bool {
    match source {
        crate::css::css_tokenizer::TokenizerInput::Latin1(source) => source
            .iter()
            .rposition(|unit| !unit.is_ascii_whitespace())
            .is_some_and(|end| end > 0 && source[end - 1..=end] == *b"*/"),
        crate::css::css_tokenizer::TokenizerInput::Utf16(source) => source
            .iter()
            .rposition(|unit| !matches!(*unit, 0x09 | 0x0a | 0x0c | 0x0d | 0x20))
            .is_some_and(|end| end > 0 && source[end - 1..=end] == [u16::from(b'*'), u16::from(b'/')]),
    }
}

pub(crate) fn single_css_wide_keyword(tokens: &[ComponentValue]) -> Option<&[u16]> {
    let [value] = trim_whitespace(tokens) else {
        return None;
    };
    let keyword = value.ident()?;
    (keyword.eq_ignore_ascii_case("inherit")
        || keyword.eq_ignore_ascii_case("initial")
        || keyword.eq_ignore_ascii_case("unset")
        || keyword.eq_ignore_ascii_case("revert")
        || keyword.eq_ignore_ascii_case("revert-layer"))
    .then_some(keyword)
}

/// The tokens of an unresolved custom property value as one read has them.
struct CustomPropertyTokens {
    components: ComponentValueList,
    /// Whether the text ends with a comment, after which trailing whitespace is discarded.
    trims_trailing_whitespace: bool,
}

impl CustomPropertyTokens {
    fn slice(&self) -> ComponentValueSlice<'_> {
        let values = ComponentValueSlice::from(&self.components);
        if self.trims_trailing_whitespace {
            values.trim_end_whitespace()
        } else {
            values
        }
    }

    fn into_list(self) -> ComponentValueList {
        if self.trims_trailing_whitespace {
            self.slice().to_owned()
        } else {
            self.components
        }
    }
}

fn tokens_for_unresolved_custom_property_value(unresolved_payload: &UnresolvedPayload) -> CustomPropertyTokens {
    CustomPropertyTokens {
        components: unresolved_payload.components(),
        // Discard trailing comments
        trims_trailing_whitespace: source_ends_with_comment(unresolved_payload.source_text()),
    }
}

/// The tokens of a value a resolution reads, which it tokenizes once however often it reads them.
/// The value must outlive the resolution's token cache.
fn cached_value_tokens(data: &StyleValueData, context: &mut ASFResolutionContext<'_>) -> Arc<ComponentValueList> {
    cached_tokens(data, None, context)
}

/// The tokens of `value`, which the resolution's token cache holds as long as it holds them.
fn cached_held_value_tokens(
    value: &RetainedStyleValueData,
    context: &mut ASFResolutionContext<'_>,
) -> Arc<ComponentValueList> {
    cached_tokens(value.data(), Some(value), context)
}

fn cached_tokens(
    data: &StyleValueData,
    held: Option<&RetainedStyleValueData>,
    context: &mut ASFResolutionContext<'_>,
) -> Arc<ComponentValueList> {
    let key = std::ptr::from_ref(data);
    if let Some((_, tokens)) = context.token_cache.as_deref().and_then(|cache| cache.get(&key)) {
        return Arc::clone(tokens);
    }
    let tokens: Arc<ComponentValueList> = match data {
        StyleValueData::Unresolved(unresolved_payload) => {
            tokens_for_unresolved_custom_property_value(unresolved_payload).into_list()
        }
        _ => parse_substitution_values(&crate::css::serialize::serialize_style_value_to_utf16(data))
            .expect("serialized style values tokenize correctly"),
    }
    .into();
    if let Some(cache) = context.token_cache.as_deref_mut() {
        cache.insert(key, (held.cloned(), Arc::clone(&tokens)));
    }
    tokens
}

/// Tokens, substitution-function presence, and attribute reads for a function value.
fn tokens_for_function_value(data: &StyleValueData) -> (ComponentValueList, bool, bool) {
    if let StyleValueData::Unresolved(unresolved_payload) = data {
        let presence_attr = &unresolved_payload.presence().attr;
        let presence_dashed_function = &unresolved_payload.presence().dashed_function;
        let presence_env = &unresolved_payload.presence().env;
        let presence_if = &unresolved_payload.presence().if_;
        let presence_inherit = &unresolved_payload.presence().inherit;
        let presence_var = &unresolved_payload.presence().var;
        return (
            ComponentValueSlice::from(&unresolved_payload.components())
                .trim_whitespace()
                .to_owned(),
            *presence_var
                || *presence_attr
                || *presence_dashed_function
                || *presence_env
                || *presence_if
                || *presence_inherit,
            *presence_attr,
        );
    }
    let source = crate::css::serialize::serialize_style_value_to_utf16(data);
    (
        parse_substitution_values(&source).expect("serialized style values form valid component values"),
        false,
        false,
    )
}

pub(crate) fn prepare_function_parameter_defaults(
    signature: &FunctionSignature,
) -> (Vec<Option<ComponentValueList>>, bool) {
    let mut parameter_defaults = Vec::with_capacity(signature.parameters.len());
    let mut reads_attributes = false;

    for parameter in &signature.parameters {
        parameter_defaults.push(parameter.default_value.as_ref().map(|data| {
            let (tokens, _, value_reads_attributes) = tokens_for_function_value(data);
            reads_attributes |= value_reads_attributes;
            tokens
        }));
    }

    (parameter_defaults, reads_attributes)
}

pub(crate) fn prepare_function_declarations(
    descriptor_block: &crate::css::descriptor_block::DescriptorBlockData,
) -> (Vec<SubstitutionFunctionDeclaration>, bool) {
    let mut declarations = Vec::with_capacity(descriptor_block.descriptors.len());
    let mut reads_attributes = false;

    for declaration in &descriptor_block.descriptors {
        let (tokens, includes_substitution, value_reads_attributes) = tokens_for_function_value(&declaration.value);

        reads_attributes |= value_reads_attributes;

        declarations.push(SubstitutionFunctionDeclaration {
            name: Utf16FlyString::from_utf16(declaration.name.units()),
            tokens,
            includes_substitution,
        });
    }

    (declarations, reads_attributes)
}

/// Builds the element-wide immutable substitution inputs once for all declarations in a cascade.
pub(crate) fn prepare_var_resolution_environment<'a>(
    attributes: &[SubstitutionAttribute],
    custom_functions: &'a CustomFunctionRegistry<'a>,
) -> VarResolutionEnvironment<'a> {
    let attributes = attributes
        .iter()
        .map(|attribute| (attribute.name.to_vec(), attribute.value.to_utf16().into_owned()))
        .collect();
    VarResolutionEnvironment {
        attributes,
        custom_functions,
        token_cache: HashMap::new(),
    }
}

/// Finalizes an element-level custom property and accumulates its dependencies.
pub(crate) fn finalize_element_custom_property(
    registry: &CustomPropertyRegistry,
    inherited: Option<&CustomPropertyStore>,
    finalization: &CustomPropertyFinalization,
    reads: &mut RegisteredValueReads,
    name_raw: usize,
    name: &Utf16FlyString,
    value: CustomPropertyValueToFinalize<'_>,
) -> RetainedStyleValueData {
    let (value, value_reads) = registry
        .finalize_custom_property_value(inherited, name_raw, name, value, finalization)
        .unwrap_or_else(|fallback| (fallback, Default::default()));
    *reads |= value_reads;
    value
}

#[cfg(test)]
thread_local! {
    static CUSTOM_PROPERTY_COMPUTATIONS: std::cell::Cell<u64> = const { std::cell::Cell::new(0) };
}

fn compute_element_custom_property(
    store: &CustomPropertyStore,
    registry: &CustomPropertyRegistry,
    name: &Utf16FlyString,
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> Result<RetainedStyleValueData, ()> {
    let name_raw = name.raw_identity();

    if let Some(cached) = context.element_values.as_deref().ok_or(())?.get(&name_raw) {
        return Ok(cached.clone());
    }

    let limit_hits_before_computation = context.substitution_limit_hits;

    #[cfg(test)]
    CUSTOM_PROPERTY_COMPUTATIONS.with(|count| count.set(count.get() + 1));

    let entry = &store.own_values[&name_raw];
    let substituted = match entry.value.data() {
        StyleValueData::Unresolved(unresolved_payload) if unresolved_payload.presence().has_any() => {
            Some(unresolved_payload)
        }
        _ => None,
    };

    let value = if let Some(unresolved_payload) = substituted {
        let tokens = tokens_for_unresolved_custom_property_value(unresolved_payload);
        let tainted = unresolved_payload.contains_attr_tainted_values;
        let guarded_contexts = GuardedSubstitutionContexts {
            contexts: Rc::new(RefCell::new(
                context
                    .guarded_contexts
                    .contexts
                    .borrow()
                    .iter()
                    .take_while(|context| !matches!(&context.dependency, SubstitutionContextDependency::Function(_)))
                    .cloned()
                    .collect(),
            )),
        };

        let mut property_context = ASFResolutionContext {
            guarded_contexts,
            contains_attr_tainted_values: tainted,
            in_url_function: false,
            uses_attr_tainted_url: false,
            style_query_dependencies: context.style_query_dependencies.as_deref_mut(),
            registered_value_reads: context.registered_value_reads.as_deref_mut(),
            element_values: context.element_values.as_deref_mut(),
            function_local_scopes: Vec::new(),
            token_cache: context.token_cache.as_deref_mut(),
            element_property_reads: context.element_property_reads.as_deref_mut(),
            ..*context
        };

        let result = substitute_arbitrary_substitution_functions(
            Some(store),
            Some(registry),
            tokens.slice(),
            &mut property_context,
            recursion_depth,
            Some(SubstitutionContextDependency::Property(name.clone(), None)),
        );

        context.substitution_limit_hits = property_context.substitution_limit_hits;

        match result {
            TokenResolution::Resolved(components) => CustomPropertyValueToFinalize::SubstitutedComponents {
                components,
                parse_context: context.parse_context.ok_or(())?,
                contains_attr_tainted_values: property_context.contains_attr_tainted_values,
            },
            TokenResolution::Invalid | TokenResolution::Cyclic => CustomPropertyValueToFinalize::StyleValue(
                RetainedStyleValueData::from_owned(StyleValueData::GuaranteedInvalid),
            ),
            TokenResolution::NotHandled => return Err(()),
        }
    } else {
        CustomPropertyValueToFinalize::StyleValue(entry.value.clone())
    };

    let finalized_value = finalize_element_custom_property(
        registry,
        context.inheritance_store,
        &context.custom_property_finalization,
        context.registered_value_reads.as_deref_mut().ok_or(())?,
        name_raw,
        name,
        value,
    );

    if context.substitution_limit_hits == limit_hits_before_computation {
        context
            .element_values
            .as_deref_mut()
            .ok_or(())?
            .insert(name_raw, finalized_value.clone());
    }

    Ok(finalized_value)
}

/// Computes the element's declared custom properties, reusing completed values across declarations.
pub(crate) fn resolve_element_custom_properties(
    inputs: &VarResolutionInputs,
    environment: &mut VarResolutionEnvironment<'_>,
    style_query_dependencies: Option<&mut Option<Box<StyleQueryDependencies>>>,
    finalization: CustomPropertyFinalization,
) -> (Vec<RetainedStyleValueData>, RegisteredValueReads) {
    let store = inputs.store.expect("declared custom properties have an element store");

    let mut reads = RegisteredValueReads::default();

    let mut context = ASFResolutionContext {
        attributes: Some(&environment.attributes),
        inheritance_store: inputs.inheritance_store,
        attribute_names_are_ascii_case_insensitive: inputs.attribute_names_are_ascii_case_insensitive,
        custom_functions: Some(environment.custom_functions),
        parse_context: inputs.parse_context,
        media_environment: inputs.media_environment,
        style_query_length_resolution_context: inputs.style_query_inputs.map(|inputs| &inputs.length),
        style_query_color_resolution_input: inputs.style_query_inputs.map(|inputs| inputs.color_input()),
        style_query_tree_counting: inputs.style_query_inputs.and_then(|inputs| inputs.tree_counting),
        style_query_dependencies,
        custom_property_finalization: finalization,
        registered_value_reads: Some(&mut reads),
        element_store: Some(store),
        element_values: Some(&mut FastMap::default()),
        token_cache: Some(&mut environment.token_cache),
        ..Default::default()
    };

    let registry = inputs
        .registry
        .unwrap_or_else(|| CustomPropertyRegistry::shared_empty());

    let values = store
        .declared_names()
        .into_iter()
        .map(|name_raw| {
            let name = &store.own_values[&name_raw].name;
            match compute_element_custom_property(store, registry, name, &mut context, 0) {
                Ok(value) => value,
                Err(()) => finalize_element_custom_property(
                    registry,
                    inputs.inheritance_store,
                    &finalization,
                    context.registered_value_reads.as_deref_mut().unwrap(),
                    name_raw,
                    name,
                    CustomPropertyValueToFinalize::StyleValue(RetainedStyleValueData::from_owned(
                        StyleValueData::GuaranteedInvalid,
                    )),
                ),
            }
        })
        .collect();

    (values, reads)
}

fn computed_property_tokens(value: &RetainedStyleValueData, context: &mut ASFResolutionContext) -> TokenResolution {
    match value.data() {
        StyleValueData::GuaranteedInvalid => TokenResolution::Invalid,
        // NB: A computed value can be one the element computed just now, which need not outlive the
        //     resolution, so its tokens are not cached by its address, or the cache holds it.
        StyleValueData::Unresolved(unresolved_payload) => {
            context.contains_attr_tainted_values |= unresolved_payload.contains_attr_tainted_values;
            TokenResolution::Resolved(tokens_for_unresolved_custom_property_value(unresolved_payload).into_list())
        }
        _ => TokenResolution::Resolved(cached_held_value_tokens(value, context).as_ref().clone()),
    }
}

fn registration_accepts_tokens(
    registry: &CustomPropertyRegistry,
    registration: &RegisteredCustomProperty,
    tokens: &[ComponentValue],
) -> bool {
    matches!(*registration.syntax, SyntaxNode::Universal)
        || registry.parse_registered_value(registration, tokens).is_some()
}

fn registered_property_fallback(
    inheritance_store: Option<&CustomPropertyStore>,
    registry: &CustomPropertyRegistry,
    registration: &RegisteredCustomProperty,
    name: &Utf16FlyString,
    recursion_depth: u32,
) -> TokenResolution {
    if registration.inherits
        && let Some(parent) = inheritance_store
    {
        return resolve_custom_property_with_lookup(
            Some(parent),
            Some(registry),
            name,
            &mut ASFResolutionContext::default(),
            recursion_depth + 1,
            CustomPropertyLookup::ExplicitInheritance,
        );
    }
    registration
        .initial_source
        .as_ref()
        .map(RetainedString::units)
        .map_or(TokenResolution::Invalid, TokenResolution::from_source)
}

fn resolve_css_wide_keyword(
    owner: &CustomPropertyStore,
    registry: Option<&CustomPropertyRegistry>,
    name: &Utf16FlyString,
    keyword: &[u16],
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
    lookup: CustomPropertyLookup,
) -> TokenResolution {
    let registration = registry.and_then(|registry| registry.registrations.get(name));
    // Mirror StyleComputer::resolve_css_wide_keyword_for_custom_property().
    if keyword.eq_ignore_ascii_case("initial") {
        return registration
            .and_then(|registration| registration.initial_source.as_ref())
            .map(RetainedString::units)
            .map_or(TokenResolution::Invalid, TokenResolution::from_source);
    }
    // FIXME: Roll `revert` and `revert-layer` back to a declaration of the custom property from a lower cascade layer
    //        or origin when there is one, instead of treating them as `unset`.
    let is_unset = keyword.eq_ignore_ascii_case("unset")
        || keyword.eq_ignore_ascii_case("revert")
        || keyword.eq_ignore_ascii_case("revert-layer");
    if keyword.eq_ignore_ascii_case("inherit")
        || is_unset && registration.is_none_or(|registration| registration.inherits)
    {
        let inheritance_store = match lookup {
            CustomPropertyLookup::Normal => context.inheritance_store,
            CustomPropertyLookup::ExplicitInheritance => owner.inheritance_parent.as_deref(),
        };
        return resolve_custom_property_with_lookup(
            inheritance_store,
            registry,
            name,
            context,
            recursion_depth + 1,
            CustomPropertyLookup::ExplicitInheritance,
        );
    }
    registration
        .and_then(|registration| registration.initial_source.as_ref())
        .map(RetainedString::units)
        .map_or(TokenResolution::Invalid, TokenResolution::from_source)
}

fn normalize_function_tokens(
    registry: Option<&CustomPropertyRegistry>,
    syntax: &SyntaxNode,
    tokens: &ComponentValueList,
) -> TokenResolution {
    if matches!(syntax, SyntaxNode::Universal) {
        return TokenResolution::Resolved(tokens.clone());
    }
    let Some(registry) = registry else {
        return TokenResolution::NotHandled;
    };
    let random_function_index = RandomFunctionIndex::default();
    let context = registry.parse_context(&random_function_index);
    let Some(parsed) = parse_with_syntax_component_values(&context, tokens, syntax, None) else {
        return TokenResolution::Invalid;
    };
    let computed = if matches!(parsed, StyleValueData::Calculated { .. }) {
        crate::css::calc::collapse_calculated_without_context(&parsed)
    } else {
        None
    };
    let source = crate::css::serialize::serialize_resolved_style_value_to_utf16(computed.as_ref().unwrap_or(&parsed));
    TokenResolution::from_source(&source)
}

#[allow(clippy::too_many_arguments)]
fn resolve_function_local_property(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    name: &Utf16FlyString,
    function_identity: CustomFunctionIdentity,
    value: Option<FunctionLocalValue<'_>>,
    registration: FunctionLocalRegistration<'_>,
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    let Some(value) = value else {
        return registration
            .initial_tokens
            .map_or(TokenResolution::Invalid, TokenResolution::Resolved);
    };
    let mut result = if value.includes_substitution {
        substitute_arbitrary_substitution_functions(
            store,
            registry,
            value.tokens.as_slice(),
            context,
            recursion_depth + 1,
            Some(SubstitutionContextDependency::Property(
                name.clone(),
                Some(function_identity),
            )),
        )
    } else {
        TokenResolution::Resolved(value.tokens.into_owned())
    };
    // https://drafts.csswg.org/css-mixins/#resolve-function-styles
    // On result, all CSS-wide keywords are left unresolved.
    if !registration.is_result
        && let TokenResolution::Resolved(tokens) = &result
        && let Some(keyword) = single_css_wide_keyword(tokens)
    {
        if keyword.eq_ignore_ascii_case("initial") {
            return registration
                .initial_tokens
                .map_or(TokenResolution::Invalid, TokenResolution::Resolved);
        }
        if keyword.eq_ignore_ascii_case("inherit") {
            let local_scope = context
                .function_local_scopes
                .pop()
                .expect("function-local property scope");
            result = resolve_custom_property(store, registry, name, context, recursion_depth + 1);
            context.function_local_scopes.push(local_scope);
        } else {
            return TokenResolution::Invalid;
        }
    }
    match result {
        TokenResolution::Resolved(tokens) => normalize_function_tokens(registry, registration.syntax, &tokens),
        other => other,
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum CustomPropertyLookup {
    Normal,
    ExplicitInheritance,
}

fn resolve_custom_property(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    name: &Utf16FlyString,
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    resolve_custom_property_with_lookup(
        store,
        registry,
        name,
        context,
        recursion_depth,
        CustomPropertyLookup::Normal,
    )
}

fn resolve_custom_property_with_lookup(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    name: &Utf16FlyString,
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
    lookup: CustomPropertyLookup,
) -> TokenResolution {
    let local_scope_index = context
        .function_local_scopes
        .iter()
        .rposition(|local_scope| local_scope.values.contains_key(name) || local_scope.registrations.contains_key(name));
    if let Some(local_scope_index) = local_scope_index {
        let function_scope = &context.function_local_scopes[local_scope_index];

        if let Some(cached_value) = function_scope.resolved_value_cache.borrow().get(name) {
            return TokenResolution::Resolved(cached_value.clone());
        }

        let function_identity = function_scope.function_identity;
        let value = function_scope.values.get(name).cloned();
        let registration = function_scope.registrations.get(name).cloned();
        let child_scopes = context.function_local_scopes.split_off(local_scope_index + 1);
        let result = resolve_function_local_property(
            store,
            registry,
            name,
            function_identity,
            value,
            registration.unwrap_or(FunctionLocalRegistration {
                syntax: &SyntaxNode::Universal,
                initial_tokens: None,
                is_result: false,
            }),
            context,
            recursion_depth,
        );
        context.function_local_scopes.extend(child_scopes);

        if let TokenResolution::Resolved(tokens) = &result {
            context.function_local_scopes[local_scope_index]
                .resolved_value_cache
                .borrow_mut()
                .insert(name.clone(), tokens.clone());
        }

        return result;
    }

    if lookup == CustomPropertyLookup::Normal
        && let Some(reads) = context.element_property_reads.as_deref_mut()
    {
        reads.insert(name.clone());
    }

    // Pop the function local scopes while we resolve the element level custom property.
    let mut local_scopes = std::mem::take(&mut context.function_local_scopes);
    let context_ref = RefCell::new(context);
    let _guard = ScopeGuard::new(|| context_ref.borrow_mut().function_local_scopes = std::mem::take(&mut local_scopes));
    let context = &mut **context_ref.borrow_mut();

    let substitution_context = SubstitutionContextDependency::Property(name.clone(), None);
    // AD-HOC: The root custom property's unresolved value is passed directly to `resolve_vars()` rather than read
    //         through this lookup. If it contains a `var()` that refers back to itself, this function is entered while
    //         the caller's root context is already guarded, so mark that context cyclic before consulting stored or
    //         cached values.
    if lookup == CustomPropertyLookup::Normal && context.guarded_contexts.mark_cycle_if_guarded(&substitution_context) {
        return TokenResolution::Cyclic;
    }
    let registration = registry.and_then(|registry| registry.registrations.get(name));
    let entry_and_owner = store.and_then(|store| {
        if lookup == CustomPropertyLookup::ExplicitInheritance {
            return store.get_by_name_with_owner(name);
        }
        match registration {
            Some(registration) if !registration.inherits => store.get_own_by_name(name).map(|entry| (entry, store)),
            _ => store.get_by_name_with_owner(name),
        }
    });
    let Some((entry, owner)) = entry_and_owner else {
        return registration
            .and_then(|registration| registration.initial_source.as_ref())
            .map(RetainedString::units)
            .map_or(TokenResolution::Invalid, TokenResolution::from_source);
    };
    // Declared names are unique keys from own_values; equal counts mean the owner lookup proves membership.
    if lookup == CustomPropertyLookup::Normal
        && context.element_store.is_some_and(|store| std::ptr::eq(store, owner))
        && store.is_some_and(|store| std::ptr::eq(store, owner))
        && owner.declares(name.raw_identity())
    {
        let computed_value = compute_element_custom_property(
            owner,
            registry.unwrap_or_else(|| CustomPropertyRegistry::shared_empty()),
            name,
            context,
            recursion_depth + 1,
        );

        return match computed_value {
            Ok(value) => computed_property_tokens(&value, context),
            Err(()) => TokenResolution::NotHandled,
        };
    }
    let data = entry.value.data();
    if matches!(data, StyleValueData::GuaranteedInvalid) {
        return TokenResolution::Invalid;
    }
    // https://drafts.csswg.org/css-cascade-5/#inheritance
    // The inherited value of a property on an element is the computed value of the property on the element’s parent
    // element.
    if context.element_store.is_some()
        || lookup == CustomPropertyLookup::ExplicitInheritance
        || store.is_some_and(|store| !std::ptr::eq(store, owner))
    {
        return computed_property_tokens(&entry.value, context);
    }
    let result = if let StyleValueData::Unresolved(unresolved_payload) = data {
        context.contains_attr_tainted_values |= unresolved_payload.contains_attr_tainted_values;
        let tokens = cached_value_tokens(data, context);
        if unresolved_payload.presence().has_any() {
            substitute_arbitrary_substitution_functions(
                store,
                registry,
                tokens.as_ref(),
                context,
                recursion_depth + 1,
                Some(substitution_context),
            )
        } else {
            TokenResolution::Resolved(tokens.as_ref().clone())
        }
    } else {
        TokenResolution::Resolved(cached_value_tokens(data, context).as_ref().clone())
    };
    if let TokenResolution::Resolved(tokens) = &result
        && let Some(keyword) = single_css_wide_keyword(tokens)
    {
        return resolve_css_wide_keyword(owner, registry, name, keyword, context, recursion_depth, lookup);
    }
    if let (Some(registry), Some(registration)) = (registry, registration) {
        let inheritance_store = match lookup {
            CustomPropertyLookup::Normal => context.inheritance_store,
            CustomPropertyLookup::ExplicitInheritance => owner.inheritance_parent.as_deref(),
        };
        return match result {
            TokenResolution::Resolved(tokens) if registration_accepts_tokens(registry, registration, &tokens) => {
                TokenResolution::Resolved(tokens)
            }
            TokenResolution::Resolved(_) | TokenResolution::Invalid | TokenResolution::Cyclic => {
                registered_property_fallback(inheritance_store, registry, registration, name, recursion_depth)
            }
            TokenResolution::NotHandled => TokenResolution::NotHandled,
        };
    }
    result
}

fn replace_var_function(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    arguments: &[ComponentValue],
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    // https://drafts.csswg.org/css-variables-1/#replace-a-var-function
    // 1. Let el be the element that the style containing the var() function is being applied to.
    //    Let first arg be the first <declaration-value> in arguments.
    //    Let second arg be the <declaration-value>? passed after the comma, or null if there was no comma.
    let comma = arguments.iter().position(ComponentValue::is_comma);
    let first_argument = &arguments[..comma.unwrap_or(arguments.len())];

    // 2. Substitute arbitrary substitution functions in first arg, then parse it as a <custom-property-name>.
    //    If parsing returned a <custom-property-name>, let result be the computed value of the corresponding custom
    //    property on el. Otherwise, let result be the guaranteed-invalid value.
    let substituted_first = match substitute_tokens(store, registry, first_argument, context, recursion_depth + 1) {
        TokenResolution::Resolved(tokens) => tokens,
        TokenResolution::Invalid | TokenResolution::Cyclic => ComponentValueList::default(),
        TokenResolution::NotHandled => return TokenResolution::NotHandled,
    };
    let name = match trim_whitespace(&substituted_first) {
        [value] => value.ident_string().filter(|name| name.starts_with_ascii("--")),
        _ => None,
    };
    if let Some(name) = name {
        let name = name.fly_string();
        match resolve_custom_property(store, registry, &name, context, recursion_depth) {
            TokenResolution::Cyclic if comma.is_none() => return TokenResolution::Cyclic,
            TokenResolution::Invalid | TokenResolution::Cyclic => {}
            result => return result,
        }
    }
    let Some(comma) = comma else {
        return TokenResolution::Invalid;
    };
    // 4. If result contains the guaranteed-invalid value, and second arg was provided, set result to the result of
    //    substitute arbitrary substitution functions on second arg.
    substitute_tokens(store, registry, &arguments[comma + 1..], context, recursion_depth + 1)
}

fn replace_inherit_function(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    arguments: &[ComponentValue],
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    // https://drafts.csswg.org/css-values-5/#replace-an-inherit-function
    let comma = arguments.iter().position(ComponentValue::is_comma);
    let first_argument = &arguments[..comma.unwrap_or(arguments.len())];
    let substituted_first = match substitute_tokens(store, registry, first_argument, context, recursion_depth + 1) {
        TokenResolution::Resolved(tokens) => tokens,
        TokenResolution::Invalid | TokenResolution::Cyclic => ComponentValueList::default(),
        TokenResolution::NotHandled => return TokenResolution::NotHandled,
    };
    let name = match trim_whitespace(&substituted_first) {
        [value] => value.ident_string().filter(|name| name.starts_with_ascii("--")),
        _ => None,
    };
    if let Some(name) = name {
        let name = name.fly_string();
        let local_scope = context.function_local_scopes.pop();
        let inherited_store = if local_scope.is_some() {
            store
        } else {
            context.inheritance_store
        };
        let result = resolve_custom_property_with_lookup(
            inherited_store,
            registry,
            &name,
            context,
            recursion_depth + 1,
            if local_scope.is_some() {
                CustomPropertyLookup::Normal
            } else {
                CustomPropertyLookup::ExplicitInheritance
            },
        );
        if let Some(local_scope) = local_scope {
            context.function_local_scopes.push(local_scope);
        }
        match result {
            TokenResolution::Invalid | TokenResolution::Cyclic => {}
            result => return result,
        }
    }
    let Some(comma) = comma else {
        return TokenResolution::Invalid;
    };
    substitute_tokens(store, registry, &arguments[comma + 1..], context, recursion_depth + 1)
}

fn replace_env_function(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    arguments: &[ComponentValue],
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    // https://drafts.csswg.org/css-env/#substitute-an-env
    let comma = arguments.iter().position(ComponentValue::is_comma);
    let first_argument = &arguments[..comma.unwrap_or(arguments.len())];
    let substituted_first = match substitute_tokens(store, registry, first_argument, context, recursion_depth + 1) {
        TokenResolution::Resolved(tokens) => tokens,
        TokenResolution::Invalid => return TokenResolution::Invalid,
        TokenResolution::Cyclic => return TokenResolution::Cyclic,
        TokenResolution::NotHandled => return TokenResolution::NotHandled,
    };
    let first_argument = trim_whitespace(&substituted_first);
    let Some(name) = first_argument.first().and_then(ComponentValue::ident) else {
        return TokenResolution::Invalid;
    };
    let Some(indices) = (|| {
        let mut indices = Vec::new();
        for token in trim_whitespace(&first_argument[1..]) {
            if token.is_whitespace() {
                continue;
            }
            let ComponentKind::Token(ParserTokenKind::Number { value, number_type }) = token.kind else {
                return None;
            };
            if number_type == CssNumberType::Number || !(0.0..=f64::from(i32::MAX)).contains(&value) {
                return None;
            }
            indices.push(value as i32);
        }
        Some(indices)
    })() else {
        return TokenResolution::Invalid;
    };

    let variable = ENVIRONMENT_VARIABLES
        .iter()
        .find(|(known_name, _, _)| name.eq_ignore_ascii_case(known_name));
    if let Some((_, dimension_count, value_type)) = variable
        && *dimension_count == indices.len()
    {
        if *value_type == "<number>" {
            return TokenResolution::from_source(b"1");
        }
        if *dimension_count == 0 {
            return TokenResolution::from_source(b"0px");
        }
        // The C++ document oracle currently exposes no viewport segments, so every
        // recognized two-dimensional viewport-segment lookup is guaranteed-invalid.
        return TokenResolution::Invalid;
    }
    let Some(comma) = comma else {
        return TokenResolution::Invalid;
    };
    substitute_tokens(store, registry, &arguments[comma + 1..], context, recursion_depth + 1)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ConditionEvaluation {
    Match(bool),
    Invalid,
    Cyclic,
    NotHandled,
}

#[derive(Debug)]
enum ParsedBooleanExpression<'a> {
    Test(&'a [ComponentValue]),
    Not(Box<ParsedBooleanExpression<'a>>),
    And(Vec<ParsedBooleanExpression<'a>>),
    Or(Vec<ParsedBooleanExpression<'a>>),
}

fn parse_boolean_expression<'a>(
    tokens: &'a [ComponentValue],
    validate_test: &mut impl FnMut(&[ComponentValue]) -> bool,
) -> Option<ParsedBooleanExpression<'a>> {
    let tokens = trim_whitespace(tokens);
    if tokens.is_empty() {
        return None;
    }
    if validate_test(tokens) {
        return Some(ParsedBooleanExpression::Test(tokens));
    }
    if tokens
        .first()
        .and_then(ComponentValue::ident)
        .is_some_and(|keyword| keyword.eq_ignore_ascii_case("not"))
    {
        return Some(ParsedBooleanExpression::Not(Box::new(parse_boolean_group(
            &tokens[1..],
            validate_test,
        )?)));
    }

    let mut combinator = None;
    let mut groups = Vec::new();
    let mut group_start = 0;
    for (index, token) in tokens.iter().enumerate() {
        let current = match token.ident() {
            Some(keyword) if keyword.eq_ignore_ascii_case("and") => Some(true),
            Some(keyword) if keyword.eq_ignore_ascii_case("or") => Some(false),
            _ => None,
        };
        if let Some(current) = current {
            if combinator.is_some_and(|combinator| combinator != current) {
                return None;
            }
            combinator = Some(current);
            groups.push(parse_boolean_group(&tokens[group_start..index], validate_test)?);
            group_start = index + 1;
        }
    }
    let Some(combinator) = combinator else {
        return parse_boolean_group(tokens, validate_test);
    };
    groups.push(parse_boolean_group(&tokens[group_start..], validate_test)?);
    Some(if combinator {
        ParsedBooleanExpression::And(groups)
    } else {
        ParsedBooleanExpression::Or(groups)
    })
}

fn parse_boolean_group<'a>(
    tokens: &'a [ComponentValue],
    validate_test: &mut impl FnMut(&[ComponentValue]) -> bool,
) -> Option<ParsedBooleanExpression<'a>> {
    let tokens = trim_whitespace(tokens);
    if let [value] = tokens
        && let ComponentKind::SimpleBlock {
            opening: ParserTokenKind::OpenParen,
            values,
        } = &value.kind
    {
        return parse_boolean_expression(values, validate_test);
    }
    validate_test(tokens).then_some(ParsedBooleanExpression::Test(tokens))
}

fn evaluate_parsed_boolean_expression(
    expression: &ParsedBooleanExpression<'_>,
    evaluate_test: &mut impl FnMut(&[ComponentValue]) -> ConditionEvaluation,
) -> ConditionEvaluation {
    match expression {
        ParsedBooleanExpression::Test(tokens) => evaluate_test(tokens),
        ParsedBooleanExpression::Not(child) => match evaluate_parsed_boolean_expression(child, evaluate_test) {
            ConditionEvaluation::Match(value) => ConditionEvaluation::Match(!value),
            other => other,
        },
        ParsedBooleanExpression::And(children) => {
            for child in children {
                match evaluate_parsed_boolean_expression(child, evaluate_test) {
                    ConditionEvaluation::Match(true) => {}
                    ConditionEvaluation::Match(false) => return ConditionEvaluation::Match(false),
                    other => return other,
                }
            }
            ConditionEvaluation::Match(true)
        }
        ParsedBooleanExpression::Or(children) => {
            for child in children {
                match evaluate_parsed_boolean_expression(child, evaluate_test) {
                    ConditionEvaluation::Match(false) => {}
                    ConditionEvaluation::Match(true) => return ConditionEvaluation::Match(true),
                    other => return other,
                }
            }
            ConditionEvaluation::Match(false)
        }
    }
}

fn registered_style_query_values_are_equal(
    registry: &CustomPropertyRegistry,
    syntax: &SyntaxNode,
    computed_tokens: &[ComponentValue],
    query_tokens: &[ComponentValue],
    resolution_context: &mut ASFResolutionContext,
) -> bool {
    let random_function_index = RandomFunctionIndex::default();
    let context = registry.parse_context(&random_function_index);
    let Some(computed) = parse_with_syntax_component_values(&context, computed_tokens, syntax, None) else {
        return false;
    };
    let Some(query) = parse_with_syntax_component_values(&context, query_tokens, syntax, None) else {
        return false;
    };
    let color_resolution_input = resolution_context
        .style_query_color_resolution_input
        .as_ref()
        .unwrap_or(&crate::css::color_resolution::EMPTY_INPUT);
    let computed_color = crate::css::color_resolution::to_color(&computed, color_resolution_input);
    let query_color = crate::css::color_resolution::to_color(&query, color_resolution_input);
    if computed_color.is_some() || query_color.is_some() {
        return computed_color.is_some() && computed_color == query_color;
    }
    // Numeric values compare computed: in their canonical unit, relative lengths resolved.
    if let (Some(computed), Some(query)) = (
        StyleRangeValue::of(&computed, resolution_context),
        StyleRangeValue::of(&query, resolution_context),
    ) {
        return computed.kind == query.kind && computed.value == query.value;
    }
    computed == query
}

/// A `<style-range-value>` as a range compares it: its type, and its value in that type's
/// canonical unit.
#[derive(Clone, Copy)]
struct StyleRangeValue {
    kind: CanonicalNumericType,
    value: f64,
}

impl StyleRangeValue {
    /// The types a `<style-range-value>` parses as, in the order it tries them.
    const SYNTAX_TYPES: [SyntaxType; 7] = [
        SyntaxType::Number,
        SyntaxType::Length,
        SyntaxType::Percentage,
        SyntaxType::Angle,
        SyntaxType::Time,
        SyntaxType::Frequency,
        SyntaxType::Resolution,
    ];

    fn of(value: &StyleValueData, context: &mut ASFResolutionContext) -> Option<Self> {
        use crate::css::calc::CalcNumericValue;
        let length_resolution_context = context.style_query_length_resolution_context;
        let tree_counting = context.style_query_tree_counting;
        let length_resolution = crate::css::calc::LengthResolution {
            context: length_resolution_context,
            fallback: None,
        };
        let (kind, value) = match *value {
            StyleValueData::Number { value } => (CanonicalNumericType::Number, value),
            StyleValueData::Integer { value } => (CanonicalNumericType::Number, f64::from(value)),
            StyleValueData::Percentage { value } => (CanonicalNumericType::Percentage, value),
            StyleValueData::Length { value, unit } => (
                CanonicalNumericType::Length,
                CalcNumericValue::Length { value, unit }.to_canonical_number(length_resolution),
            ),
            StyleValueData::Angle { value, unit } => (
                CanonicalNumericType::Angle,
                CalcNumericValue::Angle { value, unit }.to_canonical_number(length_resolution),
            ),
            StyleValueData::Time { value, unit } => (
                CanonicalNumericType::Time,
                CalcNumericValue::Time { value, unit }.to_canonical_number(length_resolution),
            ),
            StyleValueData::Frequency { value, unit } => (
                CanonicalNumericType::Frequency,
                CalcNumericValue::Frequency { value, unit }.to_canonical_number(length_resolution),
            ),
            StyleValueData::Resolution { value, unit } => (
                CanonicalNumericType::Resolution,
                CalcNumericValue::Resolution { value, unit }.to_canonical_number(length_resolution),
            ),
            // A tree-counting function resolves against the queried element's place among its
            // siblings first.
            StyleValueData::TreeCountingFunction { function, .. } => {
                context.note_style_query_sibling_position_dependency();
                let (sibling_count, sibling_index) = tree_counting?;
                let value = if function == 0 { sibling_count } else { sibling_index };
                (CanonicalNumericType::Number, value as f64)
            }
            StyleValueData::Calculated { .. } => {
                if crate::css::style_compute::collect_external_value_dependencies(value).uses_tree_counting_function {
                    context.note_style_query_sibling_position_dependency();
                }
                match tree_counting
                    .zip(length_resolution_context)
                    .and_then(|(tree_counting, context)| {
                        crate::css::calc::absolutize_calculation_value(value, context, Some(tree_counting), &[])
                    }) {
                    Some(crate::css::calc::AbsolutizedCalculation::Value(resolved)) => {
                        return Self::of(&resolved, context);
                    }
                    Some(crate::css::calc::AbsolutizedCalculation::Percentage(value)) => {
                        (CanonicalNumericType::Percentage, value)
                    }
                    Some(crate::css::calc::AbsolutizedCalculation::Unchanged) | None => {
                        crate::css::calc::resolve_calculated_canonically(value, length_resolution_context)?
                    }
                }
            }
            _ => return None,
        };
        value.is_finite().then_some(Self { kind, value })
    }

    // https://drafts.csswg.org/css-conditional-5/#style-container
    fn compare(self, comparison: FeatureComparison, other: Self) -> bool {
        // A unitless zero compares with any dimension.
        let is_dimension = |kind| !matches!(kind, CanonicalNumericType::Number | CanonicalNumericType::Percentage);
        let is_unitless_zero = |value: Self| value.kind == CanonicalNumericType::Number && value.value == 0.0;
        if self.kind != other.kind
            && !(is_unitless_zero(self) && is_dimension(other.kind))
            && !(is_unitless_zero(other) && is_dimension(self.kind))
        {
            return false;
        }
        match comparison {
            FeatureComparison::Equal => self.value == other.value,
            FeatureComparison::LessThan => self.value < other.value,
            FeatureComparison::LessThanOrEqual => self.value <= other.value,
            FeatureComparison::GreaterThan => self.value > other.value,
            FeatureComparison::GreaterThanOrEqual => self.value >= other.value,
        }
    }
}

/// One `<style-range-value>`: a custom property name substitutes as if wrapped in `var()`, other
/// values have their arbitrary substitution functions substituted, and either then parses as one
/// of the numeric types. `None` when it does not.
fn evaluate_style_range_value(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    operand: &ParsedStyleRangeValue,
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> Result<Option<StyleRangeValue>, ConditionEvaluation> {
    let resolution = match operand {
        ParsedStyleRangeValue::Property(name) => {
            let name = name.fly_string();
            resolve_custom_property(store, registry, &name, context, recursion_depth + 1)
        }
        ParsedStyleRangeValue::Components(values) => {
            let values = values.as_slice();
            substitute_arbitrary_substitution_functions(store, registry, values, context, recursion_depth + 1, None)
        }
    };
    let resolved = match resolution {
        TokenResolution::Resolved(tokens) => tokens,
        TokenResolution::Invalid => return Ok(None),
        TokenResolution::Cyclic => return Err(ConditionEvaluation::Cyclic),
        TokenResolution::NotHandled => return Err(ConditionEvaluation::NotHandled),
    };
    let random_function_index = RandomFunctionIndex::default();
    let registry_parse_context;
    let parse_context = match (context.parse_context, registry) {
        (Some(parse_context), _) => parse_context,
        (None, Some(registry)) => {
            registry_parse_context = registry.parse_context(&random_function_index);
            &registry_parse_context
        }
        (None, None) => return Ok(None),
    };
    Ok(StyleRangeValue::SYNTAX_TYPES
        .iter()
        .find_map(|&syntax_type| {
            parse_with_syntax_component_values(parse_context, &resolved, &SyntaxNode::Type(syntax_type), None)
        })
        .and_then(|value| StyleRangeValue::of(&value, context)))
}

// https://drafts.csswg.org/css-conditional-5/#style-container
fn evaluate_style_range(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    range: &StyleFeature,
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> ConditionEvaluation {
    let StyleFeature::Range {
        left,
        left_comparison,
        middle,
        right,
    } = range
    else {
        return ConditionEvaluation::Invalid;
    };
    let mut value = |operand: &ParsedStyleRangeValue| match evaluate_style_range_value(
        store,
        registry,
        operand,
        context,
        recursion_depth + 1,
    ) {
        Ok(Some(value)) => Ok(value),
        Ok(None) => Err(ConditionEvaluation::Match(false)),
        Err(result) => Err(result),
    };
    let comparisons = std::iter::once((*left_comparison, middle))
        .chain(right.as_ref().map(|(comparison, right)| (*comparison, right)));
    let mut previous = match value(left) {
        Ok(value) => value,
        Err(result) => return result,
    };
    for (comparison, operand) in comparisons {
        let next = match value(operand) {
            Ok(value) => value,
            Err(result) => return result,
        };
        if !previous.compare(comparison, next) {
            return ConditionEvaluation::Match(false);
        }
        previous = next;
    }
    ConditionEvaluation::Match(true)
}

fn has_comparison(values: &[ComponentValue]) -> bool {
    values.iter().any(|value| match &value.kind {
        ComponentKind::Token(_) => value.is_delim(b'<') || value.is_delim(b'>') || value.is_delim(b'='),
        ComponentKind::Function { values, .. } | ComponentKind::SimpleBlock { values, .. } => has_comparison(values),
    })
}

fn validate_style_feature(tokens: &[ComponentValue]) -> bool {
    let tokens = trim_whitespace(tokens);
    // A `<style-feature-value>` is a `<declaration-value>`, which holds no top-level `!`.
    if tokens.iter().any(|value| value.is_delim(b'!')) {
        return false;
    }
    if has_comparison(tokens) {
        return true;
    }
    let colon = tokens.iter().position(ComponentValue::is_colon);
    let name_tokens = trim_whitespace(&tokens[..colon.unwrap_or(tokens.len())]);
    matches!(name_tokens, [value] if value.ident().is_some())
}

fn evaluate_style_feature(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    tokens: &[ComponentValue],
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> ConditionEvaluation {
    let tokens = trim_whitespace(tokens);
    if has_comparison(tokens) {
        // A range parses as the query parser has it, as it did when C++ evaluated it.
        let Some(range) = parse_style_range(tokens) else {
            return ConditionEvaluation::Invalid;
        };
        return evaluate_style_range(store, registry, &range, context, recursion_depth + 1);
    }
    let colon = tokens.iter().position(ComponentValue::is_colon);
    let name_tokens = trim_whitespace(&tokens[..colon.unwrap_or(tokens.len())]);
    let [name] = name_tokens else {
        return ConditionEvaluation::Invalid;
    };
    let Some(name) = name.ident_string() else {
        return ConditionEvaluation::Invalid;
    };
    let query = colon.map(|colon| &tokens[colon + 1..]);
    evaluate_named_style_feature(store, registry, name, query, context, recursion_depth)
}

/// A `style()` feature naming a property, `name` or `name: query`.
fn evaluate_named_style_feature(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    name: &ParserString,
    query: Option<&[ComponentValue]>,
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> ConditionEvaluation {
    if !name.starts_with_ascii("--") {
        return ConditionEvaluation::Match(false);
    }

    let name = name.fly_string();
    let name = name.as_ref();
    let local_registration = context
        .function_local_scopes
        .last()
        .and_then(|scope| scope.registrations.get(name))
        .cloned();
    let registration = context
        .function_local_scopes
        .is_empty()
        .then(|| registry.and_then(|registry| registry.registrations.get(name)))
        .flatten();
    let computed = resolve_custom_property(store, registry, name, context, recursion_depth + 1);
    let computed = match computed {
        TokenResolution::Resolved(tokens) => Some(tokens),
        TokenResolution::Invalid | TokenResolution::Cyclic => None,
        TokenResolution::NotHandled => return ConditionEvaluation::NotHandled,
    };
    let Some(query) = query else {
        // A <style-feature-boolean> is true if the computed value differs from the property's initial value.
        // NB: A function parameter's initial value is its argument, but a parameter holding one still matches.
        let (Some(registry), Some(registration), Some(computed)) = (registry, registration, &computed) else {
            return ConditionEvaluation::Match(computed.is_some());
        };
        let Some(initial) = &registration.initial_source else {
            return ConditionEvaluation::Match(true);
        };
        return ConditionEvaluation::Match(!registered_style_query_values_are_equal(
            registry,
            &registration.syntax,
            computed,
            &parse_substitution_values(initial.units()).expect("registered initial value has valid components"),
            context,
        ));
    };
    let query = trim_whitespace(query);

    if let Some(keyword) = single_css_wide_keyword(query) {
        if keyword.eq_ignore_ascii_case("revert") || keyword.eq_ignore_ascii_case("revert-layer") {
            return ConditionEvaluation::Match(false);
        }
        let expected = if keyword.eq_ignore_ascii_case("initial")
            || keyword.eq_ignore_ascii_case("unset") && registration.is_some_and(|registration| !registration.inherits)
        {
            local_registration
                .as_ref()
                .and_then(|registration| registration.initial_tokens.clone())
                .or_else(|| {
                    registration.and_then(|registration| {
                        registration
                            .initial_source
                            .as_ref()
                            .and_then(|initial| parse_substitution_values(initial.units()))
                    })
                })
        } else {
            let local_scope = local_registration
                .as_ref()
                .and_then(|_| context.function_local_scopes.pop());
            let inherited_store = local_scope.as_ref().map_or(context.inheritance_store, |_| store);
            let result = resolve_custom_property(inherited_store, registry, name, context, recursion_depth + 1);
            if let Some(local_scope) = local_scope {
                context.function_local_scopes.push(local_scope);
            }
            match result {
                TokenResolution::Resolved(tokens) => Some(tokens),
                TokenResolution::Invalid => None,
                TokenResolution::Cyclic => return ConditionEvaluation::Cyclic,
                TokenResolution::NotHandled => return ConditionEvaluation::NotHandled,
            }
        };
        return ConditionEvaluation::Match(match (computed, expected) {
            (None, None) => true,
            (Some(computed), Some(expected)) if local_registration.is_some() || registration.is_some() => {
                registered_style_query_values_are_equal(
                    registry.expect("registered syntax requires registry"),
                    local_registration
                        .as_ref()
                        .map(|registration| registration.syntax)
                        .unwrap_or_else(|| &registration.expect("registered property").syntax),
                    &computed,
                    &expected,
                    context,
                )
            }
            (Some(computed), Some(expected)) => {
                normalized_component_values_equal(trim_whitespace(&computed), trim_whitespace(&expected))
            }
            _ => false,
        });
    }

    let Some(computed) = computed else {
        return ConditionEvaluation::Match(false);
    };
    if let (Some(registry), Some(syntax)) = (
        registry,
        local_registration
            .as_ref()
            .map(|registration| registration.syntax)
            .or_else(|| registration.map(|registration| &*registration.syntax)),
    ) {
        return ConditionEvaluation::Match(registered_style_query_values_are_equal(
            registry, syntax, &computed, query, context,
        ));
    }
    ConditionEvaluation::Match(normalized_component_values_equal(trim_whitespace(&computed), query))
}

/// Evaluate a container query's style feature against a container's retained style: its
/// custom-property environment, and lengths and colors as its record computes them.
pub(crate) fn evaluate_retained_container_style_feature(
    store: Option<&CustomPropertyStore>,
    registry: &CustomPropertyRegistry,
    feature: &StyleFeature,
    length_resolution_context: &crate::css::style_compute::FfiLengthResolutionContext,
    color_resolution_input: crate::css::color_resolution::ColorResolutionInput<'_>,
    tree_counting: Option<(u64, u64)>,
    read_sibling_position: &mut bool,
) -> MatchResult {
    let mut dependencies = None;
    let mut context = ASFResolutionContext {
        inheritance_store: store.and_then(|store| store.inheritance_parent.as_deref()),
        style_query_length_resolution_context: Some(length_resolution_context),
        style_query_color_resolution_input: Some(color_resolution_input),
        style_query_tree_counting: tree_counting,
        style_query_dependencies: Some(&mut dependencies),
        ..Default::default()
    };
    let registry = Some(registry);
    let result = match feature {
        StyleFeature::Boolean(name) => evaluate_named_style_feature(store, registry, name, None, &mut context, 0),
        // The value is computed with respect to the query container, as the property it is compared with was.
        StyleFeature::Plain { name, value, .. }
            if name.starts_with_ascii("--") && contains_arbitrary_substitution_function(value) =>
        {
            match substitute_tokens(store, registry, value.as_slice(), &mut context, 0) {
                TokenResolution::Resolved(query) => {
                    evaluate_named_style_feature(store, registry, name, Some(&query), &mut context, 0)
                }
                // NB: A value that substitutes to nothing valid matches a property that holds nothing valid either.
                TokenResolution::Invalid | TokenResolution::Cyclic => {
                    match resolve_custom_property(store, registry, name.fly_string().as_ref(), &mut context, 1) {
                        TokenResolution::Resolved(_) => ConditionEvaluation::Match(false),
                        TokenResolution::Invalid | TokenResolution::Cyclic => ConditionEvaluation::Match(true),
                        TokenResolution::NotHandled => ConditionEvaluation::NotHandled,
                    }
                }
                TokenResolution::NotHandled => ConditionEvaluation::NotHandled,
            }
        }
        StyleFeature::Plain { name, value, .. } => {
            evaluate_named_style_feature(store, registry, name, Some(value), &mut context, 0)
        }
        StyleFeature::Range { .. } => evaluate_style_range(store, registry, feature, &mut context, 0),
    };
    *read_sibling_position |=
        dependencies.is_some_and(|dependencies: Box<StyleQueryDependencies>| dependencies.sibling_position);
    match result {
        ConditionEvaluation::Match(true) => MatchResult::True,
        ConditionEvaluation::Match(false) => MatchResult::False,
        ConditionEvaluation::Invalid | ConditionEvaluation::NotHandled | ConditionEvaluation::Cyclic => {
            MatchResult::Unknown
        }
    }
}

fn evaluate_style_query(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    tokens: &[ComponentValue],
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> ConditionEvaluation {
    let Some(expression) = parse_boolean_expression(tokens, &mut validate_style_feature) else {
        return ConditionEvaluation::Invalid;
    };
    evaluate_parsed_boolean_expression(&expression, &mut |feature| {
        evaluate_style_feature(store, registry, feature, context, recursion_depth + 1)
    })
}

fn validate_if_test(tokens: &[ComponentValue]) -> bool {
    let test = trim_whitespace(tokens);
    let [test] = test else {
        return false;
    };
    let Some((name, values)) = test.function() else {
        return false;
    };
    if name.eq_ignore_ascii_case("style") {
        return parse_boolean_expression(values, &mut validate_style_feature).is_some();
    }
    true
}

fn evaluate_if_condition(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    tokens: &[ComponentValue],
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> ConditionEvaluation {
    if matches!(trim_whitespace(tokens), [value] if value.ident().is_some_and(|keyword| keyword.eq_ignore_ascii_case("else")))
    {
        return ConditionEvaluation::Match(true);
    }
    let Some(expression) = parse_boolean_expression(tokens, &mut validate_if_test) else {
        return ConditionEvaluation::Invalid;
    };
    evaluate_parsed_boolean_expression(&expression, &mut |test| {
        let test = trim_whitespace(test);
        let (name, values) = test[0].function().expect("validated if condition test");
        if name.eq_ignore_ascii_case("style") {
            return evaluate_style_query(store, registry, values, context, recursion_depth + 1);
        }
        if name.eq_ignore_ascii_case("media") {
            let Some(environment) = context.media_environment else {
                return ConditionEvaluation::NotHandled;
            };
            let source = serialize_tokens(values);
            return match parse_and_evaluate_media_if_condition(&source, environment) {
                Some(MatchResult::True) => ConditionEvaluation::Match(true),
                Some(MatchResult::False | MatchResult::Unknown) => ConditionEvaluation::Match(false),
                None => ConditionEvaluation::Invalid,
            };
        }
        if name.eq_ignore_ascii_case("supports") {
            let Some(parse_context) = context.parse_context else {
                return ConditionEvaluation::NotHandled;
            };
            let source = serialize_tokens(values);
            return match parse_and_evaluate_supports_if_condition(&source, parse_context) {
                Some(MatchResult::True) => ConditionEvaluation::Match(true),
                Some(MatchResult::False | MatchResult::Unknown) => ConditionEvaluation::Match(false),
                None => ConditionEvaluation::Invalid,
            };
        }
        ConditionEvaluation::Match(false)
    })
}

fn split_function_arguments(tokens: &[ComponentValue]) -> Vec<&[ComponentValue]> {
    if trim_whitespace(tokens).is_empty() {
        return Vec::new();
    }
    tokens.split(ComponentValue::is_comma).collect()
}

// https://drafts.csswg.org/css-mixins/#resolve-function-styles
fn resolve_function_styles<'a, 'names>(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    local_scope: FunctionLocalScope<'a>,
    property_names: impl IntoIterator<Item = &'names Utf16FlyString>,
    context: &mut ASFResolutionContext<'a>,
    recursion_depth: u32,
) -> HashMap<Utf16FlyString, TokenResolution> {
    // 1. Create a "hypothetical element" el that acts as a child of calling context's element. el is featureless, and
    //    only custom properties and the result descriptor apply to it.

    // NB: FunctionLocalScope represents the custom properties and registrations of the hypothetical element. The rest
    //     of the calling context is represented by the stores and ASFResolutionContext.
    context.function_local_scopes.push(local_scope);

    // 2. Apply rule to el to the specified value stage, with the following changes:
    //
    // - Only the custom property registrations in registrations are visible; all other custom properties are treated as
    //   unregistered.
    //
    // - The inherited value of calling context's property is the guaranteed-invalid value.
    //
    // AD-HOC: We treat references to the calling property as cycles to match Chrome's behavior.
    //
    // - On custom properties, initial resolves to the registration's initial value, inherit resolves like an inherit()
    //   function for that property, and any other CSS-wide keyword resolves to the guaranteed-invalid value. On result,
    //   CSS-wide keywords are left unresolved.
    //
    //   NB: resolve_function_local_property() implements the registration and CSS-wide keyword behavior.
    //
    // - During replacement of a custom property prop, the substitution context also includes custom function.
    //
    //   NB: resolve_function_local_property() includes the function identity in the property substitution context.

    // 3. Determine the computed value of all custom properties and the result "property" on el. Aside from custom
    //    property references and numbers/percentages, values that would normally refer to the element being styled refer
    //    to calling context's root element instead.
    let mut computed_values = HashMap::new();

    for name in property_names {
        let value = resolve_custom_property(store, registry, name, context, recursion_depth + 1);
        // FIXME: Duplicate parameter names make an @function rule invalid, but the parser currently accepts them. Keep
        //        the pre-refactor behavior where a later failed resolution does not replace an earlier successful one.
        //        See https://drafts.csswg.org/css-mixins/#function-prelude
        if matches!(&value, TokenResolution::Resolved(_))
            || !computed_values
                .get(name)
                .is_some_and(|value| matches!(value, TokenResolution::Resolved(_)))
        {
            computed_values.insert(name.clone(), value);
        }
    }

    context.function_local_scopes.pop().expect("function-local style scope");

    // 4. Return el's styles.
    computed_values
}

// https://drafts.csswg.org/css-mixins/#evaluate-a-custom-function
fn evaluate_a_custom_function<'a>(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    custom_function: &'a PreparedCustomFunction<'a>,
    arguments: Vec<TokenResolution>,
    context: &mut ASFResolutionContext<'a>,
    recursion_depth: u32,
) -> TokenResolution {
    // 1. Let substitution context be a substitution context containing «"function", custom function».
    // Note: Due to tree-scoping, the same function name may appear multiple times on the stack while referring to
    //       different custom functions. For this reason, the custom function itself is included in the substitution
    //       context, not just its name.
    let substitution_context =
        SubstitutionContext::new(SubstitutionContextDependency::Function(custom_function.identity));

    // 2. Guard substitution context for the remainder of this algorithm. If substitution context is marked as cyclic,
    //    return the guaranteed-invalid value.
    let Some(_guard) = context.guarded_contexts.guard(&substitution_context) else {
        return TokenResolution::Cyclic;
    };

    // 3. If the number of items in arguments is greater than the number of function parameters in custom function,
    //    return the guaranteed-invalid value.
    if arguments.len() > custom_function.signature.parameters.len() {
        return TokenResolution::Invalid;
    }

    // 4. Let registrations be an initially empty set of custom property registrations.
    let mut registrations = HashMap::new();

    // 5. For each function parameter of custom function, create a custom property registration with the parameter's
    //    name, a syntax of the parameter type, an inherit flag of "true", and no initial value. Add the registration to
    //    registrations.
    for parameter in &custom_function.signature.parameters {
        registrations.insert(
            parameter.name.clone(),
            FunctionLocalRegistration {
                syntax: &parameter.syntax,
                initial_tokens: None,
                is_result: false,
            },
        );
    }

    // 6. If custom function has a return type, create a custom property registration with the name "result", a syntax
    //    of the return type, an inherit flag of "false", and no initial value. Add the registration to registrations.
    // NB: Custom functions always have a return type, defaulting to the universal syntax.
    let result_name = Utf16FlyString::from_utf16(&b"result".map(u16::from));
    registrations.insert(
        result_name.clone(),
        FunctionLocalRegistration {
            syntax: &custom_function.signature.return_type,
            initial_tokens: None,
            is_result: true,
        },
    );

    // NB: FunctionLocalRegistration does not store an inherit flag; resolve_function_local_property() encodes the
    //     parameter and result inheritance behavior.

    // 7. Let argument rule be an initially empty style rule.
    let mut argument_rule = HashMap::new();

    // 8. For each function parameter of custom function:
    for (index, parameter) in custom_function.signature.parameters.iter().enumerate() {
        // AD-HOC: Chrome (the only other implementer at time of writing) resolves the entire function to the
        //         guaranteed-invalid value if a parameter without a default value is omitted.
        //         See https://github.com/w3c/csswg-drafts/issues/14165
        if index >= arguments.len() && parameter.default_value.is_none() {
            return TokenResolution::Invalid;
        }

        // 1. Let arg value be the value of the corresponding argument in arguments, or the guaranteed-invalid value if
        //    there is no corresponding argument.
        let arg_value = arguments.get(index).unwrap_or(&TokenResolution::Invalid);

        // 2. Let default value be the parameter's default value.
        let default_value = custom_function.parameter_defaults[index].as_ref();

        // 3. Add a custom property to argument rule with a name of the parameter's name, and a value of
        //    'first-valid(arg value, default value)'.
        // FIXME: We haven't implemented first-valid() yet, so do the equivalent inline.
        let normalized_argument = match arg_value {
            TokenResolution::Resolved(tokens) => normalize_function_tokens(registry, &parameter.syntax, tokens),
            TokenResolution::NotHandled => return TokenResolution::NotHandled,
            TokenResolution::Invalid | TokenResolution::Cyclic => TokenResolution::Invalid,
        };

        let value = match normalized_argument {
            TokenResolution::Resolved(tokens) => Some(FunctionLocalValue {
                tokens: Cow::Owned(tokens),
                includes_substitution: false,
            }),
            TokenResolution::NotHandled => return TokenResolution::NotHandled,
            TokenResolution::Invalid | TokenResolution::Cyclic => default_value.map(|tokens| FunctionLocalValue {
                tokens: Cow::Borrowed(tokens),
                includes_substitution: true,
            }),
        };
        // NB: If neither value is valid, the missing declaration represents the guaranteed-invalid value: the
        //     parameter's registration has no initial value, so resolve_function_local_property() returns Invalid.
        if let Some(value) = value {
            argument_rule.insert(parameter.name.clone(), value);
        }
    }

    // 9. Resolve function styles using custom function, argument rule, registrations, and calling context. Let argument
    //    styles be the result.
    let mut registrations = Rc::new(registrations);
    let mut argument_styles = resolve_function_styles(
        store,
        registry,
        FunctionLocalScope {
            function_identity: custom_function.identity,
            resolved_value_cache: Rc::new(RefCell::new(HashMap::new())),
            values: argument_rule,
            registrations: Rc::clone(&registrations),
        },
        custom_function
            .signature
            .parameters
            .iter()
            .map(|parameter| &parameter.name),
        context,
        recursion_depth,
    );

    // 10. Let body rule be the function body of custom function, as a style rule.
    let mut body_rule = HashMap::new();

    // 11. For each custom property registration of registrations except the registration with the name "result",
    //     set its initial value to the corresponding value in argument styles, and prepend a custom property to
    //     body rule with the property name and value in argument styles.
    let mutable_registrations = Rc::get_mut(&mut registrations)
        .expect("function-local registrations are uniquely owned after argument resolution clone is dropped");

    for (name, registration) in mutable_registrations.iter_mut() {
        if registration.is_result {
            continue;
        }

        let Some(TokenResolution::Resolved(tokens)) = argument_styles.remove(name) else {
            // NB: An absent value and no initial tokens represent the guaranteed-invalid value. Keeping the
            //     registration ensures the parameter still shadows values from the calling context.
            continue;
        };

        body_rule.insert(
            name.clone(),
            FunctionLocalValue {
                tokens: Cow::Owned(tokens.clone()),
                includes_substitution: false,
            },
        );

        registration.initial_tokens = Some(tokens);
    }

    // NB: Function declarations are inserted after the parameter values to emulate prepending those parameter values
    //     to the body rule.
    for declaration in &custom_function.declarations {
        body_rule.insert(
            declaration.name.clone(),
            FunctionLocalValue {
                tokens: Cow::Borrowed(&declaration.tokens),
                includes_substitution: declaration.includes_substitution,
            },
        );
    }

    // 12. Resolve function styles using custom function, body rule, registrations, and calling context. Let body styles
    //     be the result.
    let mut body_styles = resolve_function_styles(
        store,
        registry,
        FunctionLocalScope {
            function_identity: custom_function.identity,
            resolved_value_cache: Rc::new(RefCell::new(HashMap::new())),
            values: body_rule,
            registrations,
        },
        custom_function
            .declarations
            .iter()
            .map(|declaration| &declaration.name)
            .filter(|name| *name != &result_name)
            .chain(std::iter::once(&result_name)),
        context,
        recursion_depth,
    );

    // 13. If substitution context is marked as a cyclic substitution context, return the guaranteed-invalid value.
    // Note: Nested arbitrary substitution functions may have marked substitution context as cyclic at some point after
    //       step 2, for example when resolving result.
    if substitution_context.is_cyclic.get() {
        return TokenResolution::Cyclic;
    }

    // 14. Return the value of the result property in body styles.
    body_styles.remove(&result_name).expect("function result was resolved")
}

// https://drafts.csswg.org/css-mixins/#replace-a-dashed-function
fn replace_a_dashed_function(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    name: &[u16],
    arguments: &[ComponentValue],
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    // 1. Let function be the result of dereferencing the dashed function's name as a tree-scoped reference. If no such
    //    name exists, return the guaranteed-invalid value.
    let Some(functions) = context.custom_functions else {
        return TokenResolution::NotHandled;
    };

    // NB: Nested calls are resolved from the scope where the calling function was defined.
    // FIXME: Top-level calls use the element's style scope, but should use the relevant CSS rule's style scope. See the
    //        failing tests in function-shadow.html.
    let caller_scope_identity = context
        .function_local_scopes
        .last()
        .and_then(|scope| {
            functions
                .definitions
                .iter()
                .find(|definition| definition.identity == scope.function_identity)
                .map(|definition| definition.scope_identity)
        })
        .unwrap_or(functions.caller_scope_identity);
    let definition = functions
        .visible
        .iter()
        .filter(|visibility| visibility.caller_scope_identity == caller_scope_identity)
        .map(|visibility| &functions.definitions[visibility.definition_index])
        .find(|definition| definition.signature.name.units() == name);
    let Some(function) = definition else {
        return TokenResolution::Invalid;
    };

    // FIXME: The generic arbitrary-substitution algorithm should parse the argument grammar before invoking this
    //        replacement algorithm. The existing Rust path receives raw contents and parses them here instead.
    let argument_slices = split_function_arguments(arguments);

    // 2. For each arg in arguments, substitute arbitrary substitution functions in arg, and replace arg with the
    //    result.
    // Note: This may leave some (or all) arguments as the guaranteed-invalid value, triggering default values (if any).
    let mut substituted_arguments = Vec::with_capacity(argument_slices.len());
    for arg in argument_slices {
        let argument = trim_whitespace(arg);
        // NB: Braces disambiguate an argument that contains a top-level comma; they are not part of the argument value.
        let argument = if let [value] = argument
            && let ComponentKind::SimpleBlock {
                opening: ParserTokenKind::OpenCurly,
                values,
            } = &value.kind
        {
            values.as_slice()
        } else {
            ComponentValueSlice::new(argument)
        };

        substituted_arguments.push(substitute_tokens(
            store,
            registry,
            argument,
            context,
            recursion_depth + 1,
        ));
    }

    // 3. If dashed function is being substituted into a property on an element, let calling context be a calling
    //    context with that element and that property. Otherwise, let calling context contain the hypothetical element
    //    and descriptor into which the function is being substituted.

    // NB: The calling context is represented by store, registry, and ASFResolutionContext, passed below.

    // 4. Evaluate a custom function, using function, arguments, and calling context, and return the equivalent token
    //    sequence of the value resulting from the evaluation.
    evaluate_a_custom_function(
        store,
        registry,
        function,
        substituted_arguments,
        context,
        recursion_depth,
    )
}

fn replace_if_function(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    arguments: &[ComponentValue],
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    // https://drafts.csswg.org/css-values-5/#replace-an-if-function
    let mut branch_start = 0;
    while branch_start < arguments.len() {
        let branch_length = arguments[branch_start..]
            .iter()
            .position(|value| value.is(&ParserTokenKind::Semicolon))
            .unwrap_or(arguments.len() - branch_start);
        let branch_end = branch_start + branch_length;
        let branch = &arguments[branch_start..branch_end];
        let Some(colon) = branch.iter().position(ComponentValue::is_colon) else {
            return TokenResolution::Invalid;
        };
        let condition = if contains_arbitrary_substitution_function(&branch[..colon]) {
            match substitute_tokens(store, registry, &branch[..colon], context, recursion_depth + 1) {
                TokenResolution::Resolved(tokens) => Cow::Owned(tokens.into_vec()),
                TokenResolution::Invalid | TokenResolution::Cyclic => Cow::Borrowed(&branch[..colon]),
                TokenResolution::NotHandled => return TokenResolution::NotHandled,
            }
        } else {
            Cow::Borrowed(&branch[..colon])
        };
        match evaluate_if_condition(store, registry, &condition, context, recursion_depth + 1) {
            ConditionEvaluation::Match(true) => {
                return substitute_tokens(store, registry, &branch[colon + 1..], context, recursion_depth + 1);
            }
            ConditionEvaluation::Match(false) | ConditionEvaluation::Invalid => {}
            ConditionEvaluation::Cyclic => return TokenResolution::Cyclic,
            ConditionEvaluation::NotHandled => return TokenResolution::NotHandled,
        }
        branch_start = branch_end + 1;
    }
    TokenResolution::Resolved(ComponentValueList::default())
}

enum AttrSyntax {
    Omitted,
    RawString,
    Number,
    Unit(Vec<u16>),
    Syntax(SyntaxNode),
}

enum AttrSyntaxParseFailure {
    SyntaxOmitted,
    SyntaxSpecified,
}

fn parse_attr_syntax(tokens: &[ComponentValue]) -> Result<(Vec<u16>, AttrSyntax), AttrSyntaxParseFailure> {
    let tokens = trim_whitespace(tokens);
    let Some(name) = tokens.first().and_then(ComponentValue::ident) else {
        return Err(AttrSyntaxParseFailure::SyntaxOmitted);
    };
    Ok((name.to_vec(), parse_attr_type(trim_whitespace(&tokens[1..]))?))
}

fn parse_attr_type(rest: &[ComponentValue]) -> Result<AttrSyntax, AttrSyntaxParseFailure> {
    if rest.is_empty() {
        return Ok(AttrSyntax::Omitted);
    }
    if let Some(syntax) = rest[0].ident() {
        let syntax = if syntax.eq_ignore_ascii_case("raw-string") {
            AttrSyntax::RawString
        } else if syntax.eq_ignore_ascii_case("number") {
            AttrSyntax::Number
        } else if crate::css::parser::value_parser::is_dimension_unit(syntax) {
            AttrSyntax::Unit(syntax.to_vec())
        } else {
            return Err(AttrSyntaxParseFailure::SyntaxOmitted);
        };
        if rest.len() != 1 {
            return Err(AttrSyntaxParseFailure::SyntaxSpecified);
        }
        return Ok(syntax);
    }
    if rest[0].is_delim(b'%') {
        if rest.len() != 1 {
            return Err(AttrSyntaxParseFailure::SyntaxSpecified);
        }
        return Ok(AttrSyntax::Unit(vec![u16::from(b'%')]));
    }
    if let Some((function, values)) = rest[0].function()
        && function.eq_ignore_ascii_case("type")
    {
        let Some(syntax) = parse_syntax_values(values, false) else {
            return Err(AttrSyntaxParseFailure::SyntaxOmitted);
        };
        if rest.len() != 1 {
            return Err(AttrSyntaxParseFailure::SyntaxSpecified);
        }
        return Ok(AttrSyntax::Syntax(syntax));
    }
    Err(AttrSyntaxParseFailure::SyntaxOmitted)
}

pub(crate) fn literal_attr_arguments_are_valid(values: &[ComponentValue]) -> bool {
    let comma = values.iter().position(ComponentValue::is_comma).unwrap_or(values.len());
    let first_argument = trim_whitespace(&values[..comma]);
    if contains_arbitrary_substitution_function(first_argument) {
        return true;
    }
    // https://drafts.csswg.org/css-values-5/#attr-notation
    // INTEROP: Chrome and Firefox reject malformed literal names and types at parse time,
    //          rather than using the replacement algorithm's fallback. Defer substituted arguments.
    // NB: Namespace components cannot contain intervening whitespace.
    let name_end = if first_argument.first().is_some_and(|value| value.is_delim(b'|')) {
        2
    } else if first_argument.first().and_then(ComponentValue::ident).is_some() {
        if first_argument.get(1).is_some_and(|value| value.is_delim(b'|')) {
            3
        } else {
            1
        }
    } else {
        return false;
    };
    if first_argument
        .get(name_end - 1)
        .and_then(ComponentValue::ident)
        .is_none()
    {
        return false;
    }
    let attr_type = trim_whitespace(&first_argument[name_end..]);
    // NB: An unknown unit identifier is grammatically valid and uses the computed-value fallback.
    (attr_type.len() == 1 && attr_type[0].ident().is_some()) || parse_attr_type(attr_type).is_ok()
}

fn parse_attr_value_with_syntax(
    registry: Option<&CustomPropertyRegistry>,
    values: &[ComponentValue],
    syntax: &SyntaxNode,
) -> Option<ComponentValueList> {
    let registry = registry?;
    let random_function_index = RandomFunctionIndex::default();
    let parsed =
        parse_with_syntax_component_values(&registry.parse_context(&random_function_index), values, syntax, None)?;
    if let StyleValueData::Unresolved(unresolved_payload) = &parsed {
        return Some(unresolved_payload.components());
    }
    let serialized = crate::css::serialize::serialize_style_value_to_utf16(&parsed);
    parse_substitution_values(&serialized)
}

fn attr_fallback(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    arguments: &[ComponentValue],
    comma: Option<usize>,
    syntax_was_omitted: bool,
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    let Some(comma) = comma else {
        if syntax_was_omitted {
            return TokenResolution::from_source(b"\"\"");
        }
        return TokenResolution::Invalid;
    };
    substitute_tokens(store, registry, &arguments[comma + 1..], context, recursion_depth + 1)
}

fn replace_attr_function(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    arguments: &[ComponentValue],
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    // https://drafts.csswg.org/css-values-5/#replace-an-attr-function
    let comma = arguments.iter().position(ComponentValue::is_comma);
    let first_argument = &arguments[..comma.unwrap_or(arguments.len())];
    let substituted_first = match substitute_tokens(store, registry, first_argument, context, recursion_depth + 1) {
        TokenResolution::Resolved(tokens) => tokens,
        TokenResolution::Invalid | TokenResolution::Cyclic => {
            return attr_fallback(store, registry, arguments, comma, true, context, recursion_depth);
        }
        TokenResolution::NotHandled => return TokenResolution::NotHandled,
    };
    let (attribute_name, syntax) = match parse_attr_syntax(&substituted_first) {
        Ok(parsed) => parsed,
        Err(failure) => {
            return attr_fallback(
                store,
                registry,
                arguments,
                comma,
                matches!(failure, AttrSyntaxParseFailure::SyntaxOmitted),
                context,
                recursion_depth,
            );
        }
    };
    let syntax_was_omitted = matches!(syntax, AttrSyntax::Omitted);
    let attribute_value = context.attributes.and_then(|attributes| {
        attributes.get(&attribute_name).or_else(|| {
            context.attribute_names_are_ascii_case_insensitive.then(|| {
                attributes
                    .iter()
                    .find(|(name, _)| name.eq_ignore_ascii_case_utf16(&attribute_name))
                    .map(|(_, value)| value)
            })?
        })
    });
    let Some(attribute_value) = attribute_value else {
        return attr_fallback(
            store,
            registry,
            arguments,
            comma,
            syntax_was_omitted,
            context,
            recursion_depth,
        );
    };

    let resolved = match syntax {
        AttrSyntax::Omitted | AttrSyntax::RawString => parse_substitution_values(TokenizerInput::Utf16(
            &crate::css::serialize::serialize_string(attribute_value),
        ))
        .expect("serialized string tokenizes correctly"),
        AttrSyntax::Number => {
            let values = parse_substitution_values(attribute_value).unwrap_or_default();
            if !matches!(
                trim_whitespace(&values),
                [value] if matches!(value.kind, ComponentKind::Token(ParserTokenKind::Number { .. }))
            ) {
                return attr_fallback(store, registry, arguments, comma, false, context, recursion_depth);
            }
            let syntax = SyntaxNode::Type(crate::css::parser::syntax::SyntaxType::Number);
            let Some(tokens) = parse_attr_value_with_syntax(registry, &values, &syntax) else {
                return attr_fallback(store, registry, arguments, comma, false, context, recursion_depth);
            };
            tokens
        }
        AttrSyntax::Unit(unit) => {
            let values = parse_substitution_values(attribute_value).unwrap_or_default();
            if !matches!(
                trim_whitespace(&values),
                [value] if matches!(value.kind, ComponentKind::Token(ParserTokenKind::Number { .. }))
            ) {
                return attr_fallback(store, registry, arguments, comma, false, context, recursion_depth);
            }
            let number_syntax = SyntaxNode::Type(crate::css::parser::syntax::SyntaxType::Number);
            let Some(number) = parse_attr_value_with_syntax(registry, &values, &number_syntax) else {
                return attr_fallback(store, registry, arguments, comma, false, context, recursion_depth);
            };
            let mut source = serialize_tokens(&number);
            source.extend_from_slice(&unit);
            parse_substitution_values(&source).expect("serialized number and unit form a component value")
        }
        AttrSyntax::Syntax(syntax) => {
            let Some(values) = parse_substitution_values(attribute_value) else {
                return attr_fallback(store, registry, arguments, comma, false, context, recursion_depth);
            };

            if collect_arbitrary_substitution_function_presence(&values, &mut Default::default()).is_err() {
                return attr_fallback(store, registry, arguments, comma, false, context, recursion_depth);
            }

            let substituted = match substitute_arbitrary_substitution_functions(
                store,
                registry,
                &values,
                context,
                recursion_depth + 1,
                Some(SubstitutionContextDependency::Attribute(attribute_name.clone())),
            ) {
                TokenResolution::Resolved(tokens) => Some(tokens),
                TokenResolution::Invalid | TokenResolution::Cyclic => None,
                TokenResolution::NotHandled => return TokenResolution::NotHandled,
            };
            let Some(substituted) = substituted else {
                return attr_fallback(store, registry, arguments, comma, false, context, recursion_depth);
            };
            let Some(tokens) = parse_attr_value_with_syntax(registry, &substituted, &syntax) else {
                return attr_fallback(store, registry, arguments, comma, false, context, recursion_depth);
            };
            tokens
        }
    };
    context.contains_attr_tainted_values = true;
    TokenResolution::Resolved(resolved)
}

fn is_substitution_function(name: &[u16]) -> bool {
    name.starts_with_ascii("--")
        || name.eq_ignore_ascii_case("var")
        || name.eq_ignore_ascii_case("attr")
        || name.eq_ignore_ascii_case("env")
        || name.eq_ignore_ascii_case("if")
        || name.eq_ignore_ascii_case("inherit")
}

/// Whether a function's arguments are, or are made into, a URL.
fn function_takes_a_url(name: &[u16]) -> bool {
    ["url", "src", "image", "image-set", "-webkit-image-set"]
        .iter()
        .any(|url_function| name.eq_ignore_ascii_case(url_function))
}

fn component_contains_url(value: &ComponentValue) -> bool {
    match &value.kind {
        ComponentKind::Token(ParserTokenKind::Url(_)) => true,
        ComponentKind::Function { name, values } => {
            function_takes_a_url(name.as_ref()) || values.iter().any(component_contains_url)
        }
        ComponentKind::SimpleBlock { values, .. } => values.iter().any(component_contains_url),
        _ => false,
    }
}

// Step 2 of https://drafts.csswg.org/css-values-5/#substitute-arbitrary-substitution-function
fn substitute_tokens<'a>(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    values: impl Into<ComponentValueSlice<'a>>,
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    let values = values.into();
    if recursion_depth > MAX_SUBSTITUTION_RECURSION_DEPTH {
        context.substitution_limit_hits += 1;
        return TokenResolution::Invalid;
    }

    let mut output = Vec::new();
    let mut token_count = values.token_count;
    let mut is_cyclic = false;
    for value in values.iter() {
        let (name, contents) = match &value.kind {
            ComponentKind::Token(_) => {
                output.push(value.clone());
                continue;
            }
            ComponentKind::Function { name, values } => (Some(name.as_ref()), values),
            ComponentKind::SimpleBlock { values, .. } => (None, values),
        };
        token_count -= 2 + contents.token_count;
        let substitutes = name.is_some_and(is_substitution_function);

        // All ASFs should have been validated against their argument grammars at parse time.
        debug_assert!(name.is_none_or(|name| {
            arbitrary_substitution_function(name).is_none_or(|function| arguments_are_valid(function, contents))
        }));

        let tainted_before = std::mem::take(&mut context.contains_attr_tainted_values);
        let in_url_function = context.in_url_function;
        context.in_url_function |= name.is_some_and(function_takes_a_url);
        let resolved = match name {
            Some(name) if name.eq_ignore_ascii_case("var") => {
                replace_var_function(store, registry, contents, context, recursion_depth)
            }
            Some(name) if name.eq_ignore_ascii_case("attr") => {
                replace_attr_function(store, registry, contents, context, recursion_depth)
            }
            Some(name) if name.eq_ignore_ascii_case("inherit") => {
                replace_inherit_function(store, registry, contents, context, recursion_depth)
            }
            Some(name) if name.eq_ignore_ascii_case("env") => {
                replace_env_function(store, registry, contents, context, recursion_depth)
            }
            Some(name) if name.eq_ignore_ascii_case("if") => {
                replace_if_function(store, registry, contents, context, recursion_depth)
            }
            Some(name) if name.starts_with_ascii("--") => {
                replace_a_dashed_function(store, registry, name, contents, context, recursion_depth)
            }
            _ => substitute_tokens(store, registry, contents, context, recursion_depth + 1),
        };
        context.in_url_function = in_url_function;
        let tainted = context.contains_attr_tainted_values;
        context.contains_attr_tainted_values |= tainted_before;
        // https://drafts.csswg.org/css-values-5/#attr-security
        // Using an attr-tainted value as or in a <url> makes a declaration invalid at computed-value time.
        // NB: A custom function's own declarations use nothing until its result is substituted.
        if let TokenResolution::Resolved(resolved) = &resolved
            && substitutes
            && tainted
            && context.function_local_scopes.is_empty()
            && (in_url_function || resolved.iter().any(component_contains_url))
        {
            context.uses_attr_tainted_url = true;
        }
        let resolved = match resolved {
            TokenResolution::Resolved(resolved) => resolved,
            TokenResolution::Invalid => return TokenResolution::Invalid,
            TokenResolution::Cyclic => {
                is_cyclic = true;
                continue;
            }
            TokenResolution::NotHandled => return TokenResolution::NotHandled,
        };
        let resolved_start = usize::from(
            substitutes
                && output.last().is_some_and(ComponentValue::is_whitespace)
                && resolved.first().is_some_and(ComponentValue::is_whitespace),
        );
        token_count += resolved.token_count - resolved_start;
        if substitutes {
            output.extend(resolved.into_vec().into_iter().skip(resolved_start));
        } else {
            token_count += 2;
            let kind = match &value.kind {
                ComponentKind::Function { name, .. } => ComponentKind::Function {
                    name: name.clone(),
                    values: resolved.into_boxed(),
                },
                ComponentKind::SimpleBlock { opening, .. } => ComponentKind::SimpleBlock {
                    opening: opening.clone(),
                    values: resolved.into_boxed(),
                },
                ComponentKind::Token(_) => unreachable!(),
            };
            output.push(ComponentValue {
                kind,
                original_source_text: value.original_source_text.clone(),
                ..*value
            });
        }
        if token_count > MAX_SUBSTITUTED_TOKEN_COUNT {
            context.substitution_limit_hits += 1;
            return TokenResolution::Invalid;
        }
    }
    if is_cyclic {
        TokenResolution::Cyclic
    } else {
        TokenResolution::Resolved(ComponentValueList::with_token_count(output, token_count))
    }
}

// https://drafts.csswg.org/css-values-5/#substitute-arbitrary-substitution-function
fn substitute_arbitrary_substitution_functions<'a>(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    tokens: impl Into<ComponentValueSlice<'a>>,
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
    substitution_context: Option<SubstitutionContextDependency>,
) -> TokenResolution {
    let Some(dependency) = substitution_context else {
        return substitute_tokens(store, registry, tokens, context, recursion_depth);
    };

    // 1. Guard context for the remainder of this algorithm. If context is marked as a cyclic substitution context,
    //    return the guaranteed-invalid value.
    let substitution_context = SubstitutionContext::new(dependency);
    let Some(_guard) = context.guarded_contexts.guard(&substitution_context) else {
        return TokenResolution::Cyclic;
    };

    // 2. Substitute each arbitrary substitution function in values.
    let result = substitute_tokens(store, registry, tokens, context, recursion_depth);

    // 3. If context is marked as a cyclic substitution context, return the guaranteed-invalid value.
    // NOTE: Nested arbitrary substitution functions may have marked context as cyclic in step 2.
    if substitution_context.is_cyclic.get() {
        TokenResolution::Cyclic
    } else {
        result
    }
}

fn serialize_tokens(tokens: &[ComponentValue]) -> Vec<u16> {
    serialize_component_values_to_utf16(tokens, ComponentSerializationMode::Normalized)
}

/// What a substitution reads beyond the value it substitutes.
#[derive(Clone, Copy)]
pub(crate) struct VarResolutionInputs<'a> {
    pub(crate) store: Option<&'a CustomPropertyStore>,
    pub(crate) inheritance_store: Option<&'a CustomPropertyStore>,
    pub(crate) registry: Option<&'a CustomPropertyRegistry>,
    pub(crate) parse_context: Option<&'a ParseContext<'a>>,
    pub(crate) media_environment: Option<&'a FfiMediaEnvironment<'a>>,
    pub(crate) attribute_names_are_ascii_case_insensitive: bool,
    pub(crate) style_query_inputs: Option<&'a crate::css::cascaded_properties::StyleQueryInputs>,
}

/// The custom properties of the element that substituting `value`, as the custom property `name`, looks up with the
/// values `inputs` holds: what its `var()` functions and `if()` conditions name, wherever a function, a branch, a
/// fallback or an attribute's value takes that, and only where they do.
pub(crate) fn custom_properties_a_substitution_reads(
    inputs: &VarResolutionInputs,
    name: &Utf16FlyString,
    value: &UnresolvedPayload,
    environment: &mut VarResolutionEnvironment<'_>,
) -> HashSet<Utf16FlyString> {
    let VarResolutionEnvironment {
        attributes,
        custom_functions,
        token_cache,
    } = environment;
    let mut reads = HashSet::new();
    let mut context = ASFResolutionContext {
        attributes: Some(attributes),
        inheritance_store: inputs.inheritance_store,
        attribute_names_are_ascii_case_insensitive: inputs.attribute_names_are_ascii_case_insensitive,
        custom_functions: Some(custom_functions),
        parse_context: inputs.parse_context,
        media_environment: inputs.media_environment,
        style_query_length_resolution_context: inputs.style_query_inputs.map(|inputs| &inputs.length),
        style_query_color_resolution_input: inputs.style_query_inputs.map(|inputs| inputs.color_input()),
        style_query_tree_counting: inputs.style_query_inputs.and_then(|inputs| inputs.tree_counting),
        token_cache: Some(token_cache),
        element_property_reads: Some(&mut reads),
        ..Default::default()
    };
    let tokens = value.components();
    // NB: What the substitution comes to is of no interest here, only what it looked at.
    let _ = substitute_arbitrary_substitution_functions(
        inputs.store,
        inputs.registry,
        tokens.as_ref(),
        &mut context,
        0,
        Some(SubstitutionContextDependency::Property(name.clone(), None)),
    );
    drop(context);
    reads
}

pub(crate) fn resolve_vars(
    inputs: &VarResolutionInputs,
    property_id: u16,
    root_custom_property_name: &[u16],
    value_data: &StyleValueData,
    environment: &mut VarResolutionEnvironment<'_>,
    style_query_dependencies: Option<&mut Option<Box<StyleQueryDependencies>>>,
) -> NativeVarResolution {
    let VarResolutionInputs {
        store,
        inheritance_store,
        registry,
        parse_context,
        media_environment,
        attribute_names_are_ascii_case_insensitive,
        style_query_inputs,
    } = *inputs;
    let root_custom_property_name = (!root_custom_property_name.is_empty()).then(|| root_custom_property_name.to_vec());
    assert_eq!(
        root_custom_property_name.is_some(),
        property_id == crate::css::property_metadata::property_id::CUSTOM,
        "substitution root must be either a named custom property or a non-custom property",
    );
    let root_property_name = root_custom_property_name.unwrap_or_else(|| {
        crate::css::property_metadata::property_name(property_id)
            .encode_utf16()
            .collect()
    });
    let substitution_context = Some(SubstitutionContextDependency::Property(
        Utf16FlyString::from_utf16(&root_property_name),
        None,
    ));
    let VarResolutionEnvironment {
        attributes,
        custom_functions,
        token_cache,
    } = environment;
    let mut context = ASFResolutionContext {
        attributes: Some(attributes),
        inheritance_store,
        attribute_names_are_ascii_case_insensitive,
        contains_attr_tainted_values: false,
        custom_functions: Some(custom_functions),
        parse_context,
        media_environment,
        style_query_length_resolution_context: style_query_inputs.map(|inputs| &inputs.length),
        style_query_color_resolution_input: style_query_inputs.map(|inputs| inputs.color_input()),
        style_query_tree_counting: style_query_inputs.and_then(|inputs| inputs.tree_counting),
        style_query_dependencies,
        token_cache: Some(token_cache),
        ..Default::default()
    };
    let StyleValueData::Unresolved(unresolved_payload) = value_data else {
        return NativeVarResolution::NotHandled;
    };
    // A value holding no substitution function has nothing to resolve: it is invalid at
    // computed-value time, as every caller takes an unresolved value it cannot substitute.
    if !unresolved_payload.presence().has_any() {
        return NativeVarResolution::Invalid;
    }
    context.contains_attr_tainted_values = unresolved_payload.contains_attr_tainted_values;
    let tokens = cached_value_tokens(value_data, &mut context);
    let result = substitute_arbitrary_substitution_functions(
        store,
        registry,
        tokens.as_ref(),
        &mut context,
        0,
        substitution_context,
    );
    match result {
        // A custom property carries its taint to the values that substitute it; another property is invalid where a
        // tainted value reaches a URL.
        TokenResolution::Resolved(components) => NativeVarResolution::Resolved {
            components,
            contains_attr_tainted_values: if property_id == crate::css::property_metadata::property_id::CUSTOM {
                context.contains_attr_tainted_values
            } else {
                context.uses_attr_tainted_url
            },
        },
        TokenResolution::Invalid | TokenResolution::Cyclic => NativeVarResolution::Invalid,
        TokenResolution::NotHandled => NativeVarResolution::NotHandled,
    }
}

#[cfg(test)]
#[allow(clippy::items_after_test_module)]
mod tests {
    use super::*;
    use crate::css::parser::syntax::parse_with_syntax;

    #[test]
    fn style_queries_only_report_sibling_dependencies_for_tree_counting() {
        let registry = CustomPropertyRegistry::empty();
        let random_index = RandomFunctionIndex::default();
        let parse_context = registry.parse_context(&random_index);
        let length = crate::css::style_compute::FfiLengthResolutionContext::default();
        for (source, expected, reads_sibling_position) in [
            ("10px", 10.0, false),
            ("calc(2 * 10px)", 20.0, false),
            ("calc(10px * sibling-index())", 10.0, true),
            ("calc(10px * sibling-count())", 20.0, true),
        ] {
            let value =
                parse_with_syntax(&parse_context, &utf16(source), &SyntaxNode::Type(SyntaxType::Length)).unwrap();
            let mut dependencies = None;
            let mut value_reads = RegisteredValueReads::default();
            let mut context = ASFResolutionContext {
                style_query_length_resolution_context: Some(&length),
                style_query_tree_counting: Some((2, 1)),
                style_query_dependencies: Some(&mut dependencies),
                registered_value_reads: Some(&mut value_reads),
                ..Default::default()
            };
            assert_eq!(StyleRangeValue::of(&value, &mut context).unwrap().value, expected);
            assert_eq!(value_reads.sibling_position, reads_sibling_position, "{source}");
            assert_eq!(
                dependencies
                    .as_ref()
                    .is_some_and(|dependencies| dependencies.sibling_position),
                reads_sibling_position,
                "{source}",
            );
        }
    }

    fn store(
        parent: Option<Arc<CustomPropertyStore>>,
        names: impl IntoIterator<Item = u32>,
        value: f64,
    ) -> Arc<CustomPropertyStore> {
        let entries = names
            .into_iter()
            .map(|index| {
                let name = Utf16FlyString::from_utf16(&utf16(&format!("--name-{index}")));
                let entry = CustomPropertyEntry {
                    name: name.clone(),
                    value: RetainedStyleValueData::from_owned(StyleValueData::Number { value }),
                    important: false,
                    declared: None,
                };
                (name.raw_identity(), entry)
            })
            .collect();
        CustomPropertyStore::child(parent, entries)
    }

    #[test]
    fn stores_built_apart_hold_the_same_values_where_every_name_reads_the_same() {
        // NB: A parent of more names than a child takes in stays a store of the child's chain.
        let child_over = |value| store(Some(store(None, 0..20, value)), 20..22, 2.0);
        let (first, second) = (child_over(1.0), child_over(1.0));
        assert!(!Arc::ptr_eq(first.parent().unwrap(), second.parent().unwrap()));
        assert!(first.holds_same_values_as(&second));
        assert!(!first.holds_same_values_as(&child_over(3.0)));
        // NB: A name only the other chain holds differs too, whichever store is asked.
        let wider = store(Some(store(None, 0..21, 1.0)), 21..22, 2.0);
        assert!(!first.holds_same_values_as(&wider));
        assert!(!wider.holds_same_values_as(&first));
    }

    #[test]
    fn stores_declaring_the_same_names_in_another_order_do_not_hold_the_same_values() {
        let (first, second) = (store(None, [0, 1], 1.0), store(None, [1, 0], 1.0));
        assert!(!first.holds_same_values_as(&second));
        assert!(first.holds_same_values_as(&store(None, [0, 1], 1.0)));
    }

    #[test]
    fn stores_compared_again_answer_as_they_did_whichever_is_asked() {
        // NB: Turning every answer kept around shows that the next one is the one kept, and not a walk of the values.
        fn turn_kept_answers() {
            COMPARED_STORES.with_borrow_mut(|compared| {
                for compared in compared.iter_mut().flatten() {
                    compared.same = !compared.same;
                }
            });
        }
        let (first, second, other) = (
            store(None, 0..20, 1.0),
            store(None, 0..20, 1.0),
            store(None, 0..20, 2.0),
        );
        assert!(CustomPropertyStore::hold_the_same_values(&first, &first));

        // NB: Two pairs can share a slot, so each pair is asked about again before the next is compared.
        assert!(CustomPropertyStore::hold_the_same_values(&first, &second));
        turn_kept_answers();
        assert!(!CustomPropertyStore::hold_the_same_values(&second, &first));
        assert!(first.holds_same_values_as(&second));

        assert!(!CustomPropertyStore::hold_the_same_values(&first, &other));
        turn_kept_answers();
        assert!(CustomPropertyStore::hold_the_same_values(&other, &first));
        assert!(!first.holds_same_values_as(&other));
    }

    #[test]
    fn a_kept_answer_keeps_no_store_alive() {
        let (first, second) = (store(None, 0..1, 1.0), store(None, 0..1, 1.0));
        assert!(CustomPropertyStore::hold_the_same_values(&first, &second));
        let weak = Arc::downgrade(&first);
        drop(first);
        assert!(weak.upgrade().is_none());
    }

    #[test]
    fn custom_property_values_outlive_their_store() {
        let name = Utf16FlyString::default();
        let name_raw = name.raw_identity();
        let store = CustomPropertyStore::child(
            None,
            vec![(
                name_raw,
                CustomPropertyEntry {
                    name,
                    value: RetainedStyleValueData::from_owned(StyleValueData::Number { value: 1.0 }),
                    important: false,
                    declared: None,
                },
            )],
        );
        let value = store.get(name_raw).unwrap().value.clone();
        drop(store);
        assert!(matches!(value.data(), StyleValueData::Number { value } if *value == 1.0));
    }

    fn utf16(value: &str) -> Vec<u16> {
        value.encode_utf16().collect()
    }

    fn substitute_without_custom_properties(source: &str) -> TokenResolution {
        let Some(values) = parse_substitution_values(source.as_bytes()) else {
            return TokenResolution::Invalid;
        };
        substitute_tokens(None, None, &values, &mut ASFResolutionContext::default(), 0)
    }

    fn substitute_with_attributes(source: &str, attributes: &[(&str, &str)]) -> (TokenResolution, bool) {
        let attributes: HashMap<_, _> = attributes
            .iter()
            .map(|(name, value)| (utf16(name), utf16(value)))
            .collect();
        let mut context = ASFResolutionContext {
            attributes: Some(&attributes),
            ..Default::default()
        };
        let result = substitute_tokens(
            None,
            None,
            &parse_substitution_values(source.as_bytes()).unwrap(),
            &mut context,
            0,
        );
        (result, context.contains_attr_tainted_values)
    }

    #[test]
    fn missing_variable_uses_fallback() {
        let TokenResolution::Resolved(tokens) = substitute_without_custom_properties("calc(var(--missing, 1px) + 2px)")
        else {
            panic!("expected fallback substitution");
        };
        assert_eq!(serialize_tokens(&tokens), utf16("calc( 1px + 2px)"));
    }

    #[test]
    fn empty_fallback_is_valid() {
        let TokenResolution::Resolved(tokens) = substitute_without_custom_properties("var(--missing,)") else {
            panic!("expected empty fallback substitution");
        };
        assert!(tokens.is_empty());
    }

    #[test]
    fn missing_variable_without_fallback_is_invalid() {
        assert!(matches!(
            substitute_without_custom_properties("var(--missing)"),
            TokenResolution::Invalid
        ));
    }

    #[test]
    fn root_custom_property_cycles_do_not_take_var_fallbacks() {
        let mut context = ASFResolutionContext::default();
        assert!(matches!(
            substitute_arbitrary_substitution_functions(
                None,
                None,
                &parse_substitution_values(b"var(--root, fallback)").unwrap(),
                &mut context,
                0,
                Some(SubstitutionContextDependency::Property(
                    Utf16FlyString::from_utf16(&utf16("--root")),
                    None
                )),
            ),
            TokenResolution::Cyclic
        ));
    }

    #[test]
    fn substitution_context_cycle_marks_the_entire_mixed_context_suffix() {
        let guarded_contexts = GuardedSubstitutionContexts::default();
        let guard = |dependency| {
            let context = SubstitutionContext::new(dependency);
            let guard = guarded_contexts.guard(&context).unwrap();
            (context, guard)
        };
        let (root_context, _root_guard) = guard(SubstitutionContextDependency::Property(
            Utf16FlyString::from_utf16(&utf16("--root")),
            None,
        ));
        let (attribute_context, _attribute_guard) =
            guard(SubstitutionContextDependency::Attribute(utf16("data-value")));
        let (function_context, _function_guard) = guard(SubstitutionContextDependency::Function(1));

        let duplicate = SubstitutionContext::new(SubstitutionContextDependency::Attribute(utf16("data-value")));
        assert!(guarded_contexts.guard(&duplicate).is_none());
        assert!(!root_context.is_cyclic.get());
        assert!(attribute_context.is_cyclic.get());
        assert!(function_context.is_cyclic.get());
        assert!(duplicate.is_cyclic.get());
    }

    #[test]
    fn substitutes_environment_values_and_fallbacks() {
        let TokenResolution::Resolved(tokens) = substitute_without_custom_properties("env(safe-area-inset-top)") else {
            panic!("expected safe-area environment substitution");
        };
        assert_eq!(serialize_tokens(&tokens), utf16("0px"));

        let TokenResolution::Resolved(tokens) =
            substitute_without_custom_properties("env(unknown-environment-variable, 4px)")
        else {
            panic!("expected environment fallback");
        };
        assert_eq!(serialize_tokens(&tokens), utf16(" 4px"));

        assert!(matches!(
            substitute_without_custom_properties("env(\"unknown-environment-variable\", 4px)"),
            TokenResolution::Invalid
        ));
        assert!(matches!(
            substitute_without_custom_properties("env(safe-area-inset-top 1.5, 4px)"),
            TokenResolution::Invalid
        ));
    }

    #[test]
    fn substitutes_unconditional_if_branches() {
        let TokenResolution::Resolved(tokens) = substitute_without_custom_properties("if(else: 5px)") else {
            panic!("expected unconditional branch substitution");
        };
        assert_eq!(serialize_tokens(&tokens), utf16(" 5px"));
    }

    #[test]
    fn substitutes_custom_functions_from_a_snapshot() {
        let definitions =
            crate::css::rule::compile_functions_for_testing("@function --echo(--value) { result: var(--value); }");
        let function = &definitions[0];
        let functions = CustomFunctionRegistry {
            caller_scope_identity: 1,
            definitions: vec![PreparedCustomFunction {
                identity: function.identity,
                scope_identity: 1,
                signature: &function.signature,
                parameter_defaults: &function.parameter_defaults,
                declarations: function.inputs.iter().flat_map(|input| &input.declarations).collect(),
            }],
            visible: &[CustomFunctionVisibility {
                caller_scope_identity: 1,
                definition_index: 0,
            }],
        };
        let mut context = ASFResolutionContext {
            custom_functions: Some(&functions),
            ..Default::default()
        };
        let TokenResolution::Resolved(tokens) = substitute_tokens(
            None,
            None,
            &parse_substitution_values(b"--echo(12px)").unwrap(),
            &mut context,
            0,
        ) else {
            panic!("expected custom function substitution");
        };
        assert_eq!(serialize_tokens(&tokens), utf16("12px"));
    }

    #[test]
    fn a_nested_call_names_the_function_its_caller_s_scope_sees() {
        // The document (scope 1) defines --inner, a shadow tree (scope 2) defines --outer and an
        // --inner of its own. A call from the document reaches --outer, whose body calls --inner
        // as the shadow tree names it.
        let definitions = crate::css::rule::compile_functions_for_testing(
            "@function --outer() { result: --inner(); }
            @function --inner() { result: 2px; }
            @function --inner() { result: 1px; }",
        );
        let functions = CustomFunctionRegistry {
            caller_scope_identity: 1,
            definitions: definitions
                .iter()
                .zip([2, 2, 1])
                .map(|(function, scope_identity)| PreparedCustomFunction {
                    identity: function.identity,
                    scope_identity,
                    signature: &function.signature,
                    parameter_defaults: &function.parameter_defaults,
                    declarations: function.inputs.iter().flat_map(|input| &input.declarations).collect(),
                })
                .collect(),
            visible: &[
                CustomFunctionVisibility {
                    caller_scope_identity: 1,
                    definition_index: 0,
                },
                CustomFunctionVisibility {
                    caller_scope_identity: 1,
                    definition_index: 2,
                },
                CustomFunctionVisibility {
                    caller_scope_identity: 2,
                    definition_index: 0,
                },
                CustomFunctionVisibility {
                    caller_scope_identity: 2,
                    definition_index: 1,
                },
            ],
        };
        let substitute = |source: &[u8]| {
            let mut context = ASFResolutionContext {
                custom_functions: Some(&functions),
                ..Default::default()
            };
            let TokenResolution::Resolved(tokens) =
                substitute_tokens(None, None, &parse_substitution_values(source).unwrap(), &mut context, 0)
            else {
                panic!("expected custom function substitution");
            };
            serialize_tokens(&tokens)
        };
        assert_eq!(substitute(b"--outer()"), utf16("2px"));
        assert_eq!(substitute(b"--inner()"), utf16("1px"));
    }

    #[test]
    fn serialization_preserves_token_boundaries() {
        let TokenResolution::Resolved(tokens) = substitute_without_custom_properties("var(--missing, 1)px") else {
            panic!("expected fallback substitution");
        };
        assert_eq!(serialize_tokens(&tokens), utf16(" 1/**/px"));
    }

    #[test]
    fn recognizes_substituted_css_wide_keywords() {
        assert!(single_css_wide_keyword(&parse_substitution_values(b"  inherit ").unwrap()).is_some());
        assert!(single_css_wide_keyword(&parse_substitution_values(b"inherit green").unwrap()).is_none());
    }

    #[test]
    fn substitutes_raw_attributes_and_marks_taint() {
        let (TokenResolution::Resolved(tokens), contains_attr_tainted_values) =
            substitute_with_attributes("attr(data-value)", &[("data-value", "hello")])
        else {
            panic!("expected attribute substitution");
        };
        assert_eq!(serialize_tokens(&tokens), utf16("\"hello\""));
        assert!(contains_attr_tainted_values);
    }

    #[test]
    fn missing_untyped_attribute_becomes_an_empty_string() {
        let (TokenResolution::Resolved(tokens), contains_attr_tainted_values) =
            substitute_with_attributes("attr(data-value)", &[])
        else {
            panic!("expected missing-attribute substitution");
        };
        assert_eq!(serialize_tokens(&tokens), utf16("\"\""));
        assert!(!contains_attr_tainted_values);
    }

    #[test]
    fn accepts_all_dimension_units_and_rejects_unknown_units() {
        let Ok((_, AttrSyntax::Unit(unit))) = parse_attr_syntax(&parse_substitution_values(b"data-value FR").unwrap())
        else {
            panic!("expected flex unit syntax");
        };
        assert_eq!(unit, utf16("FR"));
        assert!(parse_attr_syntax(&parse_substitution_values(b"data-value unknown-unit").unwrap()).is_err());
    }

    #[test]
    fn html_attribute_names_are_ascii_case_insensitive() {
        let attributes = HashMap::from([(utf16("data-value"), utf16("hello"))]);
        let mut context = ASFResolutionContext {
            attributes: Some(&attributes),
            attribute_names_are_ascii_case_insensitive: true,
            ..Default::default()
        };
        let TokenResolution::Resolved(tokens) = substitute_tokens(
            None,
            None,
            &parse_substitution_values(b"attr(DATA-VALUE)").unwrap(),
            &mut context,
            0,
        ) else {
            panic!("expected case-insensitive attribute substitution");
        };
        assert_eq!(serialize_tokens(&tokens), utf16("\"hello\""));
    }

    #[test]
    fn substitution_updates_list_token_counts() {
        let TokenResolution::Resolved(values) =
            substitute_without_custom_properties("outer([var(--missing, inner(x))])")
        else {
            panic!("expected fallback substitution");
        };
        assert_eq!(values.token_count, 8);
        assert_eq!(values[0].token_count(), 8);
        let (_, children) = values[0].function().unwrap();
        assert_eq!(children[0].token_count(), 6);
    }

    #[test]
    fn substitution_limit_counts_tokens_inside_components() {
        // The contents reach the token limit; the block's delimiters put it over.
        let source = format!("[{}]", "a ".repeat(MAX_SUBSTITUTED_TOKEN_COUNT / 2));
        assert!(matches!(
            substitute_without_custom_properties(&source),
            TokenResolution::Invalid
        ));
    }

    #[test]
    fn substitution_limit_preserves_intermediate_counts() {
        let limit = "a ".repeat(MAX_SUBSTITUTED_TOKEN_COUNT / 2);
        for (source, valid) in [
            (format!("var(--missing,{limit})"), true),
            (format!("var(--missing,{limit})b"), false),
            (format!("var(--missing,{limit})var(--missing,)"), false),
            (
                format!(" var(--missing, {}a)", "a ".repeat(MAX_SUBSTITUTED_TOKEN_COUNT / 2 - 1)),
                true,
            ),
        ] {
            assert_eq!(
                matches!(
                    substitute_without_custom_properties(&source),
                    TokenResolution::Resolved(_)
                ),
                valid
            );
        }
    }

    #[test]
    fn deeply_nested_functions_are_invalid() {
        let nesting = MAX_SUBSTITUTION_RECURSION_DEPTH + 1;
        let source = format!(
            "{}var(--missing, 1px){}",
            "calc(".repeat(nesting as usize),
            ")".repeat(nesting as usize)
        );
        assert!(matches!(
            substitute_without_custom_properties(&source),
            TokenResolution::Invalid
        ));
    }

    fn build_custom_property_store(
        parse_context: &ParseContext,
        properties: &[(&str, &str)],
    ) -> Arc<CustomPropertyStore> {
        let entries = properties
            .iter()
            .map(|(name, source)| {
                let name = Utf16FlyString::from_utf16(&utf16(name));
                let source = utf16(source);
                let values = consume_a_counted_list_of_component_values(tokenize_for_parser(&source)).unwrap();
                let crate::css::parser::value_parser::ParseOutcome::Parsed(value) =
                    crate::css::parser::value_parser::parse_css_value_with_source(
                        parse_context,
                        crate::css::property_metadata::property_id::CUSTOM,
                        &values,
                        &source,
                    )
                else {
                    panic!("custom property value should parse");
                };
                (
                    name.raw_identity(),
                    CustomPropertyEntry {
                        name,
                        value: RetainedStyleValueData::from_arc(value),
                        important: false,
                        declared: None,
                    },
                )
            })
            .collect();

        CustomPropertyStore::child(None, entries)
    }

    #[test]
    fn element_custom_properties_are_computed_once_for_repeated_references() {
        let registry = CustomPropertyRegistry::empty();

        let random_index = RandomFunctionIndex::default();
        let parse_context = registry.parse_context(&random_index);

        // Put the consumer first so --shared is computed on demand. References inside if()
        // were not visible to the dependency ordering used before completed values were cached.
        let definitions = [
            ("--consumer", "if(else: var(--shared) var(--shared))"),
            ("--shared", "if(else: 1px)"),
        ];

        let store = build_custom_property_store(&parse_context, &definitions);

        let inputs = VarResolutionInputs {
            store: Some(&store),
            inheritance_store: None,
            registry: Some(&registry),
            parse_context: Some(&parse_context),
            media_environment: None,
            attribute_names_are_ascii_case_insensitive: false,
            style_query_inputs: None,
        };

        let functions = CustomFunctionRegistry::empty();
        let mut environment = prepare_var_resolution_environment(&[], &functions);

        CUSTOM_PROPERTY_COMPUTATIONS.with(|count| count.set(0));

        let (values, _) =
            resolve_element_custom_properties(&inputs, &mut environment, None, CustomPropertyFinalization::default());

        let sources: Vec<_> = values
            .iter()
            .map(|value| {
                String::from_utf16(&crate::css::serialize::serialize_resolved_style_value_to_utf16(
                    value.data(),
                ))
                .unwrap()
                .trim()
                .to_owned()
            })
            .collect();
        assert_eq!(sources, ["1px 1px", "1px"]);
        CUSTOM_PROPERTY_COMPUTATIONS.with(|count| {
            assert_eq!(count.get(), 2, "each declaration should be computed only once");
        });
    }
}

/// A document's registry of custom properties, which the document holds a reference to: a style
/// transaction that read it holds one of its own, so a change makes the registry anew rather than
/// writing the one a transaction may still read.
#[unsafe(no_mangle)]
pub extern "C" fn rust_custom_property_registry_create() -> *const c_void {
    Arc::into_raw(Arc::new(CustomPropertyRegistry::empty())).cast()
}

/// Makes the registry of one document anew with its effective registered custom properties,
/// gives up the document's reference to `registry`, the one it replaces, and answers the new one.
///
/// # Safety
/// `registry` must be the document's live registry from `rust_custom_property_registry_create`
/// or this function, which it gives up, and `registrations` must point at `registration_count`
/// valid entries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_registry_update(
    registry: *const c_void,
    context: *const FfiCustomPropertyRegistryContext,
    registrations: *const FfiCustomPropertyRegistration,
    registration_count: usize,
) -> *const c_void {
    let Some(context) = (unsafe { context.as_ref() }) else {
        return registry;
    };
    let registrations = if registration_count == 0 {
        &[]
    } else {
        unsafe { ak::slice_from_raw_parts(registrations, registration_count) }
    };
    // SAFETY: Guaranteed by the caller.
    drop(unsafe { Arc::from_raw(registry.cast::<CustomPropertyRegistry>()) });
    let mut registry = CustomPropertyRegistry::empty();
    registry.document_url = unsafe { crate::bytes_from_raw(context.document_url, context.document_url_length) }
        .unwrap_or_default()
        .to_vec();
    registry.document_base_url =
        unsafe { crate::bytes_from_raw(context.document_base_url, context.document_base_url_length) }
            .unwrap_or_default()
            .to_vec();
    registry.registrations.reserve(registrations.len());
    for registration in registrations {
        let name = registration
            .name
            .to_utf16()
            .expect("invalid registered custom property name");
        let initial_source = if registration.has_initial_value {
            let units = registration
                .initial_value
                .to_utf16()
                .expect("invalid registered custom property initial value");
            Some(RetainedString::from_utf16(&units).expect("invalid registered custom property initial value"))
        } else {
            None
        };
        if registration.syntax.is_null() {
            continue;
        }
        // SAFETY: The host lends its counted reference to the parsed syntax for the call.
        let syntax = unsafe {
            let syntax = registration.syntax.cast::<SyntaxNode>();
            Arc::increment_strong_count(syntax);
            Arc::from_raw(syntax)
        };
        // SAFETY: The host lends a live style value for the call; the registry keeps a reference of
        //         its own for as long as it holds the registration.
        let computed_initial = unsafe {
            RetainedStyleValueData::from_retained_pointer(crate::css::style_value::retain_style_value(
                registration.computed_initial_value.cast(),
            ))
        };
        registry.registrations.insert(
            Utf16FlyString::from_utf16(&name),
            RegisteredCustomProperty {
                syntax,
                inherits: registration.inherits,
                initial_source,
                computed_initial,
            },
        );
    }
    Arc::into_raw(Arc::new(registry)).cast()
}

/// Gives up the document's reference to its registry.
#[unsafe(no_mangle)]
pub extern "C" fn rust_custom_property_registry_destroy(registry: OwnedArc<CustomPropertyRegistry>) {
    drop(registry);
}

/// Takes a reference of its own to the document registry `registry`, for a style update that
/// reads it beside the document, which may make the document's registry anew meanwhile.
///
/// # Safety
/// `registry` must be null or a document's live registry.
pub(crate) unsafe fn retain_custom_property_registry(registry: *const c_void) -> Option<Arc<CustomPropertyRegistry>> {
    let registry = registry.cast::<CustomPropertyRegistry>();
    if registry.is_null() {
        return None;
    }
    // SAFETY: Guaranteed by the caller.
    unsafe {
        Arc::increment_strong_count(registry);
        Some(Arc::from_raw(registry))
    }
}

/// Creates one Rust store node. Each entry transfers a leaked fly-string reference and a
/// strong style value data handle. The structural and inheritance parents are other Arc raw
/// pointers; they can differ when the C++ store has compacted its structural chain.
///
/// # Safety
/// `entries` must point at `entry_count` valid entries. Both parent pointers must be null or
/// pointers returned by this function that remain live for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_create(
    entries: *const FfiCustomPropertyStoreEntry,
    entry_count: usize,
    declared_count: usize,
    parent: *const c_void,
    inheritance_parent: *const c_void,
) -> *const c_void {
    let entries = if entry_count == 0 {
        &[]
    } else {
        unsafe { ak::slice_from_raw_parts(entries, entry_count) }
    };
    assert!(declared_count <= entries.len());
    let parent = if parent.is_null() {
        None
    } else {
        let parent = parent.cast::<CustomPropertyStore>();
        unsafe { Arc::increment_strong_count(parent) };
        Some(unsafe { Arc::from_raw(parent) })
    };
    let inheritance_parent = if inheritance_parent.is_null() {
        None
    } else {
        let inheritance_parent = inheritance_parent.cast::<CustomPropertyStore>();
        unsafe { Arc::increment_strong_count(inheritance_parent) };
        Some(unsafe { Arc::from_raw(inheritance_parent) })
    };
    let entry_of = |entry: &FfiCustomPropertyStoreEntry| {
        (
            entry.name_raw,
            CustomPropertyEntry {
                name: unsafe { Utf16FlyString::from_raw_owned(entry.name_raw) },
                value: unsafe { RetainedStyleValueData::from_retained_pointer(entry.data.cast()) },
                important: entry.important,
                declared: None,
            },
        )
    };
    let (declared, taken_in) = entries.split_at(declared_count);
    let mut own_values = CustomPropertyEntriesBuilder::default();
    own_values.declare(declared.iter().map(entry_of));
    for (name_raw, entry) in taken_in.iter().map(entry_of) {
        own_values.insert(name_raw, entry);
    }
    Arc::into_raw(Arc::new(CustomPropertyStore {
        own_values: own_values.finished(),
        ancestor_count: parent.as_ref().map_or(0, |parent| parent.ancestor_count + 1),
        parent,
        inheritance_parent,
        animation_overlay_base: None,
    }))
    .cast()
}

/// Creates a store holding an element's animated custom property values on top of the element's
/// own environment store. Transfers one strong reference per entry value and returns one strong
/// store reference.
///
/// # Safety
/// `entries` must point to `entry_count` valid entries whose `data` pointers are retained
/// style value references. `base` must be null or a live store reference.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_create_animation_overlay(
    entries: *const FfiCustomPropertyStoreEntry,
    entry_count: usize,
    base: *const c_void,
) -> *const c_void {
    let entries = if entry_count == 0 {
        &[]
    } else {
        unsafe { ak::slice_from_raw_parts(entries, entry_count) }
    };
    let base = (!base.is_null()).then(|| {
        let base = base.cast::<CustomPropertyStore>();
        // SAFETY: Guaranteed by the caller.
        unsafe {
            Arc::increment_strong_count(base);
            Arc::from_raw(base)
        }
    });
    let animated = entries.iter().map(|entry| {
        (
            entry.name_raw,
            CustomPropertyEntry {
                name: unsafe { Utf16FlyString::from_raw_owned(entry.name_raw) },
                value: unsafe { RetainedStyleValueData::from_retained_pointer(entry.data.cast()) },
                important: entry.important,
                declared: None,
            },
        )
    });
    Arc::into_raw(Arc::new(CustomPropertyStore::animation_overlay(animated, base))).cast()
}

/// Releases one store reference returned by `rust_custom_property_store_create`.
#[unsafe(no_mangle)]
pub extern "C" fn rust_custom_property_store_destroy(store: OwnedArc<CustomPropertyStore>) {
    drop(store);
}

/// Takes another reference to `store`, which the host holds as an environment.
///
/// # Safety
/// `store` must be a live store pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_retain(store: *const c_void) {
    // SAFETY: Guaranteed by the caller.
    unsafe { Arc::increment_strong_count(store.cast::<CustomPropertyStore>()) };
}

/// How many references to `store` there are.
///
/// # Safety
/// `store` must be a live store pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_reference_count(store: *const c_void) -> usize {
    // SAFETY: Guaranteed by the caller: the store lives in an Arc another reference keeps.
    let store = std::mem::ManuallyDrop::new(unsafe { Arc::from_raw(store.cast::<CustomPropertyStore>()) });
    Arc::strong_count(&store)
}

/// How many stores `store` lies over, as its chain counts them.
///
/// # Safety
/// `store` must be a live store pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_ancestor_count(store: *const c_void) -> u8 {
    unsafe { &*store.cast::<CustomPropertyStore>() }.ancestor_count
}

/// The store `store` lies over in its chain, or, for the values an element's animations sample,
/// the store of the environment they lie over; borrowed, or null.
///
/// # Safety
/// `store` must be a live store pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_chain_parent(store: *const c_void) -> *const c_void {
    let store = unsafe { &*store.cast::<CustomPropertyStore>() };
    store
        .animation_overlay_base
        .as_ref()
        .or(store.parent.as_ref())
        .map_or(std::ptr::null(), |parent| Arc::as_ptr(parent).cast())
}

/// The value `name_raw` takes in `store`, or in a store it lies over, borrowed, with whether it
/// is important in `important`; null where none of them holds a value of it.
///
/// # Safety
/// `store` must be a live store pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_get(
    store: *const c_void,
    name_raw: usize,
    important: &mut bool,
) -> *const c_void {
    let store = unsafe { &*store.cast::<CustomPropertyStore>() };
    match store.get(name_raw) {
        Some(entry) => {
            *important = entry.important;
            entry.value.pointer().cast()
        }
        None => std::ptr::null(),
    }
}

/// Hands every custom property `store` holds a value of, its own or one of a store it lies over,
/// to `callback`, each name once with the value the nearest store holds.
///
/// # Safety
/// As for [`rust_custom_property_store_for_each_own_entry`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_for_each_entry(
    store: *const c_void,
    context: *mut c_void,
    callback: unsafe extern "C" fn(*mut c_void, usize, bool, *const c_void),
) {
    let mut seen = crate::fast_hash::FastSet::default();
    let mut current = Some(unsafe { &*store.cast::<CustomPropertyStore>() });
    while let Some(store) = current {
        for (name_raw, entry) in store.own_values.iter() {
            if seen.insert(name_raw) {
                unsafe { callback(context, name_raw, entry.important, entry.value.pointer().cast()) };
            }
        }
        current = store.parent.as_deref();
    }
}

/// Hands every custom property `store` holds a value of itself to `callback`: the ones it
/// declares in declaration order, then the ones it took in from the stores it lies over.
///
/// # Safety
/// As for [`rust_custom_property_store_for_each_own_entry`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_for_each_own_value(
    store: *const c_void,
    context: *mut c_void,
    callback: unsafe extern "C" fn(*mut c_void, usize, bool, *const c_void),
) {
    let store = unsafe { &*store.cast::<CustomPropertyStore>() };
    unsafe { rust_custom_property_store_for_each_own_entry(std::ptr::from_ref(store).cast(), context, callback) };
    for (name_raw, entry) in store.own_values.iter() {
        if entry.declared.is_none() {
            unsafe { callback(context, name_raw, entry.important, entry.value.pointer().cast()) };
        }
    }
}

/// How many custom properties `store` declares itself.
///
/// # Safety
/// `store` must be a live store pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_declared_count(store: *const c_void) -> usize {
    unsafe { &*store.cast::<CustomPropertyStore>() }.declared_count()
}

/// How many custom properties `store` holds a value of itself.
///
/// # Safety
/// `store` must be a live store pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_own_count(store: *const c_void) -> usize {
    unsafe { &*store.cast::<CustomPropertyStore>() }.own_values.len()
}

/// Whether neither `store` nor any store it lies over holds a value.
///
/// # Safety
/// `store` must be a live store pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_is_empty(store: *const c_void) -> bool {
    let mut current = Some(unsafe { &*store.cast::<CustomPropertyStore>() });
    while let Some(store) = current {
        if !store.own_values.is_empty() {
            return false;
        }
        current = store.parent.as_deref();
    }
    true
}

/// Whether `store` declares `name_raw` itself with a value equal to `data`.
///
/// # Safety
/// `store` must be a live store pointer and `data` a live style value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_declares_value(
    store: *const c_void,
    name_raw: usize,
    data: *const c_void,
) -> bool {
    let store = unsafe { &*store.cast::<CustomPropertyStore>() };
    let data = unsafe { &*data.cast::<StyleValueData>() };
    store
        .own_values
        .get(&name_raw)
        .is_some_and(|entry| entry.declared.is_some() && entry.value.data() == data)
}

/// Hands every custom property a store declares itself to `callback`, in declaration order, with
/// the fly string it is named by and a borrowed value.
///
/// # Safety
/// `store` must be a live store pointer, and `callback` must not retain the value past the call
/// without retaining it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_for_each_own_entry(
    store: *const c_void,
    context: *mut c_void,
    callback: unsafe extern "C" fn(*mut c_void, usize, bool, *const c_void),
) {
    let store = unsafe { &*store.cast::<CustomPropertyStore>() };
    for name_raw in store.declared_names() {
        let entry = &store.own_values[&name_raw];
        unsafe {
            callback(context, name_raw, entry.important, entry.value.pointer().cast());
        }
    }
}
