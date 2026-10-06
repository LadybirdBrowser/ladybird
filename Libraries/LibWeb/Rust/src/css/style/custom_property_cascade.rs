/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The custom-property environment of an element the engine computes a record for.
//!
//! The cascade decides a custom property the way it decides a longhand - the highest-priority
//! declaration of the name wins - but the engine keeps no winner column per name: the names are
//! unbounded, and nothing but the environment reads them. So an element's custom declarations
//! cascade here, by name, when its environment is computed: over the matched rules and the inline
//! style, into the store the C++ resolver builds from, resolved by the same Rust resolver against
//! the environment the element inherits.

use std::ffi::c_void;
use std::ops::ControlFlow;
use std::sync::Arc;

use super::publication::{Drive, Unanswered};
use super::*;
use crate::css::cascaded_properties::{
    CallbackFreeParseOutcome, FfiCascadeResolutionContext, FfiCustomPropertyDriveInput,
    destroy_resolved_custom_properties, parse_substituted_source, parse_substituted_without_callbacks,
    resolve_declared_custom_properties,
};
use crate::css::custom_properties::{
    CustomPropertyFinalization, CustomPropertyStore, FfiSubstitutionFunctionDeclaration,
    FfiSubstitutionFunctionDefinition, FfiSubstitutionFunctionVisibility, NativeVarResolution,
    prepare_var_resolution_environment,
};
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::component_value::{ComponentKind, ComponentValue};
use crate::css::parser::value_parser::ParseOutcome;
use crate::css::rule::CompiledFunction;
use crate::css::style_value::{
    RetainedStyleValueData, StyleValueData, release_style_value, utf16_equals_ascii_case_insensitive,
};
use custom_property_environments::{CascadedCustomProperty, CustomPropertyName};

/// The document's media features as the style update a transaction belongs to saw them, which
/// `media()` conditions in `if()` read. Copied rather than borrowed: the host's snapshot ends with
/// the style update, and a record demanded after it still resolves against these.
#[derive(Default)]
pub(super) struct DocumentMediaSnapshot {
    values: Vec<crate::css::parser::query_parser::FfiMediaFeatureValue>,
    /// Lent no output flag: `take_in` clears the one the host's context points to.
    length: Option<crate::css::style_compute::FfiLengthResolutionContext>,
}

// SAFETY: The only pointer the snapshot holds is the length context's output flag, which `take_in`
// clears before keeping the context.
unsafe impl Send for DocumentMediaSnapshot {}
unsafe impl Sync for DocumentMediaSnapshot {}

impl DocumentMediaSnapshot {
    /// Take in what the host lent with the inputs, and clear the borrowed fields so the inputs
    /// compare by value from here on.
    ///
    /// # Safety
    /// The borrowed fields must name a live array of their stated length and a live length
    /// context, or nothing, for this call.
    pub(super) unsafe fn take_in(&mut self, inputs: &mut bridge::FfiDocumentStyleComputationInputs) {
        self.values.clear();
        if !inputs.media_feature_values.is_none() && inputs.media_feature_value_count != 0 {
            self.values.extend_from_slice(unsafe {
                std::slice::from_raw_parts(
                    inputs.media_feature_values.as_pointer().cast(),
                    inputs.media_feature_value_count,
                )
            });
        }
        self.length = unsafe {
            inputs
                .media_length_resolution_context
                .as_pointer()
                .cast::<crate::css::style_compute::FfiLengthResolutionContext>()
                .as_ref()
        }
        .map(|&context| crate::css::style_compute::FfiLengthResolutionContext {
            resolved_viewport_relative_length: std::ptr::null_mut(),
            ..context
        });
        inputs.media_feature_values = bridge::FfiHostHandle::default();
        inputs.media_feature_value_count = 0;
        inputs.media_length_resolution_context = bridge::FfiHostHandle::default();
    }

    /// The snapshot as the resolver reads a media environment, borrowing from it.
    fn environment(&self) -> crate::css::parser::query_parser::FfiMediaEnvironment {
        crate::css::parser::query_parser::FfiMediaEnvironment {
            values: self.values.as_ptr(),
            value_count: self.values.len(),
            length_resolution_context: self
                .length
                .as_ref()
                .map_or(std::ptr::null(), |length| std::ptr::from_ref(length).cast()),
        }
    }

    /// What lengths resolve against for a document without styled elements.
    pub(super) fn length(&self) -> Option<&crate::css::style_compute::FfiLengthResolutionContext> {
        self.length.as_ref()
    }
}

/// The `@function` definitions each scope sees, as the host published them with a transaction's
/// inputs: what a custom function call resolves against. A scope is a host `StyleScope`, named by
/// its identity, as the resolver names the scope of a call and of a definition.
#[derive(Default)]
pub(super) struct DocumentFunctionSnapshot {
    /// Each definition some scope sees, by its identity, with the scope that defines it.
    definitions: HashMap<u64, (Arc<CompiledFunction>, usize)>,
    /// Which definition each scope's calls name, a name at a time.
    visibilities: Vec<FfiSubstitutionFunctionVisibility>,
    /// The scope of each tree scope that sees a definition.
    caller_scopes: HashMap<TreeScopeID, usize>,
    /// What a call from each scope reaches, worked out once for the transaction.
    scope_functions: HashMap<usize, ScopeFunctions>,
}

/// What a call from one scope reaches, as `visible_from` finds it, with the input blocks of each
/// definition's body the document's media selects: only the container-gated blocks among those
/// differ from one element to the next.
pub(super) struct ScopeFunctions {
    /// Each reachable definition, with the scope that defines it and its selected blocks.
    definitions: Vec<(Arc<CompiledFunction>, usize, Box<[usize]>)>,
    visibilities: Vec<FfiSubstitutionFunctionVisibility>,
}

/// What a call from one tree scope may reach: the definitions its scope sees, and those the
/// defining scope of each of them sees, which the calls in its body name.
struct VisibleFunctions<'a> {
    /// Each reachable definition, with the scope that defines it.
    definitions: Vec<(&'a CompiledFunction, usize)>,
    visibilities: Vec<FfiSubstitutionFunctionVisibility>,
}

impl DocumentFunctionSnapshot {
    /// Take in what the host lent with the inputs, retaining each definition a scope sees, and
    /// clear the borrowed fields so the inputs compare by value from here on. `layer_index` ranks a
    /// cascade layer within its tree scope.
    ///
    /// # Safety
    /// The borrowed fields must name a live array of their stated length, or nothing, for this
    /// call, and each entry a live compiled function or none.
    pub(super) unsafe fn take_in(
        &mut self,
        inputs: &mut bridge::FfiDocumentStyleComputationInputs,
        media: &DocumentMediaSnapshot,
        layer_index: impl Fn(TreeScopeID, CascadeLayerID) -> u32,
    ) {
        self.definitions.clear();
        self.visibilities.clear();
        self.caller_scopes.clear();
        self.scope_functions.clear();
        let entries = if inputs.custom_functions.is_none() || inputs.custom_function_count == 0 {
            &[]
        } else {
            unsafe {
                std::slice::from_raw_parts(
                    inputs
                        .custom_functions
                        .as_pointer()
                        .cast::<bridge::FfiCustomFunctionEntry>(),
                    inputs.custom_function_count,
                )
            }
        };
        // SAFETY: The host lends a live compiled function, or none.
        let function_of =
            |entry: &bridge::FfiCustomFunctionEntry| unsafe { entry.function.cast::<CompiledFunction>().as_ref() };
        // What a scope defines under a name is the rule of the most precedent origin, and within it
        // of the last layer, a later rule winning a tie.
        let mut parent_scopes = HashMap::default();
        let mut winners = HashMap::default();
        for entry in entries {
            self.caller_scopes.insert(TreeScopeID(entry.tree_scope), entry.scope);
            parent_scopes.insert(entry.scope, entry.parent_scope);
            let Some(function) = function_of(entry) else {
                continue;
            };
            let precedence = (
                entry.origin,
                layer_index(TreeScopeID(entry.tree_scope), CascadeLayerID(entry.layer)),
            );
            let key = (entry.scope, function.signature.name.units());
            if winners.get(&key).is_none_or(|&(winner, _)| winner <= precedence) {
                winners.insert(key, (precedence, function));
            }
        }
        let mut defined: HashMap<usize, Vec<&CompiledFunction>> = HashMap::default();
        for entry in entries {
            if let Some(function) = function_of(entry)
                && std::ptr::eq(winners[&(entry.scope, function.signature.name.units())].1, function)
            {
                defined.entry(entry.scope).or_default().push(function);
            }
        }
        // https://drafts.csswg.org/css-shadow-1/#tree-scoped-name-global
        // A name a scope does not define is looked up in the scope of its host's tree, and so on up.
        for &caller_scope in parent_scopes.keys() {
            let mut names = HashSet::default();
            let mut scope = caller_scope;
            while scope != 0 {
                for &function in defined.get(&scope).into_iter().flatten() {
                    if !names.insert(function.signature.name.units()) {
                        continue;
                    }
                    self.definitions.entry(function.identity).or_insert_with(|| {
                        let function = std::ptr::from_ref(function);
                        // SAFETY: The host lends a function shared by reference count; the snapshot keeps one.
                        let function = unsafe {
                            Arc::increment_strong_count(function);
                            Arc::from_raw(function)
                        };
                        (function, scope)
                    });
                    self.visibilities.push(FfiSubstitutionFunctionVisibility {
                        caller_scope_identity: caller_scope,
                        function_identity: function.identity,
                    });
                }
                scope = parent_scopes.get(&scope).copied().unwrap_or(0);
            }
        }
        inputs.custom_functions = bridge::FfiHostHandle::default();
        inputs.custom_function_count = 0;
        let media_environment = media.environment();
        // SAFETY: The environment points into the media snapshot, which outlives the evaluation.
        let media_environment = unsafe { media_environment.borrow() };
        for &caller_scope in self.caller_scopes.values() {
            if self.scope_functions.contains_key(&caller_scope) {
                continue;
            }
            let visible = self.visible_from_scope(caller_scope);
            let definitions = visible
                .definitions
                .iter()
                .map(|(function, scope)| {
                    // A list without queries matches; one with queries matches when any query does.
                    let selected = function
                        .inputs
                        .iter()
                        .enumerate()
                        .filter(|(_, input)| {
                            input.media.iter().all(|list| {
                                list.queries.is_empty()
                                    || list.queries.iter().any(|query| query.matches_media(media_environment))
                            })
                        })
                        .map(|(index, _)| index)
                        .collect();
                    let function = self.definitions[&function.identity].0.clone();
                    (function, *scope, selected)
                })
                .collect();
            let scope_functions = ScopeFunctions {
                definitions,
                visibilities: visible.visibilities,
            };
            self.scope_functions.insert(caller_scope, scope_functions);
        }
    }

