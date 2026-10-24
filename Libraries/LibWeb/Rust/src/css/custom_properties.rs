/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Rust-owned custom-property data with the same structurally shared parent shape as the C++
//! `CustomPropertyData` shell.

use std::borrow::Cow;
use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::c_void;
use std::rc::Rc;
use std::sync::Arc;

use ak::ScopeGuard;

use crate::css::calc::CanonicalNumericType;
use crate::css::css_tokenizer::{CssNumberType, ParserTokenKind, TokenizerInput, tokenize_for_parser};
use crate::css::ffi_support::{FfiUtf16View, OwnedArc};
use crate::css::function_signature::FunctionSignature;
use crate::css::parser::arbitrary_substitution::{
    arbitrary_substitution_function, arguments_are_valid, collect_arbitrary_substitution_function_presence,
};
use crate::css::parser::component_value::{
    ComponentKind, ComponentSerializationMode, ComponentValue, ComponentValueList, ComponentValueSlice,
    consume_a_counted_list_of_component_values, trim_whitespace,
};
use crate::css::parser::query_parser::{
    FeatureComparison, FfiMediaEnvironment, MatchResult, StyleFeature, StyleRangeValue as ParsedStyleRangeValue,
    parse_and_evaluate_media_if_condition, parse_and_evaluate_supports_if_condition, parse_style_range,
};
use crate::css::parser::syntax::{
    SyntaxNode, SyntaxType, clone_syntax_handle, parse_syntax_values, parse_with_syntax_component_values,
};
use crate::css::parser::value_parser::{
    FfiValueParsingContext, FfiValueParsingContextKind, ParseContext, RandomFunctionIndex,
};
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::serialize::serialize_component_values_to_utf16;
use crate::css::style_value::RetainedStyleValueData;
use crate::css::style_value::StyleValueData;

include!(concat!(env!("OUT_DIR"), "/environment_variables_generated.rs"));

// NB: Mirrors the lookup that Meta/Generators/generate_libweb_css_environment_variables.py
//     emitted before this series removed that generator.
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
pub struct FfiCustomPropertyStoreEntry<'a> {
    pub name_raw: usize,
    pub name: FfiUtf16View<'a>,
    pub important: bool,
    pub data: *const c_void,
}

#[derive(Clone)]
pub(crate) struct CustomPropertyEntry {
    _name: RetainedUtf16FlyString,
    pub(crate) name: Arc<[u16]>,
    pub(crate) value: RetainedStyleValueData,
    pub(crate) important: bool,
}

pub struct CustomPropertyStore {
    pub(crate) own_values: HashMap<usize, CustomPropertyEntry>,
    pub(crate) declared_names: Vec<usize>,
    own_names: HashMap<Arc<[u16]>, usize>,
    parent: Option<Arc<CustomPropertyStore>>,
    inheritance_parent: Option<Arc<CustomPropertyStore>>,
    ancestor_count: u8,
}

// SAFETY: Store nodes and their entries are immutable after construction. Style workers only
// borrow the graph while resolving substitution text; the C++-owned raw Arc reference remains
// alive until every worker in the blocking batch has joined, so destruction stays on the main
// thread. The retained style values and names are likewise only read by workers.
unsafe impl Send for CustomPropertyStore {}
unsafe impl Sync for CustomPropertyStore {}

pub struct CustomPropertyRegistry {
    registrations: HashMap<Vec<u16>, RegisteredCustomProperty>,
    document_url: Vec<u8>,
    document_base_url: Vec<u8>,
    /// What a value parses to with a registration's syntax, by registration and value identity,
    /// keeping the value alive so its identity is not reused: one declared value is parsed for
    /// what its computation reads, then again to finalize it, for every element declaring it.
    parses: std::sync::Mutex<HashMap<(usize, usize), RegisteredValueParse>>,
}

/// A value the registry has parsed, kept alive, and what it parsed to.
type RegisteredValueParse = (RetainedStyleValueData, Option<RetainedStyleValueData>);

/// How many parses the registry keeps before it starts over.
const MAX_REGISTERED_VALUE_PARSES: usize = 4096;

#[derive(PartialEq)]
struct RegisteredCustomProperty {
    syntax: SyntaxNode,
    inherits: bool,
    initial_source: Option<Vec<u16>>,
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
    resolved_value_cache: Rc<RefCell<HashMap<Vec<u16>, ComponentValueList>>>,
    values: HashMap<Vec<u16>, FunctionLocalValue<'a>>,
    registrations: Rc<HashMap<Vec<u16>, FunctionLocalRegistration<'a>>>,
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
    pub(crate) value: &'a [u16],
}

/// One declaration in the body of a custom function, as its conditions select it.
pub(crate) struct SubstitutionFunctionDeclaration {
    name: Vec<u16>,
    tokens: ComponentValueList,
    includes_substitution: bool,
}