    /// The scope a call from an element of `tree_scope` names, none where its scope sees no
    /// definition, and what the call reaches.
    pub(super) fn scope_functions(&self, tree_scope: TreeScopeID) -> (usize, Option<&ScopeFunctions>) {
        let caller_scope = self.caller_scopes.get(&tree_scope).copied().unwrap_or(0);
        (caller_scope, self.scope_functions.get(&caller_scope))
    }

    /// What a call from an element of `tree_scope` may reach. A tree scope whose scope sees no
    /// definition reaches none, and each of its calls names nothing.
    #[cfg(test)]
    fn visible_from(&self, tree_scope: TreeScopeID) -> VisibleFunctions<'_> {
        self.visible_from_scope(self.caller_scopes.get(&tree_scope).copied().unwrap_or(0))
    }

    fn visible_from_scope(&self, caller_scope: usize) -> VisibleFunctions<'_> {
        let mut scopes = vec![caller_scope];
        let mut definitions: Vec<(&CompiledFunction, usize)> = Vec::new();
        let mut index = 0;
        while let Some(&scope) = scopes.get(index) {
            for visibility in self
                .visibilities
                .iter()
                .filter(|visibility| visibility.caller_scope_identity == scope)
            {
                let Some((function, definition_scope)) = self.definitions.get(&visibility.function_identity) else {
                    debug_assert!(false, "a visible custom function is published with its definition");
                    continue;
                };
                if definitions
                    .iter()
                    .any(|(reached, _)| reached.identity == function.identity)
                {
                    continue;
                }
                definitions.push((&**function, *definition_scope));
                if !scopes.contains(definition_scope) {
                    scopes.push(*definition_scope);
                }
            }
            index += 1;
        }
        let visibilities = self
            .visibilities
            .iter()
            .filter(|visibility| scopes.contains(&visibility.caller_scope_identity))
            .copied()
            .collect();
        VisibleFunctions {
            definitions,
            visibilities,
        }
    }
}

/// The custom functions a node's calls may reach, as the resolver takes them: each definition with
/// the declarations its conditions select for the node, and which definition each scope's calls
/// name. It points into the definitions the engine retains for the transaction, which outlive
/// every drive in it.
pub(super) struct PreparedCustomFunctions {
    /// What `definitions` point into.
    _declarations: Vec<Vec<FfiSubstitutionFunctionDeclaration>>,
    definitions: Vec<FfiSubstitutionFunctionDefinition>,
    visibilities: Vec<FfiSubstitutionFunctionVisibility>,
    caller_scope: usize,
    /// Whether a selected declaration substitutes `attr()`, which reads the node's attributes.
    pub(super) reads_attributes: bool,
    /// What the container conditions read of the node's containers, for the host to record with
    /// its record; `None` when no condition was evaluated.
    pub(super) container_effects: Option<super::container_queries::ContainerVerdict>,
}

/// What a registered custom property's value computes against: the element's lengths as its style
/// computes them after line-height, and the color scheme its colors resolve with (a
/// PreferredColorScheme code). The length context lends no output flag.
#[derive(Clone, Copy)]
pub(super) struct RegisteredValueContext {
    pub length: crate::css::style_compute::FfiLengthResolutionContext,
    pub color_scheme: u8,
}

/// Draws the random base value of a registered value's random caching key for its element from
/// the engine's own, as the host draws one.
///
/// # Safety
/// `context` must point at the element and the engine's random base values, and `name` at
/// `length` code units.
unsafe extern "C" fn draw_engine_random_base_value(
    context: *mut c_void,
    name: *const u16,
    length: usize,
    element_shared: bool,
) -> f64 {
    let (node, values) = unsafe { &mut *context.cast::<(StyleNodeID, &mut super::random_bases::RandomBaseValues)>() };
    let name = match length {
        0 => &[],
        _ => unsafe { std::slice::from_raw_parts(name, length) },
    };
    values.ensure(Some(*node), name, element_shared)
}

/// The resolution context the engine substitutes under: the stores alone, with no callback into
/// C++ - what the engine cannot resolve without one is left to C++ before this is built.
#[expect(
    clippy::too_many_arguments,
    reason = "the context borrows each resolution input on its own"
)]
fn engine_resolution_context(
    parse_context: &crate::css::parser::value_parser::ParseContext,
    store: *const c_void,
    inheritance_store: *const c_void,
    registry: *const c_void,
    attributes: Option<&SubstitutionAttributes<'_>>,
    media_environment: &crate::css::parser::query_parser::FfiMediaEnvironment,
    style_query: Option<&crate::css::cascaded_properties::FfiStyleQueryInputs>,
    style_query_references: Option<&mut Option<Box<crate::css::custom_properties::StyleQueryDependencies>>>,
    functions: Option<&PreparedCustomFunctions>,
) -> FfiCascadeResolutionContext {
    let substitution_attributes = attributes.map_or(&[][..], |attributes| attributes.attributes.as_slice());
    FfiCascadeResolutionContext {
        parse_context: std::ptr::from_ref(parse_context).cast(),
        media_environment: std::ptr::from_ref(media_environment).cast(),
        load_media_environment: None,
        custom_property_store: store,
        inheritance_custom_property_store: inheritance_store,
        custom_property_registry: registry,
        root_custom_property_name: FfiUtf16View {
            ascii: std::ptr::null(),
            utf16: std::ptr::null(),
            length: 0,
        },
        attributes: substitution_attributes.as_ptr(),
        attribute_count: substitution_attributes.len(),
        attribute_names_are_ascii_case_insensitive: attributes
            .is_some_and(|attributes| attributes.names_are_ascii_case_insensitive),
        custom_functions: functions.map_or(std::ptr::null(), |functions| functions.definitions.as_ptr()),
        custom_function_count: functions.map_or(0, |functions| functions.definitions.len()),
        custom_function_scope_identity: functions.map_or(0, |functions| functions.caller_scope),
        custom_function_visibilities: functions.map_or(std::ptr::null(), |functions| functions.visibilities.as_ptr()),
        custom_function_visibility_count: functions.map_or(0, |functions| functions.visibilities.len()),
        callback_context: std::ptr::null_mut(),
        install_custom_properties: None,
        style_query_inputs: style_query.map_or(std::ptr::null(), std::ptr::from_ref),
        load_style_query_inputs: None,
        style_query_dependencies: style_query_references
            .map_or(std::ptr::null_mut(), |references| std::ptr::from_mut(references).cast()),
        note_substitution: None,
    }
}

/// The token stream a written value substitutes: itself, or the shorthand a longhand pending its
/// substitution takes its part of.
fn substituted_tokens(value: &StyleValueData) -> &StyleValueData {
    match value {
        StyleValueData::PendingSubstitution {
            original_shorthand_value,
        } => substituted_tokens(original_shorthand_value.data()),
        value => value,
    }
}

/// Whether a written value substitutes `attr()`, which reads the element's attributes.
pub(super) fn value_reads_attributes(value: &StyleValueData) -> bool {
    matches!(
        substituted_tokens(value),
        StyleValueData::Unresolved {
            presence_attr: true,
            ..
        }
    )
}

/// Whether a written value substitutes `inherit()`, which reads the custom-property environment
/// of the parent rather than the element's own.
pub(super) fn value_reads_inherited_values(value: &StyleValueData) -> bool {
    matches!(
        substituted_tokens(value),
        StyleValueData::Unresolved {
            presence_inherit: true,
            ..
        }
    )
}

/// Whether a written value substitutes `if()`, whose conditions read the document's media
/// features and the element's lengths beside its custom-property environment.
pub(super) fn value_reads_conditions(value: &StyleValueData) -> bool {
    matches!(
        substituted_tokens(value),
        StyleValueData::Unresolved { presence_if: true, .. }
    )
}

/// Whether a written value calls a custom function, whose result depends on the `@function`
/// definitions its scope sees and on the conditions inside them.
pub(super) fn value_calls_custom_functions(value: &StyleValueData) -> bool {
    matches!(
        substituted_tokens(value),
        StyleValueData::Unresolved {
            presence_dashed_function: true,
            ..
        }
    )
}

/// Whether a written value's substitution can come out as `revert` or `revert-layer`, which roll
/// its property back to the declarations it beat. A custom property never computes to a CSS-wide
/// keyword, so `var()` and `inherit()` substitute one only from a fallback written in the value,
/// as `env()` and the branches of `if()` do. A custom function's result is not written in the
/// value, so it may be one, and so may an attribute's text that an `attr()` parses with the
/// universal syntax; any other `attr()` parses it as a string or with a syntax no CSS-wide
/// keyword matches.
pub(super) fn value_may_substitute_revert(value: &StyleValueData) -> bool {
    fn is_universally_typed_attr(name: &[u16], arguments: &[ComponentValue]) -> bool {
        utf16_equals_ascii_case_insensitive(name, b"attr")
            && arguments.iter().any(|argument| {
                argument.function().is_some_and(|(name, syntax)| {
                    utf16_equals_ascii_case_insensitive(name, b"type")
                        && syntax.iter().any(|component| component.is_delim(b'*'))
                })
            })
    }
    fn may_yield_revert_keyword(values: &[ComponentValue]) -> bool {
        values.iter().any(|value| match &value.kind {
            ComponentKind::Function { name, values } => {
                is_universally_typed_attr(name, values) || may_yield_revert_keyword(values)
            }
            ComponentKind::SimpleBlock { values, .. } => may_yield_revert_keyword(values),
            ComponentKind::Token(_) => value.ident().is_some_and(|ident| {
                utf16_equals_ascii_case_insensitive(ident, b"revert")
                    || utf16_equals_ascii_case_insensitive(ident, b"revert-layer")
            }),
        })
    }
    match substituted_tokens(value) {
        StyleValueData::Unresolved {
            components,
            presence_dashed_function,
            ..
        } => *presence_dashed_function || may_yield_revert_keyword(components.as_slice()),
        _ => false,
    }
}

/// What a written value's substitutions read beyond the element's custom-property environment,
/// as `FfiNodeRecordReads` bits.
fn substitution_reads(value: &StyleValueData) -> u8 {
    let mut reads = 0;
    if value_calls_custom_functions(value) {
        reads |= bridge::FfiNodeRecordReads::CustomFunction as u8;
    }
    if value_reads_conditions(value) {
        reads |= bridge::FfiNodeRecordReads::IfFunction as u8;
    }
    if value_reads_attributes(value) {
        reads |= bridge::FfiNodeRecordReads::Attributes as u8;
    }
    if value_reads_inherited_values(value) {
        reads |= bridge::FfiNodeRecordReads::InheritFunction as u8;
    }
    reads
}

/// The custom-property environments a node's winners substitute under: its own, which `var()`
/// reads, and the one it inherits, which `inherit()` reads - the parent's, or the originating
/// element's for a pseudo-element.
#[derive(Clone, Copy)]
pub(super) struct SubstitutionEnvironment {
    pub own: u64,
    pub inherited: u64,
}

/// What a node's winners substitute against beside their written values: the document's inputs
/// and media features, the custom-property environments, and what is read of the element itself,
/// where a winner reads it - its attributes for `attr()`, the lengths and colors a `style()` query
/// in `if()` resolves against, and the custom functions its calls reach.
pub(super) struct SubstitutionInputs<'a> {
    pub document: &'a bridge::FfiDocumentStyleComputationInputs,
    pub media: &'a DocumentMediaSnapshot,
    pub environment: SubstitutionEnvironment,
    pub attributes: Option<&'a SubstitutionAttributes<'a>>,
    pub style_query: Option<&'a crate::css::cascaded_properties::FfiStyleQueryInputs>,
    /// Where the custom properties a `style()` query reads are noted.
    pub style_query_references:
        Option<&'a std::cell::RefCell<Option<Box<crate::css::custom_properties::StyleQueryDependencies>>>>,
    pub functions: Option<&'a PreparedCustomFunctions>,
}

/// What an `attr()` reads of an element, as the resolution takes it: each of the element's
/// attributes in no namespace, by local name, with its value's text, and whether names match ASCII
/// case-insensitively, as an HTML element's do. It points into the facts it borrows.
pub(super) struct SubstitutionAttributes<'a> {
    attributes: Vec<crate::css::custom_properties::FfiSubstitutionAttribute>,
    names_are_ascii_case_insensitive: bool,
    facts: std::marker::PhantomData<&'a index::ElementFactStore>,
}

impl<'a> SubstitutionAttributes<'a> {
    pub(super) fn of(facts: &'a index::ElementFactStore, node: StyleNodeID, html_namespace: StyleAtomID) -> Self {
        use crate::css::custom_properties::FfiSubstitutionAttribute;
        let view = |text: &[u16]| FfiUtf16View {
            ascii: std::ptr::null(),
            utf16: text.as_ptr(),
            length: text.len(),
        };
        Self {
            attributes: facts
                .substitution_attributes(node)
                .map(|(name, value)| FfiSubstitutionAttribute {
                    name: view(name),
                    value: view(value),
                })
                .collect(),
            names_are_ascii_case_insensitive: !html_namespace.is_none() && facts.namespace_of(node) == html_namespace,
            facts: std::marker::PhantomData,
        }
    }
}

impl RetainedState {
    /// Hand each of a node's element-target matches, with the cascade inputs its priority is
    /// computed from, to `visit`, stopping when it breaks. `None` when the node has no answer to read.
    fn try_for_each_element_match(
        &self,
        node: StyleNodeID,
        visit: impl FnMut(RuleID, TreeScopeID, Specificity, u32) -> ControlFlow<()>,
    ) -> Option<ControlFlow<()>> {
        self.try_for_each_match(node, None, visit)
    }

    /// The matches declaring custom properties in the answer a transaction publishes for the node,
    /// read before that answer is installed: the answers the lookups below find are the installed
    /// ones, which a node published in this transaction does not have yet, or has from before.
    pub(super) fn batch_custom_property_matches_of(
        &self,
        published: &PublishedMatchAnswers,
        answer: &PublishedMatchAnswer,
    ) -> Option<Vec<BatchCustomPropertyMatch>> {
        let mut matches = Vec::new();
        let mut push = |rule: RuleID,
                        tree_scope: TreeScopeID,
                        specificity: Specificity,
                        scope_proximity: u32,
                        pseudo: Option<u16>| {
            if !self.program.custom_declarations_of(rule).is_empty() {
                matches.push(BatchCustomPropertyMatch {
                    rule,
                    tree_scope,
                    specificity,
                    scope_proximity,
                    pseudo,
                });
            }
        };
        if let Some(published_matches) = published.matches_for(answer) {
            for entry in published_matches {
                push(
                    entry.rule,
                    entry.tree_scope,
                    entry.specificity,
                    entry.scope_proximity,
                    entry.pseudo_element.map(|target| target.kind.0),
                );
            }
        } else {
            for rule_match in self.match_answers.answer(answer.cascade_input?)?.iter() {
                let entry = &self.programs.get(rule_match.program).entries()[rule_match.entry as usize];
                push(
                    rule_match.rule,
                    rule_match.tree_scope,
                    entry.specificity,
                    rule_match.scope_proximity,
                    entry.pseudo_element.map(|target| target.kind.0),
                );
            }
        }
        Some(matches)
    }

    /// The element's matches when `pseudo` is `None`, else the matches for that pseudo-element.
    /// In a batch, those declaring custom properties alone.
    fn try_for_each_match(
        &self,
        node: StyleNodeID,
        pseudo: Option<u8>,
        mut visit: impl FnMut(RuleID, TreeScopeID, Specificity, u32) -> ControlFlow<()>,
    ) -> Option<ControlFlow<()>> {
        if let Some(matches) = self.batch_custom_property_matches.get(&node) {
            for entry in matches.iter().filter(|entry| entry.pseudo == pseudo.map(u16::from)) {
                if visit(entry.rule, entry.tree_scope, entry.specificity, entry.scope_proximity).is_break() {
                    return Some(ControlFlow::Break(()));
                }
            }
            return Some(ControlFlow::Continue(()));
        }
        self.try_for_each_answer_match(node, pseudo, visit)
    }

    /// Every match of the node's answer for the element when `pseudo` is `None`, else for that
    /// pseudo-element: the answer its transaction publishes, else the one it retains.
    pub(super) fn try_for_each_answer_match(
        &self,
        node: StyleNodeID,
        pseudo: Option<u8>,
        mut visit: impl FnMut(RuleID, TreeScopeID, Specificity, u32) -> ControlFlow<()>,
    ) -> Option<ControlFlow<()>> {
        let wanted =
            |target: Option<tree::PseudoElementTarget>| target.map(|target| target.kind.0) == pseudo.map(u16::from);
        if let Some((published, answer)) = Self::published_answer_lookup(
            &self.published_match_answers,
            self.batch_matching_traversal.as_deref(),
            node,
        ) && let Some(matches) = published.matches_for(answer)
        {
            for entry in matches.iter().filter(|entry| wanted(entry.pseudo_element)) {
                if visit(entry.rule, entry.tree_scope, entry.specificity, entry.scope_proximity).is_break() {
                    return Some(ControlFlow::Break(()));
                }
            }
            return Some(ControlFlow::Continue(()));
        }
        // An answer published by its identity alone names the matches the catalog holds for it;
        // the answer the node retains from before is not the one it is being published with.
        let published_identity = Self::published_answer_lookup(
            &self.published_match_answers,
            self.batch_matching_traversal.as_deref(),
            node,
        )
        .map(|(_, answer)| answer.cascade_input);
        let answer = match published_identity {
            Some(identity) => identity.and_then(|identity| self.match_answers.answer(identity))?,
            None => match self.retained_match_answer(node) {
                Lookup::Known(answer) => answer,
                _ => return None,
            },
        };
        for rule_match in answer.iter() {
            let entry = &self.programs.get(rule_match.program).entries()[rule_match.entry as usize];
            if !wanted(entry.pseudo_element) {
                continue;
            }
            if visit(
                rule_match.rule,
                rule_match.tree_scope,
                entry.specificity,
                rule_match.scope_proximity,
            )
            .is_break()
            {
                return Some(ControlFlow::Break(()));
            }
        }
        Some(ControlFlow::Continue(()))
    }

    /// Whether anything in the document declares a custom property. Nothing declaring one means
    /// every environment is the inherited one, and no cascade need look.
    pub(super) fn any_custom_property_is_declared(&self) -> bool {
        self.program.any_rule_declares_custom_properties() || self.facts.any_element_declares_custom_properties()
    }

    /// The environment the element's own values substitute under: `own`, the one its declarations
    /// resolve to, with what its animations sampled into its custom properties laid over it.
    pub(super) fn sampled_custom_property_environment(&mut self, node: StyleNodeID, own: u64) -> u64 {
        let Some((overlay, sampled_over)) = self
            .element_custom_property_data
            .get(&node)
            .and_then(|held| Some((held.identity, held.sampled_over?)))
        else {
            return own;
        };
        if sampled_over == own {
            return overlay;
        }
        self.custom_property_environments.overlay_moved_over(overlay, own)
    }