/// A definition visible to a caller scope, indexed in the prepared definition list.
#[derive(Clone, Copy)]
pub(crate) struct CustomFunctionVisibility {
    pub(crate) caller_scope_identity: usize,
    pub(crate) definition_index: usize,
}

/// That a call from a scope names a function: the definition its name dereferences to there, as
/// a tree-scoped reference.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FfiSubstitutionFunctionVisibility {
    pub caller_scope_identity: usize,
    pub function_identity: u64,
}

impl CustomPropertyRegistry {
    /// The registry of a document that registers no custom property.
    fn empty() -> Self {
        Self {
            registrations: HashMap::new(),
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

    pub(crate) fn name_is_registered(&self, name: &[u16]) -> bool {
        self.registrations.contains_key(name)
    }

    /// Whether a name has the same effective registration in two document registries.
    pub(crate) fn registration_matches(&self, other: &Self, name: &[u16]) -> bool {
        self.registrations.get(name) == other.registrations.get(name)
    }

    /// Whether a custom property of this name inherits: only a registration can say it does not.
    pub(crate) fn name_inherits(&self, name: &[u16]) -> bool {
        self.registrations
            .get(name)
            .is_none_or(|registration| registration.inherits)
    }

    /// Whether a custom property of this name is registered with a syntax other than the universal
    /// one, which its values compute against.
    pub(crate) fn name_has_syntax(&self, name: &[u16]) -> bool {
        self.registrations
            .get(name)
            .is_some_and(|registration| !matches!(registration.syntax, SyntaxNode::Universal))
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
        self.registrations
            .get(name)
            .is_some_and(|registration| is_interpolable(&registration.syntax))
    }

    /// A custom property's initial value: what its registration's initial value computes to, and
    /// the guaranteed-invalid value for an unregistered name or a registration without one.
    pub(crate) fn initial_value(&self, name: &[u16]) -> RetainedStyleValueData {
        self.registrations.get(name).map_or_else(
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
/// viewport, its place among its siblings, and the container-relative units it resolved against
/// its query containers, as a `container_relative_length_unit_mask`.
#[derive(Clone, Copy, Default)]
pub(crate) struct RegisteredValueReads {
    pub(crate) viewport: bool,
    pub(crate) sibling_position: bool,
    pub(crate) container_unit_mask: u8,
}

impl std::ops::BitOrAssign for RegisteredValueReads {
    fn bitor_assign(&mut self, other: Self) {
        self.viewport |= other.viewport;
        self.sibling_position |= other.sibling_position;
        self.container_unit_mask |= other.container_unit_mask;
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
        let source = crate::css::serialize::serialize_resolved_style_value_to_utf16(value.data());
        let parsed = parse_substitution_values(&source)
            .and_then(|values| self.parse_registered_value(registration, &values))
            .map(RetainedStyleValueData::from_owned);
        if parses.len() >= MAX_REGISTERED_VALUE_PARSES {
            parses.clear();
        }
        parses.insert(key, (value.clone(), parsed.clone()));
        parsed
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
        name: &[u16],
        value: RetainedStyleValueData,
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

        let value = match value.data() {
            // https://drafts.csswg.org/css-mixins/#resolve-function-styles
            // On result, all CSS-wide keywords are left unresolved.
            StyleValueData::Keyword { keyword } if !name.iter().copied().eq("result".encode_utf16()) => {
                match *keyword {
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
                    _ => value,
                }
            }
            _ => value,
        };

        // https://drafts.csswg.org/css-values-5/#invalid-substitution
        // An unregistered name, or one registered with the universal syntax, computes to the
        // guaranteed-invalid value; any other to its inherited value or its initial value, as if
        // its value had been specified as unset.
        let syntax_registration =
            registration.filter(|registration| !matches!(registration.syntax, SyntaxNode::Universal));
        let invalid_fallback = || match syntax_registration {
            Some(registration) if registration.inherits => inherited_value(),
            Some(_) => initial(),
            None => RetainedStyleValueData::from_owned(StyleValueData::GuaranteedInvalid),
        };
        if matches!(value.data(), StyleValueData::GuaranteedInvalid) {
            return Ok((invalid_fallback(), RegisteredValueReads::default()));
        }
        let Some(registration) = syntax_registration else {
            return Ok((value, RegisteredValueReads::default()));
        };

        let contains_attr_tainted_values = matches!(
            value.data(),
            StyleValueData::Unresolved {
                contains_attr_tainted_values: true,
                ..
            }
        );
        let Some(parsed) = self.parse_registered_value_memoized(registration, &value) else {
            return Ok((invalid_fallback(), RegisteredValueReads::default()));
        };
        // Without the element's lengths the value stays as parsed, as the host leaves one it has
        // no computed style to compute against.
        let Some(length) = finalization.length else {
            return Ok((parsed, RegisteredValueReads::default()));
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
        };
        if !contains_attr_tainted_values {
            return Ok((computed, reads));
        }

        // A value an attr() substituted into stays tainted, as an unresolved value carrying what
        // it computed to.
        let source = crate::css::serialize::serialize_resolved_style_value_to_utf16(computed.data());
        let mut wrapped = crate::css::parser::value_parser::unresolved_value_from_string(
            &source,
            &[],
            crate::css::parser::arbitrary_substitution::SubstitutionFunctionsPresence::default(),
        );
        let StyleValueData::Unresolved {
            contains_attr_tainted_values,
            parsed_value,
            ..
        } = &mut wrapped
        else {
            unreachable!("an unresolved value is built");
        };
        *contains_attr_tainted_values = true;
        *parsed_value = computed;
        Ok((RetainedStyleValueData::from_owned(wrapped), reads))
    }
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
        StyleValueData::RandomValueSharing {
            is_auto,
            name,
            element_shared,
            ..
        } => Some((*is_auto, name, *element_shared)),
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
        base: Option<&Self>,
    ) -> Self {
        let (mut own_values, mut own_names, parent, inheritance_parent, ancestor_count) = match base {
            Some(base) => (
                base.own_values.clone(),
                base.own_names.clone(),
                base.parent.clone(),
                base.inheritance_parent.clone(),
                base.ancestor_count,
            ),
            None => (HashMap::new(), HashMap::new(), None, None, 0),
        };
        let mut declared_names = Vec::with_capacity(animated.len());
        for (name_raw, entry) in animated {
            declared_names.push(name_raw);
            own_names.insert(entry.name.clone(), name_raw);
            own_values.insert(name_raw, entry);
        }
        Self {
            own_values,
            declared_names,
            own_names,
            ancestor_count,
            parent,
            inheritance_parent,
        }
    }

    /// The values `overlay`, an animation overlay, laid over its base, laid over `base` instead.
    pub(crate) fn animated_values_over(overlay: &Self, base: Option<&Self>) -> Self {
        let animated = overlay
            .declared_names
            .iter()
            .map(|name_raw| (*name_raw, overlay.own_values[name_raw].clone()));
        Self::animation_overlay(animated, base)
    }

    pub(crate) fn parent(&self) -> Option<&Arc<CustomPropertyStore>> {
        self.parent.as_ref()
    }

    /// The store this one was built over, whichever of its values it took in.
    pub(crate) fn inheritance_parent(&self) -> Option<&Arc<CustomPropertyStore>> {
        self.inheritance_parent.as_ref()
    }

    pub(crate) fn get(&self, name_raw: usize) -> Option<&CustomPropertyEntry> {
        self.own_values
            .get(&name_raw)
            .or_else(|| self.parent.as_ref()?.get(name_raw))
    }

    fn get_by_name_with_owner(&self, name: &[u16]) -> Option<(&CustomPropertyEntry, &CustomPropertyStore)> {
        self.own_names
            .get(name)
            .and_then(|name_raw| self.own_values.get(name_raw))
            .map(|entry| (entry, self))
            .or_else(|| self.parent.as_ref()?.get_by_name_with_owner(name))
    }

    fn get_own_by_name(&self, name: &[u16]) -> Option<&CustomPropertyEntry> {
        self.own_names
            .get(name)
            .and_then(|name_raw| self.own_values.get(name_raw))
    }

    /// What a child inherits of this store: the store without the entries a registration keeps
    /// from inheriting, over the same chain; `None` when every entry inherits.
    pub(crate) fn inheritable(&self, registry: &CustomPropertyRegistry) -> Option<Arc<CustomPropertyStore>> {
        if self.hands_down_every_value(registry) {
            return None;
        }
        let inherits = |entry: &CustomPropertyEntry| registry.name_inherits(&entry.name);
        let own_values: HashMap<usize, CustomPropertyEntry> = self
            .own_values
            .iter()
            .filter(|(_, entry)| inherits(entry))
            .map(|(&name_raw, entry)| (name_raw, entry.clone()))
            .collect();
        Some(Arc::new(Self {
            own_names: own_values
                .iter()
                .map(|(&name_raw, entry)| (entry.name.clone(), name_raw))
                .collect(),
            declared_names: self
                .declared_names
                .iter()
                .copied()
                .filter(|name_raw| own_values.contains_key(name_raw))
                .collect(),
            own_values,
            parent: self.parent.clone(),
            inheritance_parent: self.inheritance_parent.clone(),
            ancestor_count: self.ancestor_count,
        }))
    }

    /// Whether the store holds the same values as `other`, built over the same store and chained to
    /// the same parent.
    pub(crate) fn holds_same_values_as(&self, other: &Self) -> bool {
        let same = |first: &Option<Arc<Self>>, second: &Option<Arc<Self>>| match (first, second) {
            (None, None) => true,
            (Some(first), Some(second)) => Arc::ptr_eq(first, second),
            _ => false,
        };
        same(&self.parent, &other.parent)
            && same(&self.inheritance_parent, &other.inheritance_parent)
            && self.declared_names == other.declared_names
            && self.own_values.len() == other.own_values.len()
            && self.own_values.iter().all(|(name_raw, entry)| {
                other.own_values.get(name_raw).is_some_and(|other_entry| {
                    other_entry.important == entry.important
                        && crate::css::style_value::retained_style_values_equal(&other_entry.value, &entry.value)
                })
            })
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
        let mut own_names = HashMap::with_capacity(entries.len());
        let mut own_values = HashMap::with_capacity(entries.len());
        let mut declared_names = Vec::with_capacity(entries.len());
        for (name_raw, entry) in entries {
            declared_names.push(name_raw);
            own_names.insert(entry.name.clone(), name_raw);
            own_values.insert(name_raw, entry);
        }

        let inheritance_parent = parent.clone();
        let ancestor_count = if let Some(current_parent) = parent.clone() {
            if current_parent.ancestor_count >= MAX_ANCESTOR_COUNT - 1 {
                let mut ancestor = Some(current_parent.as_ref());
                while let Some(current) = ancestor {
                    for (&name_raw, entry) in &current.own_values {
                        if let std::collections::hash_map::Entry::Vacant(slot) = own_values.entry(name_raw) {
                            own_names.insert(entry.name.clone(), name_raw);
                            slot.insert(entry.clone());
                        }
                    }
                    ancestor = current.parent.as_deref();
                }
                parent = None;
                0
            } else if current_parent.own_values.len() <= ABSORB_THRESHOLD {
                for (&name_raw, entry) in &current_parent.own_values {
                    if let std::collections::hash_map::Entry::Vacant(slot) = own_values.entry(name_raw) {
                        own_names.insert(entry.name.clone(), name_raw);
                        slot.insert(entry.clone());
                    }
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
            own_values,
            declared_names,
            own_names,
            inheritance_parent,
            parent,
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
                        _name: source._name.clone(),
                        name: source.name.clone(),
                        value,
                        important: source.important,
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
        values: Vec<(usize, Arc<[u16]>, bool, *const c_void)>,
    ) -> Arc<CustomPropertyStore> {
        let entries = values
            .into_iter()
            .map(|(name_raw, name, important, value)| {
                (
                    name_raw,
                    CustomPropertyEntry {
                        _name: unsafe { RetainedUtf16FlyString::from_borrowed_raw(name_raw) },
                        name,
                        value: unsafe {
                            RetainedStyleValueData::from_retained_pointer(
                                crate::css::style_value::retain_style_value(value.cast()).cast(),
                            )
                        },
                        important,
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
        source: Vec<u16>,
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
    Property(Vec<u16>, Option<CustomFunctionIdentity>),
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
    /// Where the custom properties and sibling position a style query reads are noted, for the host to record.
    style_query_dependencies: Option<&'a mut Option<Box<StyleQueryDependencies>>>,
    custom_property_finalization: CustomPropertyFinalization<'a>,
    registered_value_reads: Option<&'a mut RegisteredValueReads>,
    element_store: Option<&'a CustomPropertyStore>,
    element_values: Option<&'a mut HashMap<usize, RetainedStyleValueData>>,
    substitution_limit_hits: u64,
    function_local_scopes: Vec<FunctionLocalScope<'a>>,
    token_cache: Option<&'a mut CustomPropertyTokenCache>,
}

impl ASFResolutionContext<'_> {
    fn note_style_query_dependency(&mut self, name: &[u16]) {
        if let Some(dependencies) = self.style_query_dependencies.as_deref_mut() {
            dependencies.get_or_insert_default().note(name);
        }
    }

    fn note_style_query_sibling_position_dependency(&mut self) {
        if let Some(reads) = self.registered_value_reads.as_deref_mut() {
            reads.sibling_position = true;
        }
        if let Some(dependencies) = self.style_query_dependencies.as_deref_mut() {
            dependencies.get_or_insert_default().sibling_position = true;
        }
    }
}

/// The custom properties and sibling position style queries read in one resolution.
#[derive(Clone, Default)]
pub(crate) struct StyleQueryDependencies {
    names: Vec<Vec<u16>>,
    pub(crate) sibling_position: bool,
}

impl StyleQueryDependencies {
    fn note(&mut self, name: &[u16]) {
        if !self.names.iter().any(|noted| noted == name) {
            self.names.push(name.to_vec());
        }
    }

    pub(crate) fn extend(&mut self, other: &Self) {
        self.sibling_position |= other.sibling_position;
        for name in other.names() {
            self.note(name);
        }
    }

    pub(crate) fn names(&self) -> impl Iterator<Item = &[u16]> {
        self.names.iter().map(Vec::as_slice)
    }
}

/// Visits each custom property the style queries of a resolution read, and frees them.
///
/// # Safety
/// `dependencies` must be null or what a resolution left in its `style_query_dependencies`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_query_dependencies_take(
    dependencies: *mut c_void,
    context: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, FfiUtf16View),
) {
    if dependencies.is_null() {
        return;
    }
    let dependencies = unsafe { Box::from_raw(dependencies.cast::<StyleQueryDependencies>()) };
    for name in dependencies.names() {
        unsafe {
            visit(context, FfiUtf16View::from_utf16(name));
        }
    }
}

type CustomPropertyTokenCache = HashMap<*const StyleValueData, Arc<ComponentValueList>>;

pub(crate) struct VarResolutionEnvironment<'a> {
    attributes: HashMap<Vec<u16>, Vec<u16>>,
    custom_functions: &'a CustomFunctionRegistry<'a>,
    token_cache: CustomPropertyTokenCache,
}

fn source_ends_with_comment(source: crate::css::css_tokenizer::TokenizerInput<'_>) -> bool {
    match source {
        crate::css::css_tokenizer::TokenizerInput::Ascii(source) => source
            .iter()
            .rposition(|unit| !unit.is_ascii_whitespace())
            .is_some_and(|end| end > 0 && source[end - 1..=end] == *b"*/"),
        crate::css::css_tokenizer::TokenizerInput::Utf16(source) => source
            .iter()
            .rposition(|unit| !matches!(*unit, 0x09 | 0x0a | 0x0c | 0x0d | 0x20))
            .is_some_and(|end| end > 0 && source[end - 1..=end] == [u16::from(b'*'), u16::from(b'/')]),
    }
}

fn single_css_wide_keyword(tokens: &[ComponentValue]) -> Option<&[u16]> {
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

fn tokens_for_unresolved_custom_property_value(data: &StyleValueData) -> (ComponentValueSlice<'_>, bool, bool) {
    let StyleValueData::Unresolved {
        components,
        presence_attr,
        presence_dashed_function,
        presence_env,
        presence_if,
        presence_inherit,
        presence_var,
        contains_attr_tainted_values,
        source_text,
        ..
    } = data
    else {
        unreachable!("expected an unresolved custom property value");
    };

    let mut values = components.as_counted_slice();

    // Discard trailing comments
    if source_ends_with_comment(TokenizerInput::from(source_text)) {
        values = values.trim_end_whitespace();
    }

    (
        values,
        *presence_var
            || *presence_attr
            || *presence_dashed_function
            || *presence_env
            || *presence_if
            || *presence_inherit,
        *contains_attr_tainted_values,
    )
}

fn resolve_typed_custom_property_value(
    data: &StyleValueData,
    context: &mut ASFResolutionContext<'_>,
) -> TokenResolution {
    let key = std::ptr::from_ref(data);
    if let Some(tokens) = context.token_cache.as_deref().and_then(|cache| cache.get(&key)) {
        return TokenResolution::Resolved(tokens.as_ref().clone());
    }
    let source = crate::css::serialize::serialize_style_value_to_utf16(data);
    let tokens: Arc<ComponentValueList> = parse_substitution_values(&source)
        .expect("serialized style values tokenize correctly")
        .into();
    if let Some(cache) = context.token_cache.as_deref_mut() {
        cache.insert(key, Arc::clone(&tokens));
    }
    TokenResolution::Resolved(tokens.as_ref().clone())
}

/// Tokens, substitution-function presence, and attribute reads for a function value.
fn tokens_for_function_value(data: &StyleValueData) -> (ComponentValueList, bool, bool) {
    if let StyleValueData::Unresolved {
        components,
        presence_attr,
        presence_dashed_function,
        presence_env,
        presence_if,
        presence_inherit,
        presence_var,
        ..
    } = data
    {
        return (
            components.as_counted_slice().trim_whitespace().to_owned(),
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
            name: declaration.name.units().to_vec(),
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
        .map(|attribute| (attribute.name.to_vec(), attribute.value.to_vec()))
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
    name: &[u16],
    value: RetainedStyleValueData,
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
    name: &[u16],
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> Result<RetainedStyleValueData, ()> {
    let name_raw = store.own_names[name];

    if let Some(cached) = context.element_values.as_deref().ok_or(())?.get(&name_raw) {
        return Ok(cached.clone());
    }

    let limit_hits_before_computation = context.substitution_limit_hits;

    #[cfg(test)]
    CUSTOM_PROPERTY_COMPUTATIONS.with(|count| count.set(count.get() + 1));

    let entry = &store.own_values[&name_raw];
    let (tokens, includes_substitution, tainted) = match entry.value.data() {
        data @ StyleValueData::Unresolved { .. } => tokens_for_unresolved_custom_property_value(data),
        _ => (ComponentValueSlice::new(&[]), false, false),
    };

    let value = if includes_substitution {
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
            ..*context
        };

        let result = substitute_arbitrary_substitution_functions(
            Some(store),
            Some(registry),
            tokens,
            &mut property_context,
            recursion_depth,
            Some(SubstitutionContextDependency::Property(name.to_vec(), None)),
        );

        context.substitution_limit_hits = property_context.substitution_limit_hits;

        match result {
            TokenResolution::Resolved(tokens) => crate::css::cascaded_properties::parse_substituted_value(
                property_context.parse_context.ok_or(())?,
                crate::css::property_metadata::property_id::CUSTOM,
                &serialize_tokens(&tokens),
                property_context.contains_attr_tainted_values,
            ),
            TokenResolution::Invalid | TokenResolution::Cyclic => {
                RetainedStyleValueData::from_owned(StyleValueData::GuaranteedInvalid)
            }
            TokenResolution::NotHandled => return Err(()),
        }
    } else {
        entry.value.clone()
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
        element_values: Some(&mut HashMap::new()),
        token_cache: Some(&mut environment.token_cache),
        ..Default::default()
    };

    let registry = inputs
        .registry
        .unwrap_or_else(|| CustomPropertyRegistry::shared_empty());

    let values = store
        .declared_names
        .iter()
        .map(|&name_raw| {
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
                    RetainedStyleValueData::from_owned(StyleValueData::GuaranteedInvalid),
                ),
            }
        })
        .collect();

    (values, reads)
}

fn computed_property_tokens(value: &RetainedStyleValueData, context: &mut ASFResolutionContext) -> TokenResolution {
    match value.data() {
        StyleValueData::GuaranteedInvalid => TokenResolution::Invalid,
        data @ StyleValueData::Unresolved { .. } => {
            let (tokens, _, tainted) = tokens_for_unresolved_custom_property_value(data);
            context.contains_attr_tainted_values |= tainted;
            TokenResolution::Resolved(tokens.to_owned())
        }
        data => resolve_typed_custom_property_value(data, context),
    }
}

fn registration_accepts_tokens(
    registry: &CustomPropertyRegistry,
    registration: &RegisteredCustomProperty,
    tokens: &[ComponentValue],
) -> bool {
    matches!(registration.syntax, SyntaxNode::Universal)
        || registry.parse_registered_value(registration, tokens).is_some()
}

fn registered_property_fallback(
    inheritance_store: Option<&CustomPropertyStore>,
    registry: &CustomPropertyRegistry,
    registration: &RegisteredCustomProperty,
    name: &[u16],
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
        .map_or(TokenResolution::Invalid, TokenResolution::from_source)
}

fn resolve_css_wide_keyword(
    owner: &CustomPropertyStore,
    registry: Option<&CustomPropertyRegistry>,
    name: &[u16],
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
    name: &[u16],
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
                name.to_owned(),
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
    name: &[u16],
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
    name: &[u16],
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
                .insert(name.to_owned(), tokens.clone());
        }

        return result;
    }

    // Pop the function local scopes while we resolve the element level custom property.
    let mut local_scopes = std::mem::take(&mut context.function_local_scopes);
    let context_ref = RefCell::new(context);
    let _guard = ScopeGuard::new(|| context_ref.borrow_mut().function_local_scopes = std::mem::take(&mut local_scopes));
    let context = &mut **context_ref.borrow_mut();

    let substitution_context = SubstitutionContextDependency::Property(name.to_owned(), None);
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
            .map_or(TokenResolution::Invalid, TokenResolution::from_source);
    };
    if lookup == CustomPropertyLookup::Normal
        && context.element_store.is_some_and(|store| std::ptr::eq(store, owner))
        && owner.declared_names.contains(&owner.own_names[name])
        && store.is_some_and(|store| std::ptr::eq(store, owner))
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
    let result = if matches!(data, StyleValueData::Unresolved { .. }) {
        let (source, includes_var, contains_attr_tainted_values) = tokens_for_unresolved_custom_property_value(data);
        context.contains_attr_tainted_values |= contains_attr_tainted_values;
        if includes_var {
            substitute_arbitrary_substitution_functions(
                store,
                registry,
                source,
                context,
                recursion_depth + 1,
                Some(substitution_context),
            )
        } else {
            TokenResolution::Resolved(source.to_owned())
        }
    } else {
        resolve_typed_custom_property_value(data, context)
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
        [value] => value.ident().filter(|name| name.starts_with_ascii("--")),
        _ => None,
    };
    if let Some(name) = name {
        match resolve_custom_property(store, registry, name, context, recursion_depth) {
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
        [value] => value.ident().filter(|name| name.starts_with_ascii("--")),
        _ => None,
    };
    if let Some(name) = name {
        let local_scope = context.function_local_scopes.pop();
        let inherited_store = if local_scope.is_some() {
            store
        } else {
            context.inheritance_store
        };
        let result = resolve_custom_property_with_lookup(
            inherited_store,
            registry,
            name,
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
            context.note_style_query_dependency(name);
            resolve_custom_property(store, registry, name, context, recursion_depth + 1)
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
    let Some(name) = name.ident() else {
        return ConditionEvaluation::Invalid;
    };
    let query = colon.map(|colon| &tokens[colon + 1..]);
    evaluate_named_style_feature(store, registry, name, query, context, recursion_depth)
}

/// A `style()` feature naming a property, `name` or `name: query`.
fn evaluate_named_style_feature(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    name: &[u16],
    query: Option<&[ComponentValue]>,
    context: &mut ASFResolutionContext,
    recursion_depth: u32,
) -> ConditionEvaluation {
    if !name.starts_with_ascii("--") {
        return ConditionEvaluation::Match(false);
    }

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
    context.note_style_query_dependency(name);
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
            &parse_substitution_values(initial).expect("registered initial value has valid components"),
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
                        registration.initial_source.as_ref().and_then(parse_substitution_values)
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
                serialize_component_values_to_utf16(trim_whitespace(&computed), ComponentSerializationMode::Normalized)
                    == serialize_component_values_to_utf16(
                        trim_whitespace(&expected),
                        ComponentSerializationMode::Normalized,
                    )
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
            .or_else(|| registration.map(|registration| &registration.syntax)),
    ) {
        return ConditionEvaluation::Match(registered_style_query_values_are_equal(
            registry, syntax, &computed, query, context,
        ));
    }
    ConditionEvaluation::Match(
        serialize_component_values_to_utf16(trim_whitespace(&computed), ComponentSerializationMode::Normalized)
            == serialize_component_values_to_utf16(query, ComponentSerializationMode::Normalized),
    )
}

/// Evaluate a container query's style feature against a container's retained style: its
/// custom-property environment, and lengths and colors as its record computes them. The custom
/// properties the feature reads are noted in `dependencies`.
pub(crate) fn evaluate_retained_container_style_feature(
    store: Option<&CustomPropertyStore>,
    registry: &CustomPropertyRegistry,
    feature: &StyleFeature,
    length_resolution_context: &crate::css::style_compute::FfiLengthResolutionContext,
    color_resolution_input: crate::css::color_resolution::ColorResolutionInput<'_>,
    tree_counting: Option<(u64, u64)>,
    dependencies: &mut Option<Box<StyleQueryDependencies>>,
) -> MatchResult {
    let mut context = ASFResolutionContext {
        inheritance_store: store.and_then(|store| store.inheritance_parent.as_deref()),
        style_query_length_resolution_context: Some(length_resolution_context),
        style_query_color_resolution_input: Some(color_resolution_input),
        style_query_tree_counting: tree_counting,
        style_query_dependencies: Some(dependencies),
        ..Default::default()
    };
    let registry = Some(registry);
    let result = match feature {
        StyleFeature::Boolean(name) => evaluate_named_style_feature(store, registry, name, None, &mut context, 0),
        StyleFeature::Plain { name, value, .. } => {
            evaluate_named_style_feature(store, registry, name, Some(value), &mut context, 0)
        }
        StyleFeature::Range { .. } => evaluate_style_range(store, registry, feature, &mut context, 0),
    };
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
    property_names: impl IntoIterator<Item = &'names [u16]>,
    context: &mut ASFResolutionContext<'a>,
    recursion_depth: u32,
) -> HashMap<Vec<u16>, TokenResolution> {
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
            computed_values.insert(name.to_vec(), value);
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
            parameter.name.units().to_vec(),
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
    let result_name: Vec<u16> = b"result".iter().copied().map(u16::from).collect();
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
            argument_rule.insert(parameter.name.units().to_vec(), value);
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
            .map(|parameter| parameter.name.units()),
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
            .map(|declaration| declaration.name.as_slice())
            .filter(|name| *name != result_name.as_slice())
            .chain(std::iter::once(result_name.as_slice())),
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
        let condition = match substitute_tokens(store, registry, &branch[..colon], context, recursion_depth + 1) {
            TokenResolution::Resolved(tokens) => tokens,
            TokenResolution::Invalid | TokenResolution::Cyclic => branch[..colon].to_vec().into(),
            TokenResolution::NotHandled => return TokenResolution::NotHandled,
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
    let rest = trim_whitespace(&tokens[1..]);
    if rest.is_empty() {
        return Ok((name.to_vec(), AttrSyntax::Omitted));
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
        return Ok((name.to_vec(), syntax));
    }
    if rest[0].is_delim(b'%') {
        if rest.len() != 1 {
            return Err(AttrSyntaxParseFailure::SyntaxSpecified);
        }
        return Ok((name.to_vec(), AttrSyntax::Unit(vec![u16::from(b'%')])));
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
        return Ok((name.to_vec(), AttrSyntax::Syntax(syntax)));
    }
    Err(AttrSyntaxParseFailure::SyntaxOmitted)
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
    if let StyleValueData::Unresolved { components, .. } = parsed {
        return Some(components.as_counted_slice().to_owned());
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
    pub(crate) style_query_inputs: Option<&'a crate::css::cascaded_properties::FfiStyleQueryInputs>,
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
    let substitution_context = Some(SubstitutionContextDependency::Property(root_property_name, None));
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
    if !matches!(value_data, StyleValueData::Unresolved { .. }) {
        return NativeVarResolution::NotHandled;
    }
    let (source, includes_substitution, contains_attr_tainted_values) =
        tokens_for_unresolved_custom_property_value(value_data);
    // A value holding no substitution function has nothing to resolve: it is invalid at
    // computed-value time, as every caller takes an unresolved value it cannot substitute.
    if !includes_substitution {
        return NativeVarResolution::Invalid;
    }
    context.contains_attr_tainted_values = contains_attr_tainted_values;
    let result =
        substitute_arbitrary_substitution_functions(store, registry, source, &mut context, 0, substitution_context);
    match result {
        // A custom property carries its taint to the values that substitute it; another property is invalid where a
        // tainted value reaches a URL.
        TokenResolution::Resolved(tokens) => NativeVarResolution::Resolved {
            source: serialize_tokens(&tokens),
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

    #[test]
    fn custom_property_values_outlive_their_store() {
        let name = utf16("--value");
        let store = CustomPropertyStore::child(
            None,
            vec![(
                0,
                CustomPropertyEntry {
                    _name: RetainedUtf16FlyString::none(),
                    name: name.into(),
                    value: RetainedStyleValueData::from_owned(StyleValueData::Number { value: 1.0 }),
                    important: false,
                },
            )],
        );
        let value = store.get(0).unwrap().value.clone();
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
                Some(SubstitutionContextDependency::Property(utf16("--root"), None)),
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
        let (root_context, _root_guard) = guard(SubstitutionContextDependency::Property(utf16("--root"), None));
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
            .enumerate()
            .map(|(name_raw, (name, source))| {
                let source = utf16(source);
                let values = consume_a_counted_list_of_component_values(tokenize_for_parser(&source)).unwrap();
                let crate::css::parser::value_parser::ParseOutcome::Parsed(value) =
                    crate::css::parser::value_parser::parse_css_value_with_source(
                        parse_context,
                        crate::css::property_metadata::property_id::CUSTOM,
                        &values,
                        &source,
                        &source,
                    )
                else {
                    panic!("custom property value should parse");
                };
                (
                    name_raw,
                    CustomPropertyEntry {
                        _name: RetainedUtf16FlyString::none(),
                        name: utf16(name).into(),
                        value: RetainedStyleValueData::from_arc(value),
                        important: false,
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
            Some(
                registration
                    .initial_value
                    .to_utf16()
                    .expect("invalid registered custom property initial value"),
            )
        } else {
            None
        };
        let Some(syntax) = (unsafe { clone_syntax_handle(registration.syntax) }) else {
            continue;
        };
        // SAFETY: The host lends a live style value for the call; the registry keeps a reference of
        //         its own for as long as it holds the registration.
        let computed_initial = unsafe {
            RetainedStyleValueData::from_retained_pointer(crate::css::style_value::retain_style_value(
                registration.computed_initial_value.cast(),
            ))
        };
        registry.registrations.insert(
            name,
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

/// Takes a reference of its own to the document registry `registry`, for a style transaction that
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
    let mut own_names = HashMap::with_capacity(entries.len());
    let own_values = entries
        .iter()
        .map(|entry| {
            let name: Arc<[u16]> = entry.name.to_utf16().expect("invalid custom property name").into();
            own_names.insert(name.clone(), entry.name_raw);
            (
                entry.name_raw,
                CustomPropertyEntry {
                    _name: unsafe { RetainedUtf16FlyString::from_leaked_raw(entry.name_raw) },
                    name,
                    value: unsafe { RetainedStyleValueData::from_retained_pointer(entry.data.cast()) },
                    important: entry.important,
                },
            )
        })
        .collect();
    Arc::into_raw(Arc::new(CustomPropertyStore {
        own_values,
        declared_names: entries[..declared_count].iter().map(|entry| entry.name_raw).collect(),
        own_names,
        ancestor_count: parent.as_ref().map_or(0, |parent| parent.ancestor_count + 1),
        parent,
        inheritance_parent,
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
    let base = unsafe { base.cast::<CustomPropertyStore>().as_ref() };
    let animated = entries.iter().map(|entry| {
        let name: Arc<[u16]> = entry.name.to_utf16().expect("invalid custom property name").into();
        (
            entry.name_raw,
            CustomPropertyEntry {
                _name: unsafe { RetainedUtf16FlyString::from_leaked_raw(entry.name_raw) },
                name,
                value: unsafe { RetainedStyleValueData::from_retained_pointer(entry.data.cast()) },
                important: entry.important,
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
    for name_raw in &store.declared_names {
        let entry = store
            .own_values
            .get(name_raw)
            .expect("declared custom property must be an own value");
        unsafe {
            callback(context, *name_raw, entry.important, entry.value.pointer().cast());
        }
    }
}