    /// Whether the element's animations sample custom properties, which its own values read.
    pub(super) fn element_samples_custom_properties(&self, node: StyleNodeID) -> bool {
        self.element_custom_property_data
            .get(&node)
            .is_some_and(|held| held.sampled_over.is_some())
    }

    pub(super) fn node_declares_custom_properties(&self, node: StyleNodeID) -> bool {
        if !self.any_custom_property_is_declared() {
            return false;
        }
        if !self.facts.element_custom_declarations(node).is_empty() {
            return true;
        }
        matches!(
            self.try_for_each_element_match(node, |rule, _, _, _| {
                if self.program.custom_declarations_of(rule).is_empty() {
                    ControlFlow::Continue(())
                } else {
                    ControlFlow::Break(())
                }
            }),
            Some(ControlFlow::Break(()))
        )
    }

    /// Whether a reaction on the node may move its custom-property environment, which its
    /// descendants inherit: it holds an environment of its own, or its cascade declares custom
    /// properties now. A node whose environment is its parent's and whose cascade declares none
    /// keeps the parent's whatever its reaction computes.
    pub(super) fn node_environment_may_move(&self, node: StyleNodeID) -> bool {
        let own = self.computed_group_sets.custom_property_environment_identity(node);
        let parent = self
            .tree
            .inheritance_parent(node)
            .map_or(Some(0), |parent| self.inherited_custom_property_environment(parent));
        own != parent || self.node_declares_custom_properties(node)
    }

    /// The environment the node's children inherit: the one behind its record, without the custom
    /// properties a registration keeps from inheriting.
    pub(super) fn inherited_custom_property_environment(&self, node: StyleNodeID) -> Option<u64> {
        let environment = self.computed_group_sets.custom_property_environment_identity(node)?;
        Some(self.custom_property_environments.inheritable(environment))
    }

    /// Whether the node's style reads custom properties: its cascade declares some, or a winner
    /// of its state was written with a substitution. A node whose reads are unknown reads.
    pub fn node_style_reads_custom_properties(&mut self, node: StyleNodeID) -> bool {
        // Published substitution usage also includes the pseudo styles C++ computes, whose
        // reads are not represented by the element's own winner state.
        if self.facts.uses_unnamed_custom_properties(node) || self.node_declares_custom_properties(node) {
            return true;
        }
        let Lookup::Known((_, state)) = self
            .current_winner_groups()
            .token_for(WinnerGroupKey::current(node, self.program.version()))
        else {
            return true;
        };
        self.state_has_substitutions(node, state)
    }

    /// Whether the node's winner inventory is complete once custom properties are set aside: the
    /// engine computes an environment from those itself, and a rule declaring them is otherwise as
    /// complete as any, for the element and for each of its pseudo-elements alike.
    pub(super) fn cascade_winners_are_complete_but_for_custom_properties(&self, node: StyleNodeID) -> bool {
        if let Some(&complete) = self.batch_answers_complete_but_for_custom_properties.get(&node) {
            return complete;
        }
        if let Some((published, answer)) = Self::published_answer_lookup(
            &self.published_match_answers,
            self.batch_matching_traversal.as_deref(),
            node,
        ) && let Some(matches) = published.matches_for(answer)
        {
            return matches
                .iter()
                .all(|entry| self.match_is_complete_but_for_custom_properties(node, entry.rule));
        }
        let Lookup::Known(answer) = self.retained_match_answer(node) else {
            return false;
        };
        self.retained_matches_are_complete_but_for_custom_properties(node, answer)
    }

    /// What `cascade_winners_are_complete_but_for_custom_properties` says of the answer a
    /// transaction publishes for the node, read before that answer is installed. `None` when the
    /// answer holds neither its matches nor an identity the catalog materializes.
    pub(super) fn answer_is_complete_but_for_custom_properties(
        &self,
        node: StyleNodeID,
        published: &PublishedMatchAnswers,
        answer: &PublishedMatchAnswer,
    ) -> Option<bool> {
        if let Some(matches) = published.matches_for(answer) {
            return Some(
                matches
                    .iter()
                    .all(|entry| self.match_is_complete_but_for_custom_properties(node, entry.rule)),
            );
        }
        let matches = self.match_answers.answer(answer.cascade_input?)?;
        Some(self.retained_matches_are_complete_but_for_custom_properties(node, matches))
    }

    fn retained_matches_are_complete_but_for_custom_properties(
        &self,
        node: StyleNodeID,
        matches: &[RetainedRuleMatch],
    ) -> bool {
        matches
            .iter()
            .all(|rule_match| self.match_is_complete_but_for_custom_properties(node, rule_match.rule))
    }

    /// Whether the winners the cascade publishes hold a match: they hold its container conditions
    /// (`container_gate_is_held`). Its custom properties are the environment's.
    pub(super) fn match_is_complete_but_for_custom_properties(&self, node: StyleNodeID, rule: RuleID) -> bool {
        self.container_gate_is_held(Some(node), rule)
    }

    /// The custom properties the node's cascade decides, each with its winning declaration and
    /// the value it was written with, in the order the C++ cascade lists them: by first
    /// appearance, applying blocks from the lowest priority up, a later declaration of a name
    /// replacing the earlier in place. `None` when the node has no answer to cascade from, or a
    /// declaration arrived without its written value.
    fn cascaded_custom_declarations(
        &self,
        node: StyleNodeID,
    ) -> Option<Vec<(CustomDeclaration, RetainedStyleValueData)>> {
        self.cascaded_custom_declarations_of(node, None)
    }

    /// What `cascaded_custom_declarations` says of the element, for one of its pseudo-elements:
    /// its rules alone, since a pseudo-element has no declarations of its own.
    fn cascaded_custom_declarations_of(
        &self,
        node: StyleNodeID,
        pseudo: Option<u8>,
    ) -> Option<Vec<(CustomDeclaration, RetainedStyleValueData)>> {
        self.cascade_custom_declarations(node, pseudo, None)
    }

    /// What a pseudo-element's custom declarations cascade to, from the matches being published
    /// for it rather than from a published answer. `None` when a declaration arrived without its
    /// written value.
    pub(super) fn cascaded_pseudo_custom_declarations_in(
        &self,
        node: StyleNodeID,
        matches: &[RuleMatch],
        pseudo: tree::PseudoElementTarget,
    ) -> Option<Vec<CustomDeclaration>> {
        let kind = u8::try_from(pseudo.kind.0).ok()?;
        let cascaded = self.cascade_custom_declarations(node, Some(kind), Some(matches))?;
        Some(cascaded.into_iter().map(|(declared, _)| declared).collect())
    }

    pub(super) fn cascade_custom_declarations(
        &self,
        node: StyleNodeID,
        pseudo: Option<u8>,
        matches: Option<&[RuleMatch]>,
    ) -> Option<Vec<(CustomDeclaration, RetainedStyleValueData)>> {
        struct Candidate<'a> {
            priority: CascadePriority,
            stratum: CascadeStratum,
            declared: CustomDeclaration,
            written: &'a RetainedStyleValueData,
        }

        let mut candidates = Vec::new();
        let mut visit = |rule: RuleID, tree_scope: TreeScopeID, specificity: Specificity, scope_proximity: u32| {
            let declared = self.program.custom_declarations_of(rule);
            if declared.is_empty() {
                return ControlFlow::Continue(());
            }
            // A gated rule declares for the node where its container conditions held when its
            // winners were published, as its longhands do.
            if !self.published_container_verdict_holds(node, rule, pseudo.is_some()) {
                return ControlFlow::Continue(());
            }
            let written = self.program.custom_written_values_of(rule);
            if written.len() != declared.len() {
                return ControlFlow::Break(());
            }
            let mut priority_and_stratum_by_importance = [None; 2];
            for (&declared, written) in declared.iter().zip(written) {
                let (priority, stratum) = *priority_and_stratum_by_importance[declared.important as usize]
                    .get_or_insert_with(|| {
                        (
                            self.cascade_priority_of(
                                node,
                                rule,
                                tree_scope,
                                specificity,
                                scope_proximity,
                                declared.important,
                            ),
                            self.cascade_stratum_of(node, rule, tree_scope, declared.important),
                        )
                    });
                candidates.push(Candidate {
                    priority,
                    stratum,
                    declared,
                    written,
                });
            }
            ControlFlow::Continue(())
        };
        let result = match matches {
            Some(matches) => {
                let wanted = pseudo.map(u16::from);
                let mut result = ControlFlow::Continue(());
                for entry in matches
                    .iter()
                    .filter(|entry| entry.pseudo_element.map(|target| target.kind.0) == wanted)
                {
                    result = visit(entry.rule, entry.tree_scope, entry.specificity, entry.scope_proximity);
                    if result.is_break() {
                        break;
                    }
                }
                result
            }
            None => self.try_for_each_match(node, pseudo, &mut visit)?,
        };
        if result.is_break() {
            return None;
        }
        let element_declarations = pseudo
            .is_none()
            .then(|| self.facts.element_custom_declarations_written(node))
            .flatten();
        let mut priority_and_stratum_by_importance = [None; 2];
        for (&declared, written) in element_declarations
            .into_iter()
            .flat_map(index::ElementCustomDeclarations::iter)
        {
            let (priority, stratum) = *priority_and_stratum_by_importance[declared.important as usize]
                .get_or_insert_with(|| {
                    (
                        self.element_cascade_priority(node, ElementDeclarationKind::InlineStyle, declared.important),
                        self.element_cascade_stratum(node, ElementDeclarationKind::InlineStyle, declared.important),
                    )
                });
            candidates.push(Candidate {
                priority,
                stratum,
                declared,
                written,
            });
        }
        if let [candidate] = candidates.as_slice() {
            return Some(if candidate.stratum.ceiling(candidate.declared.operator).is_none() {
                vec![(candidate.declared, candidate.written.clone_retained())]
            } else {
                Vec::new()
            });
        }
        // Keep insertion order for declarations with equal cascade priority.
        candidates.sort_by_key(|candidate| candidate.priority);
        let mut name_indices = HashMap::default();
        let mut candidates_by_name: Vec<Vec<Candidate>> = Vec::new();
        for candidate in candidates {
            let index = *name_indices.entry(candidate.declared.name).or_insert_with(|| {
                candidates_by_name.push(Vec::new());
                candidates_by_name.len() - 1
            });
            candidates_by_name[index].push(candidate);
        }
        let mut cascaded = Vec::with_capacity(candidates_by_name.len());
        let mut ceilings = Vec::new();
        for candidates in candidates_by_name {
            ceilings.clear();
            for candidate in candidates.into_iter().rev() {
                if !ceilings.iter().all(|&ceiling| candidate.stratum.is_below(ceiling)) {
                    continue;
                }
                let Some(ceiling) = candidate.stratum.ceiling(candidate.declared.operator) else {
                    cascaded.push((candidate.declared, candidate.written.clone_retained()));
                    break;
                };
                ceilings.push(ceiling);
            }
        }
        Some(cascaded)
    }

    fn environment_inputs(
        parent: u64,
        registration_generation: u64,
        cascaded: &[(CustomDeclaration, RetainedStyleValueData)],
    ) -> custom_property_environments::EnvironmentInputs {
        custom_property_environments::EnvironmentInputs {
            parent,
            registration_generation,
            cascaded: cascaded
                .iter()
                .map(|(declared, written)| CascadedCustomProperty {
                    name: declared.name,
                    important: declared.important,
                    written_value: written.pointer() as usize,
                })
                .collect(),
        }
    }

    /// Remember the environment C++ resolved for a node's custom declarations, so a later engine
    /// derivation of the node, or of an element alike in its declarations, takes that environment
    /// rather than resolving an equal one under an identity of its own.
    pub(super) fn remember_cpp_custom_property_environment(&mut self, node: StyleNodeID, environment: u64) {
        if environment == 0 || environment & custom_property_environments::ENGINE_ENVIRONMENT_IDENTITY_BIT != 0 {
            return;
        }
        let inputs = self.document_style_computation_inputs;
        if !self.node_declares_custom_properties(node) {
            return;
        }
        let Some(cascaded) = self.cascaded_custom_declarations(node) else {
            return;
        };
        if cascaded.is_empty() {
            return;
        }
        // What an `attr()`, an `if()` or a custom function call resolves to is the element's
        // alone, which no memo hands to another.
        if cascaded.iter().any(|(_, written)| {
            value_reads_attributes(written.data())
                || value_reads_conditions(written.data())
                || value_calls_custom_functions(written.data())
        }) {
            return;
        }
        let parent_environment = match self.tree.inheritance_parent(node) {
            Some(parent) => match self.computed_group_sets.custom_property_environment_identity(parent) {
                Some(parent_environment) => parent_environment,
                None => return,
            },
            None => 0,
        };
        let key = Self::environment_inputs(
            parent_environment,
            inputs.custom_property_registration_generation,
            &cascaded,
        );
        if self.custom_property_environments.memoized(&key).is_none() {
            let written_values = cascaded.into_iter().map(|(_, written)| written).collect();
            self.custom_property_environments
                .remember(key, environment, written_values);
        }
    }

    /// What a `style()` query of an `if()` resolves against for a node or one of its
    /// pseudo-elements, as the host takes it: the style the target holds, else the one its parent
    /// holds, which for a pseudo-element is its originating element's, else the document's initial
    /// font, preferred color scheme and initial color.
    pub(super) fn style_query_inputs(
        &self,
        target: computed::ComputedStyleTarget,
    ) -> Option<crate::css::cascaded_properties::FfiStyleQueryInputs> {
        let node = target.node();
        let parent_record = || {
            if target.pseudo_kind() == u8::MAX {
                self.computed_group_sets
                    .assigned_style_record(self.tree.inheritance_parent(node)?)
            } else {
                self.computed_group_sets.assigned_style_record(node)
            }
        };
        let held = self
            .computed_group_sets
            .assigned_final_style_record(target)
            .or_else(parent_record)
            .and_then(|record| self.computed_group_sets.style_record_view(record.raw()));
        if let Some(view) = held
            && let Some(length) = self.record_length_resolution_context(&view)
        {
            let values = crate::css::computed_value_views::ComputedValuesView::new(
                crate::css::host_shared::SharedPayload::as_pointer_slice(view.payloads),
            );
            return Some(crate::css::cascaded_properties::FfiStyleQueryInputs {
                length,
                color_scheme: values.inherited_ui().color_scheme,
                current_color: values.inherited_text().color,
            });
        }
        Some(crate::css::cascaded_properties::FfiStyleQueryInputs {
            length: *self.document_media.length()?,
            color_scheme: self.document_style_computation_inputs.preferred_color_scheme,
            // Opaque black, the initial color.
            current_color: 0xff00_0000,
        })
    }

    /// Whether the custom declarations of a node, or of one of its pseudo-elements, name a
    /// registered custom property: its drive then waits for the font it settles before resolving
    /// them, as `FullDrive::AwaitsRegisteredContext` says.
    pub(super) fn declares_registered_custom_property(
        &self,
        node: StyleNodeID,
        pseudo: Option<u8>,
        inputs: &bridge::FfiDocumentStyleComputationInputs,
    ) -> bool {
        inputs.custom_property_registry().has_registrations()
            && self
                .cascaded_custom_declarations_of(node, pseudo)
                .is_some_and(|cascaded| self.declarations_name_a_registered_custom_property(&cascaded, inputs))
    }

    pub(super) fn declarations_name_a_registered_custom_property(
        &self,
        cascaded: &[(CustomDeclaration, RetainedStyleValueData)],
        inputs: &bridge::FfiDocumentStyleComputationInputs,
    ) -> bool {
        let registry = inputs.custom_property_registry();
        registry.has_registrations()
            && cascaded.iter().any(|(declared, _)| {
                self.declared_custom_property_name(declared.name)
                    .is_some_and(|name| registry.name_is_registered(&name.text))
            })
    }

    pub(super) fn declares_custom_property_registered_with_syntax(
        &self,
        node: StyleNodeID,
        inputs: &bridge::FfiDocumentStyleComputationInputs,
    ) -> bool {
        let registry = inputs.custom_property_registry();
        registry.has_registrations()
            && self
                .cascaded_custom_declarations_of(node, None)
                .is_some_and(|cascaded| {
                    cascaded.iter().any(|(declared, _)| {
                        self.declared_custom_property_name(declared.name)
                            .is_some_and(|name| registry.name_has_syntax(&name.text))
                    })
                })
    }

    /// What a registered custom property computes against in a record's element: the record's
    /// lengths, and the color scheme its table settled. `None` for a record without a font.
    pub(super) fn record_registered_value_context(
        &self,
        record: computed::FinalStyleRecordID,
    ) -> Option<RegisteredValueContext> {
        let view = self.computed_group_sets.style_record_view(record.raw())?;
        let length = self.record_length_resolution_context(&view)?;
        let table = unsafe { view.longhand_table.as_ref() }?;
        Some(RegisteredValueContext {
            length,
            color_scheme: u8::try_from(table.effective_color_scheme())
                .unwrap_or(self.document_style_computation_inputs.preferred_color_scheme),
        })
    }

    /// What a registered custom property computes against where the caller brings nothing
    /// better: the record the node holds, as a row keeping its font reads it, else the one its
    /// parent holds, as a first record's provisional environment reads it before the drive
    /// settles its font, else the document's initial font. A pseudo-element's is its originating
    /// element's.
    pub(super) fn standing_registered_value_context(
        &self,
        node: StyleNodeID,
        pseudo: Option<u8>,
    ) -> RegisteredValueContext {
        let own = self.computed_group_sets.assigned_style_record(node);
        let record = match pseudo {
            Some(_) => own,
            None => own.or_else(|| {
                self.tree
                    .inheritance_parent(node)
                    .and_then(|parent| self.computed_group_sets.assigned_style_record(parent))
            }),
        };
        if let Some(context) = record.and_then(|record| self.record_registered_value_context(record)) {
            return context;
        }
        use crate::css::style_compute::{FfiFontMetrics, FfiLengthResolutionContext};
        let inputs = &self.document_style_computation_inputs;
        let initial = FfiFontMetrics {
            font_size: inputs.initial_font_size,
            x_height: inputs.initial_font_x_height,
            cap_height: inputs.initial_font_cap_height,
            zero_advance: inputs.initial_font_zero_advance,
            line_height: 0.0,
        };
        RegisteredValueContext {
            length: FfiLengthResolutionContext {
                viewport_width: inputs.viewport_width,
                viewport_height: inputs.viewport_height,
                font_metrics: initial,
                root_font_metrics: initial,
                font_metrics_depend_on_viewport_metrics: false,
                root_font_metrics_depend_on_viewport_metrics: false,
                has_container_width_basis: false,
                has_container_height_basis: false,
                container_width_basis: 0.0,
                container_height_basis: 0.0,
                container_width_basis_depends_on_viewport_metrics: false,
                container_height_basis_depends_on_viewport_metrics: false,
                subject_inline_axis_is_horizontal: true,
                resolved_viewport_relative_length: std::ptr::null_mut(),
            },
            color_scheme: inputs.preferred_color_scheme,
        }
    }

    /// What the custom function calls in the values of a node, or of one of its pseudo-elements,
    /// resolve against: the functions its tree scope's calls reach, each with the blocks of
    /// declarations whose `@media` conditions hold for the transaction and whose container
    /// conditions hold for the node. `None` when the engine cannot decide a container condition.
    pub(super) fn prepare_custom_functions(
        &self,
        node: StyleNodeID,
        pseudo: Option<u8>,
    ) -> Option<PreparedCustomFunctions> {
        let (caller_scope, visible) = self.document_functions.scope_functions(self.tree.tree_scope(node));
        let visible_definitions = visible.map_or(&[][..], |visible| visible.definitions.as_slice());
        let mut container_effects: Option<super::container_queries::ContainerVerdict> = None;
        let mut reads_attributes = false;
        let mut declarations = Vec::with_capacity(visible_definitions.len());
        for (function, _, selected_inputs) in visible_definitions {
            reads_attributes |= function
                .signature
                .parameters
                .iter()
                .filter_map(|parameter| parameter.default_value.as_deref())
                .any(value_reads_attributes);
            let mut selected = Vec::new();
            for input in selected_inputs.iter().map(|&index| &function.inputs[index]) {
                if !input.containers.is_empty() {
                    let verdict = self.container_conditions_verdict(&input.containers, node, pseudo.is_some())?;
                    let matches = verdict.matches;
                    container_effects.get_or_insert_default().add_reads(verdict);
                    if !matches {
                        continue;
                    }
                }
                for descriptor in &input.declarations.descriptors {
                    reads_attributes |= value_reads_attributes(&descriptor.value);
                    let name = descriptor.name.units();
                    selected.push(FfiSubstitutionFunctionDeclaration {
                        name: FfiUtf16View {
                            ascii: std::ptr::null(),
                            utf16: name.as_ptr(),
                            length: name.len(),
                        },
                        data: Arc::as_ptr(&descriptor.value).cast(),
                    });
                }
            }
            declarations.push(selected);
        }
        let definitions = visible_definitions
            .iter()
            .zip(&declarations)
            .map(
                |((function, scope, _), declarations)| FfiSubstitutionFunctionDefinition {
                    identity: function.identity,
                    scope_identity: *scope,
                    signature: Arc::as_ptr(&function.signature).cast(),
                    declarations: declarations.as_ptr(),
                    declaration_count: declarations.len(),
                },
            )
            .collect();
        Some(PreparedCustomFunctions {
            _declarations: declarations,
            definitions,
            visibilities: visible.map_or_else(Vec::new, |visible| visible.visibilities.clone()),
            caller_scope,
            reads_attributes,
            container_effects,
        })
    }

    /// The name a cascaded custom declaration names, as its store entry keys it. A block's
    /// publication notes every custom property name it declares before the block is set, so a
    /// live declaration's name is always known.
    fn declared_custom_property_name(&self, name: StyleAtomID) -> Option<&CustomPropertyName> {
        let noted = self.custom_property_environments.name(name);
        debug_assert!(
            noted.is_some(),
            "a custom declaration's name is noted at its publication"
        );
        noted.filter(|noted| noted.raw.raw() != 0)
    }

    /// The store behind an environment a node inherits, null for the empty one. A record that
    /// names an environment keeps its store alive, and C++ makes a store for every environment,
    /// so a parent's is always held. `None` if it is not anyway: an environment built over no
    /// store would drop every custom property the node inherits.
    fn inherited_environment_store(&self, environment: u64) -> Option<*const c_void> {
        if environment == 0 {
            return Some(std::ptr::null());
        }
        let store = self.custom_property_environments.store(environment);
        debug_assert!(store.is_some(), "an inherited environment keeps its store");
        store
    }

    /// The environment of a node the engine computes a record for: the one it inherits of its
    /// parent's when its cascade declares no custom property, else what its declarations resolve
    /// to over that one. What a node inherits is the parent's environment without the custom
    /// properties a registration keeps from inheriting, while `inherit` reads the parent's whole.
    /// A registered name computes against `registered`, else against what
    /// `standing_registered_value_context` says. Refused when an input is missing.
    pub(super) fn engine_custom_property_environment(
        &mut self,
        node: StyleNodeID,
        parent_environment: u64,
        inputs: &bridge::FfiDocumentStyleComputationInputs,
        registered: Option<&RegisteredValueContext>,
    ) -> Drive<u64> {
        self.engine_custom_property_environment_of(node, None, parent_environment, inputs, registered)
    }

    /// What `engine_custom_property_environment` says of the element, for one of its
    /// pseudo-elements over the element's own environment.
    pub(super) fn engine_custom_property_environment_of(
        &mut self,
        node: StyleNodeID,
        pseudo: Option<u8>,
        parent_environment: u64,
        inputs: &bridge::FfiDocumentStyleComputationInputs,
        registered: Option<&RegisteredValueContext>,
    ) -> Drive<u64> {
        if !self.any_custom_property_is_declared() {
            if pseudo.is_none() {
                self.custom_declaration_reads.remove(&node);
            }
            return Ok(self.custom_property_environments.inheritable(parent_environment));
        }
        // A node the engine drives has the match answer its winners came from, and a published
        // block carries a written value for each custom declaration, so the cascade answers. One
        // that does not is refused rather than resolved without the node's own declarations.
        let cascaded = self.cascaded_custom_declarations_of(node, pseudo);
        debug_assert!(cascaded.is_some(), "a driven node's custom declarations cascade");
        let Some(cascaded) = cascaded else {
            self.counters.bump(Counter::EngineCustomPropertyEnvironmentBails);
            return Err(Unanswered::Refused);
        };
        self.note_custom_declaration_reads(node, pseudo, &cascaded);
        self.engine_custom_property_environment_over(node, pseudo, cascaded, parent_environment, inputs, registered)
    }

    /// Note what the custom declarations cascaded for an element or one of its pseudo-elements
    /// read beyond the environment they resolve over, whatever they resolve to: the host notes it
    /// beside the element's record. The element's own resolve first, and its pseudo-elements'
    /// only add to what they found.
    pub(super) fn note_custom_declaration_reads(
        &mut self,
        node: StyleNodeID,
        pseudo: Option<u8>,
        cascaded: &[(CustomDeclaration, RetainedStyleValueData)],
    ) {
        let reads = cascaded
            .iter()
            .fold(0, |reads, (_, value)| reads | substitution_reads(value.data()));
        match (reads, pseudo) {
            (0, None) => {
                self.custom_declaration_reads.remove(&node);
            }
            (0, Some(_)) => {}
            (reads, None) => {
                self.custom_declaration_reads.insert(node, reads);
            }
            (reads, Some(_)) => *self.custom_declaration_reads.entry(node).or_default() |= reads,
        }
    }

    /// What `engine_custom_property_environment_of` says of custom declarations the caller
    /// cascaded for the node or one of its pseudo-elements; `attr()` among them reads the node's
    /// attributes.
    pub(super) fn engine_custom_property_environment_over(
        &mut self,
        node: StyleNodeID,
        pseudo: Option<u8>,
        cascaded: Vec<(CustomDeclaration, RetainedStyleValueData)>,
        parent_environment: u64,
        inputs: &bridge::FfiDocumentStyleComputationInputs,
        registered: Option<&RegisteredValueContext>,
    ) -> Drive<u64> {
        let inherited_environment = self.custom_property_environments.inheritable(parent_environment);
        if cascaded.is_empty() {
            return Ok(inherited_environment);
        }
        let registry_ref = inputs.custom_property_registry();
        // A registered name computes against the element's own font and viewport, so what it
        // resolves to is the element's alone and takes no memo.
        let registered = self
            .declarations_name_a_registered_custom_property(&cascaded, inputs)
            .then(|| {
                registered
                    .copied()
                    .unwrap_or_else(|| self.standing_registered_value_context(node, pseudo))
            });
        // A custom function call reads the definitions its scope sees, with the blocks of
        // declarations their conditions select for the element. What those conditions read of
        // the containers is noted once the environment resolves.
        let mut container_effects = None;
        let functions = if cascaded
            .iter()
            .any(|(_, value)| value_calls_custom_functions(value.data()))
        {
            // A container condition the engine cannot decide asks about an ancestor the host styles
            // in this update: the row waits for it.
            let Some(mut functions) = self.prepare_custom_functions(node, pseudo) else {
                self.counters.bump(Counter::EngineComputedRecordBailRecordParent);
                return Err(Unanswered::AwaitsParent);
            };
            container_effects = functions.container_effects.take();
            Some(functions)
        } else {
            None
        };
        // An `attr()` among the declarations, or in the declarations of a function they call,
        // reads the element's attributes, so what they resolve to is the element's alone and
        // takes no memo.
        let reads_attributes = functions.as_ref().is_some_and(|functions| functions.reads_attributes)
            || cascaded.iter().any(|(_, value)| value_reads_attributes(value.data()));
        // So does an `if()`, whose conditions read the document's media features and the
        // element's lengths, and so does a function call, which may hold one.
        let reads_conditions =
            functions.is_some() || cascaded.iter().any(|(_, value)| value_reads_conditions(value.data()));
        let memoizes = !reads_attributes && !reads_conditions && registered.is_none();
        let key = Self::environment_inputs(
            parent_environment,
            inputs.custom_property_registration_generation,
            &cascaded,
        );
        // An environment C++ resolved for an element alike in its declarations is C++'s own
        // identity, and the host installs no record under one it does not recognise as the
        // engine's: handing it back settles a row the host then computes again. The memo is worth
        // only what it saves, so where it holds such an identity this resolves one of its own.
        let memoized = self.custom_property_environments.memoized(&key);
        let keeps_cpp_environment = memoized.is_some_and(|identity| {
            identity != inherited_environment
                && identity & custom_property_environments::ENGINE_ENVIRONMENT_IDENTITY_BIT == 0
        });
        if memoizes && let Some(identity) = memoized.filter(|_| !keeps_cpp_environment) {
            self.counters.bump(Counter::EngineCustomPropertyEnvironmentMemoHits);
            return Ok(identity);
        }
        let (Some(parent_store), Some(inheritance_store)) = (
            self.inherited_environment_store(inherited_environment),
            self.inherited_environment_store(parent_environment),
        ) else {
            self.counters.bump(Counter::EngineCustomPropertyEnvironmentBails);
            return Err(Unanswered::Refused);
        };
        let parent = unsafe { parent_store.cast::<CustomPropertyStore>().as_ref() };
        let mut values = Vec::with_capacity(cascaded.len());
        for (declared, value) in &cascaded {
            let Some(name) = self.declared_custom_property_name(declared.name) else {
                continue;
            };
            // A value the parent already holds, by identity, declares nothing new.
            if parent.is_some_and(|parent| parent.value_is_identical(name.raw.raw(), value.pointer().cast())) {
                continue;
            }
            values.push((
                name.raw.raw(),
                name.text.clone(),
                declared.important,
                value.pointer().cast(),
            ));
        }
        if values.is_empty() {
            if let Some(effects) = container_effects {
                self.note_container_effects_for_host(node, effects);
            }
            if memoizes && !keeps_cpp_environment {
                let written_values = cascaded.into_iter().map(|(_, written)| written).collect();
                self.custom_property_environments
                    .remember(key, inherited_environment, written_values);
            }
            return Ok(inherited_environment);
        }
        // The attributes an `attr()` among the declarations reads.
        let attributes =
            reads_attributes.then(|| SubstitutionAttributes::of(&self.facts, node, self.html_element_namespace));
        // SAFETY: The parent store is live for as long as a record names its environment, and the
        // values are the program's interned values, live for the call.
        let cascaded_store = unsafe { CustomPropertyStore::cascaded_child(parent_store, values) };
        let mut random_function_index = 0_usize;
        let mut parse_context = registry_ref.parse_context(&mut random_function_index);
        parse_context.in_quirks_mode = inputs.in_quirks_mode;
        let media_environment = self.document_media.environment();
        let style_query = reads_conditions
            .then(|| self.style_query_inputs(computed::ComputedStyleTarget::new(node, pseudo.unwrap_or(u8::MAX))))
            .flatten();
        // The custom properties a `style()` query reads are the element's style query references,
        // which the host records with its record.
        let mut style_query_references = None;
        let resolution_context = engine_resolution_context(
            &parse_context,
            cascaded_store,
            inheritance_store,
            std::ptr::from_ref(registry_ref).cast(),
            attributes.as_ref(),
            &media_environment,
            style_query.as_ref(),
            Some(&mut style_query_references),
            functions.as_ref(),
        );
        // A registered value computes as the host computes one: against the element's lengths with
        // its query containers' sizes, its place among its siblings, and the random base values
        // drawn for the element, which the engine keeps. A pseudo-element's are its element's.
        let length = registered.as_ref().map(|registered| {
            let mut length = registered.length;
            self.container_unit_bases(node).apply_to(&mut length);
            length
        });
        let tree_counting = registered
            .is_some()
            .then(|| self.sibling_position(node))
            .flatten()
            .map(|position| (u64::from(position.count), u64::from(position.index)));
        let mut random_base_values = (node, &mut self.random_base_values);
        let finalization = CustomPropertyFinalization {
            length: length.as_ref(),
            tree_counting,
            random_base_values: &[],
            color_scheme: registered.as_ref().map_or(0, |registered| registered.color_scheme),
            draw_random_base_value: Some((
                draw_engine_random_base_value,
                std::ptr::from_mut(&mut random_base_values).cast(),
            )),
        };
        let drive = FfiCustomPropertyDriveInput {
            store: cascaded_store,
            resolved_parent_store: parent_store,
            reuse_resolved_parent_if_empty: !parent_store.is_null(),
            resolution_context: &raw const resolution_context,
            finalization_length_resolution_context: std::ptr::null(),
            finalization_environment: std::ptr::null(),
            finalization_color_scheme: 0,
            draw_random_base_value: None,
            random_base_context: std::ptr::null_mut(),
        };
        // SAFETY: Every pointer the drive reads is live for the call.
        let (resolved, reads) = unsafe { resolve_declared_custom_properties(&drive, finalization) };
        // The resolved values live in the store; the listing transfers references of its own.
        let properties = match resolved.count {
            0 => &[],
            count => unsafe { std::slice::from_raw_parts(resolved.properties, count) },
        };
        for property in properties {
            unsafe { release_style_value(property.data.cast()) };
        }
        unsafe { destroy_resolved_custom_properties(resolved.storage, resolved.count) };
        unsafe { Arc::decrement_strong_count(cascaded_store.cast::<CustomPropertyStore>()) };
        self.counters.bump(Counter::EngineCustomPropertyEnvironmentsResolved);
        // What the registered values read beyond the element's fonts reaches the element's
        // records as what they read themselves does: a sibling change, a container's size.
        if reads.sibling_position {
            *self.custom_declaration_reads.entry(node).or_default() |=
                bridge::FfiNodeRecordReads::SiblingPosition as u8;
        }
        if reads.container_unit_mask != 0
            && let Some(length) = length
        {
            self.note_container_unit_reads_for_host(
                node,
                reads.container_unit_mask,
                length.subject_inline_axis_is_horizontal,
            );
        }
        if let Some(effects) = container_effects {
            self.note_container_effects_for_host(node, effects);
        }
        if style_query_references.is_some() {
            self.note_container_effects_for_host(
                node,
                super::container_queries::ContainerVerdict {
                    style_query_references,
                    ..Default::default()
                },
            );
        }
        let identity = if resolved.rust_store.is_null() {
            inherited_environment
        } else {
            unsafe {
                self.custom_property_environments.adopt_engine_environment(
                    resolved.rust_store,
                    inherited_environment,
                    parent_store,
                    registry_ref,
                )
            }
        };
        // A viewport change reaches the records under the environment. One the element shares with
        // its parent is the parent's too, which then restyles with it.
        if reads.viewport {
            self.custom_property_environments.note_reads_viewport(identity);
        }
        if memoizes && !keeps_cpp_environment {
            let written_values = cascaded.into_iter().map(|(_, written)| written).collect();
            self.custom_property_environments
                .remember(key, identity, written_values);
        }
        Ok(identity)
    }

    /// What a node's records read beyond their cascade, as `FfiNodeRecordReads` bits, for the row
    /// that installs them: an `attr()`, `inherit()`, `if()` or custom function call in the
    /// element's winners, in its pseudo-elements' (which read the originating element's
    /// attributes), or in the custom properties either declares, as their resolution found; a
    /// tree-counting function in the records the engine derived for the element or its
    /// pseudo-elements, written or substituted, as their installation noted.
    pub(super) fn node_record_reads(&self, node: StyleNodeID) -> u8 {
        let groups = self.current_winner_groups();
        let state = match groups.token_for(WinnerGroupKey::current(node, self.program.version())) {
            Lookup::Known((_, state)) => Some(state),
            _ => None,
        };
        let mut reads = self.custom_declaration_reads.get(&node).copied().unwrap_or(0);
        for state in state
            .into_iter()
            .chain(groups.pseudo_states(node).map(|(_, _, state, _)| state))
        {
            let state_reads = self.state_reads(node, state);
            if state_reads & cascade::STATE_READS_ATTRIBUTES != 0 {
                reads |= bridge::FfiNodeRecordReads::Attributes as u8;
            }
            if state_reads & cascade::STATE_READS_INHERIT_FUNCTION != 0 {
                reads |= bridge::FfiNodeRecordReads::InheritFunction as u8;
            }
            if state_reads & cascade::STATE_READS_IF_FUNCTION != 0 {
                reads |= bridge::FfiNodeRecordReads::IfFunction as u8;
            }
            if state_reads & cascade::STATE_READS_CUSTOM_FUNCTION != 0 {
                reads |= bridge::FfiNodeRecordReads::CustomFunction as u8;
            }
        }
        if self.nodes_with_tree_counting_records.contains_key(&node) {
            reads |= bridge::FfiNodeRecordReads::SiblingPosition as u8;
        }
        reads
    }

    /// What a keyframe's written value substitutes to on the element being sampled, against the
    /// custom-property store the element holds and the one it inherits from, which the host hands
    /// over: what the host's `resolve_unresolved_style_value` makes of it.
    /// `root_custom_property_name` names the custom property the value is written for, and is
    /// empty for a longhand. Like a cascaded declaration, a value that does not substitute or parse
    /// is guaranteed-invalid, and so is one calling a custom function whose container condition the
    /// engine cannot decide. The custom properties a `style()` query reads are noted in
    /// `style_query_references`.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn substitute_keyframe_value(
        &self,
        node: StyleNodeID,
        pseudo_kind: Option<u8>,
        store: *const c_void,
        inheritance_store: *const c_void,
        property: u16,
        root_custom_property_name: &[u16],
        written: &RetainedStyleValueData,
        style_query_references: &std::cell::RefCell<Option<Box<crate::css::custom_properties::StyleQueryDependencies>>>,
    ) -> RetainedStyleValueData {
        let calls_functions = value_calls_custom_functions(written.data());
        let functions = calls_functions
            .then(|| self.prepare_custom_functions(node, pseudo_kind))
            .flatten();
        let reads_attributes = value_reads_attributes(written.data())
            || functions.as_ref().is_some_and(|functions| functions.reads_attributes);
        let attributes = reads_attributes.then(|| {
            SubstitutionAttributes::of(
                &self.facts,
                self.substitution_attribute_element(node, pseudo_kind),
                self.html_element_namespace,
            )
        });
        let style_query = (calls_functions || value_reads_conditions(written.data()))
            .then(|| self.style_query_inputs(computed::ComputedStyleTarget::new(node, pseudo_kind.unwrap_or(u8::MAX))))
            .flatten();
        let inputs = SubstitutionInputs {
            document: &self.document_style_computation_inputs,
            media: &self.document_media,
            environment: SubstitutionEnvironment { own: 0, inherited: 0 },
            attributes: attributes.as_ref(),
            style_query: style_query.as_ref(),
            style_query_references: Some(style_query_references),
            functions: functions.as_ref(),
        };
        substitute_against_stores(
            store,
            inheritance_store,
            &inputs,
            property,
            root_custom_property_name,
            written,
        )
    }

    /// What a written value with `var()` references substitutes to for a property under an
    /// environment, parsed as the property's value: what the C++ cascade computes for the
    /// declaration, memoized by the written value. An `attr()` reads the element's attributes, an
    /// `inherit()` the environment the element inherits, an `if()` the document's media features
    /// and the element's lengths, and a custom function call the definitions its scope sees and
    /// whatever their declarations read, so any of these values is the element's alone and takes
    /// no memo. A substitution the resolver cannot make, or a substituted source no grammar
    /// accepts, is the guaranteed-invalid value, as in the C++ cascade: the declaration is invalid
    /// at computed-value time. Refused when the inputs lack what the value reads, or an
    /// environment is one the engine holds no store for.
    pub(super) fn substitute_written_value(
        environments: &mut custom_property_environments::CustomPropertyEnvironments,
        inputs: &SubstitutionInputs<'_>,
        property: u16,
        written: RetainedStyleValueData,
        counters: &Counters,
    ) -> Drive<RetainedStyleValueData> {
        let calls_functions = value_calls_custom_functions(written.data());
        let functions = inputs.functions.filter(|_| calls_functions);
        let reads_attributes =
            value_reads_attributes(written.data()) || functions.is_some_and(|functions| functions.reads_attributes);
        let attributes = inputs.attributes.filter(|_| reads_attributes);
        if (reads_attributes && attributes.is_none()) || (calls_functions && functions.is_none()) {
            counters.bump(Counter::EngineComputedRecordBailSubstitution);
            return Err(Unanswered::Refused);
        }
        // A function's declarations may substitute `inherit()` and `if()` as well.
        let reads_inherited_values = calls_functions || value_reads_inherited_values(written.data());
        let memoizes =
            !reads_attributes && !reads_inherited_values && !calls_functions && !value_reads_conditions(written.data());
        let environment = inputs.environment;
        if memoizes && let Some(value) = environments.substitution(&written, property, environment.own) {
            counters.bump(Counter::EngineComputedRecordSubstitutionMemoHits);
            return Ok(value);
        }
        let store_of = |identity| match identity {
            0 => Some(std::ptr::null()),
            identity => environments.store(identity),
        };
        let Some(store) = store_of(environment.own) else {
            counters.bump(Counter::EngineComputedRecordBailSubstitution);
            return Err(Unanswered::Refused);
        };
        let inheritance_store = if reads_inherited_values {
            let Some(store) = store_of(environment.inherited) else {
                counters.bump(Counter::EngineComputedRecordBailSubstitution);
                return Err(Unanswered::Refused);
            };
            store
        } else {
            std::ptr::null()
        };
        // The resolution reads the attributes and the custom functions only where the value does.
        let inputs = SubstitutionInputs {
            attributes,
            functions,
            ..*inputs
        };
        let value = substitute_against_stores(store, inheritance_store, &inputs, property, &[], &written);
        counters.bump(Counter::EngineComputedRecordSubstitutions);
        if memoizes {
            environments.remember_substitution(written, property, environment.own, value.clone_retained());
        }
        Ok(value)
    }
}

/// What a written value substitutes to for a property against a custom-property store and the
/// store its element inherits from, parsed as the property's value: what the C++ cascade computes
/// for the declaration. `root_custom_property_name` names the custom property the value is written
/// for, and is empty for a longhand. A substitution the resolver cannot make, or a substituted
/// source no grammar accepts, is the guaranteed-invalid value: the declaration is invalid at
/// computed-value time.
fn substitute_against_stores(
    store: *const c_void,
    inheritance_store: *const c_void,
    inputs: &SubstitutionInputs<'_>,
    property: u16,
    root_custom_property_name: &[u16],
    written: &RetainedStyleValueData,
) -> RetainedStyleValueData {
    let attributes = inputs.attributes;
    let functions = inputs.functions;
    let registry_ref = inputs.document.custom_property_registry();
    let mut random_function_index = 0_usize;
    let mut parse_context = registry_ref.parse_context(&mut random_function_index);
    parse_context.in_quirks_mode = inputs.document.in_quirks_mode;
    let media_environment = inputs.media.environment();
    let substitution_attributes = attributes.map_or(&[][..], |attributes| attributes.attributes.as_slice());
    let resolution_environment = unsafe {
        prepare_var_resolution_environment(
            substitution_attributes.as_ptr(),
            substitution_attributes.len(),
            functions.map_or(std::ptr::null(), |functions| functions.definitions.as_ptr()),
            functions.map_or(0, |functions| functions.definitions.len()),
            functions.map_or(0, |functions| functions.caller_scope),
            functions.map_or(std::ptr::null(), |functions| functions.visibilities.as_ptr()),
            functions.map_or(0, |functions| functions.visibilities.len()),
        )
    };
    let mut style_query_references = inputs.style_query_references.map(std::cell::RefCell::borrow_mut);
    // A function definition whose declarations do not tokenize resolves nothing.
    let resolution = match resolution_environment {
        // SAFETY: The store is live while a record names its environment, and the written
        // value is retained by the declaration that carries it.
        Some(mut resolution_environment) => unsafe {
            crate::css::custom_properties::resolve_vars(
                store,
                inheritance_store,
                std::ptr::from_ref(registry_ref).cast(),
                Some(&parse_context),
                Some(&media_environment),
                None,
                property,
                FfiUtf16View {
                    ascii: std::ptr::null(),
                    utf16: root_custom_property_name.as_ptr(),
                    length: root_custom_property_name.len(),
                },
                written.pointer().cast(),
                &mut resolution_environment,
                attributes.is_some_and(|attributes| attributes.names_are_ascii_case_insensitive),
                std::ptr::null_mut(),
                inputs.style_query,
                None,
                style_query_references.as_deref_mut(),
                None,
            )
        },
        None => NativeVarResolution::NotHandled,
    };
    // The substituted source parses as the C++ cascade parses it: without callbacks first,
    // then with the parse context's.
    let invalid = || RetainedStyleValueData::from_owned(StyleValueData::GuaranteedInvalid);
    match resolution {
        NativeVarResolution::Resolved {
            source,
            contains_attr_tainted_values,
        } => {
            let CallbackFreeParseOutcome { outcome, source } =
                parse_substituted_without_callbacks(&parse_context, property, source, contains_attr_tainted_values);
            let outcome = match outcome {
                ParseOutcome::NotHandled => {
                    parse_substituted_source(&parse_context, property, &source, contains_attr_tainted_values)
                }
                outcome => outcome,
            };
            match outcome {
                ParseOutcome::Parsed(value) => unsafe {
                    RetainedStyleValueData::from_retained_pointer(std::sync::Arc::into_raw(value))
                },
                ParseOutcome::Invalid | ParseOutcome::NotHandled => invalid(),
            }
        }
        NativeVarResolution::Invalid | NativeVarResolution::NotHandled => invalid(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_call_reaches_what_its_scope_and_the_scope_of_each_reached_definition_see() {
        let functions: Vec<_> = crate::css::rule::compile_functions_for_testing(
            "@function --outer() { result: --inner(); }
            @function --inner() { result: 2px; }
            @function --local() { result: 1px; }
            @function --local() { result: 3px; }
            @function --inner() { result: 4px; }",
        )
        .into_iter()
        .map(Arc::new)
        .collect();
        let [outer, inner, user_agent_local, local, shadow_inner] = [
            &functions[0],
            &functions[1],
            &functions[2],
            &functions[3],
            &functions[4],
        ];
        // The document's scope (1) defines --outer, --inner and --local, whose author rule wins
        // over the user agent's in a later layer. A shadow tree's scope (2) defines another --inner,
        // and a scope below it (3) defines nothing.
        let entry = |function: Option<&Arc<CompiledFunction>>, scope, parent_scope, tree_scope, layer, origin| {
            bridge::FfiCustomFunctionEntry {
                function: function.map_or(std::ptr::null(), |function| Arc::as_ptr(function).cast()),
                scope,
                parent_scope,
                tree_scope,
                layer,
                origin,
            }
        };
        let entries = [
            entry(Some(outer), 1, 0, 0, 0, 2),
            entry(Some(inner), 1, 0, 0, 0, 2),
            entry(Some(local), 1, 0, 0, 0, 2),
            entry(Some(user_agent_local), 1, 0, 0, 7, 0),
            entry(Some(shadow_inner), 2, 1, 5, 0, 2),
            entry(None, 3, 2, 9, 0, 0),
        ];
        let mut inputs = bridge::FfiDocumentStyleComputationInputs {
            custom_functions: bridge::FfiHostHandle::from_pointer(entries.as_ptr().cast()),
            custom_function_count: entries.len(),
            ..Default::default()
        };
        let mut snapshot = DocumentFunctionSnapshot::default();
        unsafe { snapshot.take_in(&mut inputs, &DocumentMediaSnapshot::default(), |_, layer| layer.0) };
        assert_eq!(inputs, bridge::FfiDocumentStyleComputationInputs::default());
        let reached = |tree_scope| {
            snapshot
                .visible_from(TreeScopeID(tree_scope))
                .definitions
                .iter()
                .map(|(function, scope)| (function.identity, *scope))
                .collect::<Vec<_>>()
        };

        assert_eq!(snapshot.scope_functions(TreeScopeID(0)).0, 1);
        assert_eq!(
            reached(0),
            [(outer.identity, 1), (inner.identity, 1), (local.identity, 1)]
        );

        // A call in the shadow tree reaches its own --inner, and the document's through the body
        // of --outer, as does one in the scope below it.
        let below_shadow = [
            (shadow_inner.identity, 2),
            (outer.identity, 1),
            (local.identity, 1),
            (inner.identity, 1),
        ];
        assert_eq!(snapshot.scope_functions(TreeScopeID(5)).0, 2);
        assert_eq!(reached(5), below_shadow);
        assert_eq!(snapshot.scope_functions(TreeScopeID(9)).0, 3);
        assert_eq!(reached(9), below_shadow);

        // A tree scope with no scope reaches none.
        assert_eq!(snapshot.scope_functions(TreeScopeID(11)).0, 0);
        assert!(reached(11).is_empty());
    }
}
