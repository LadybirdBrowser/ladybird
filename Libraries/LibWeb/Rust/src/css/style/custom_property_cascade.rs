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
use std::sync::Arc;

use super::*;
use crate::css::cascaded_properties::{
    CascadeResolutionContext, parse_substituted_value, resolve_declared_custom_properties,
};
use crate::css::custom_properties::{
    CustomFunctionRegistry, CustomFunctionVisibility, CustomPropertyFinalization, CustomPropertyStore,
    NativeVarResolution, PreparedCustomFunction, SubstitutionAttribute, SubstitutionFunctionVisibility,
    VarResolutionInputs, prepare_var_resolution_environment,
};
use crate::css::rule::CompiledFunction;
use crate::css::style_value::{RetainedStyleValueData, StyleValueData, custom_property_references};
use custom_property_environments::{CustomPropertyName, retained_store};

static EMPTY_CUSTOM_FUNCTIONS: CustomFunctionRegistry<'static> = CustomFunctionRegistry::empty();

/// The document's media features as the style update a transaction belongs to saw them, which
/// `media()` conditions in `if()` read. Copied rather than borrowed: the host's snapshot ends with
/// the style update, and a record demanded after it still resolves against these.
#[derive(Clone, Default)]
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
                ak::slice_from_raw_parts(
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
    pub(super) fn environment(&self) -> crate::css::parser::query_parser::FfiMediaEnvironment<'_> {
        crate::css::parser::query_parser::FfiMediaEnvironment::new(&self.values, self.length.as_ref())
    }

    /// What lengths resolve against for a document without styled elements.
    pub(super) fn length(&self) -> Option<&crate::css::style_compute::FfiLengthResolutionContext> {
        self.length.as_ref()
    }
}

/// The `@function` definitions each scope sees, as the host published them with a transaction's
/// inputs: what a custom function call resolves against. A scope is a host `StyleScope`, named by
/// its identity, as the resolver names the scope of a call and of a definition.
#[derive(Clone, Default)]
pub(super) struct DocumentFunctionSnapshot {
    /// Each definition some scope sees, by its identity, with the scope that defines it.
    definitions: HashMap<u64, (Arc<CompiledFunction>, usize)>,
    /// Which definition each scope's calls name, a name at a time.
    visibilities: Vec<SubstitutionFunctionVisibility>,
    /// The scope of each tree scope that sees a definition.
    caller_scopes: HashMap<TreeScopeID, usize>,
    /// What a call from each scope reaches, worked out once for the transaction.
    scope_functions: HashMap<usize, ScopeFunctions>,
}

/// What a call from one scope reaches, as `visible_from` finds it, with the input blocks of each
/// definition's body the document's media selects: only the container-gated blocks among those
/// differ from one element to the next.
#[derive(Clone)]
pub(super) struct ScopeFunctions {
    /// Each reachable definition, with the scope that defines it and its selected blocks.
    definitions: Vec<(Arc<CompiledFunction>, usize, Box<[usize]>)>,
    /// Caller scope identities paired with indices into `definitions`.
    visibilities: Vec<CustomFunctionVisibility>,
}

/// What a call from one tree scope may reach: the definitions its scope sees, and those the
/// defining scope of each of them sees, which the calls in its body name.
struct VisibleFunctions<'a> {
    /// Each reachable definition, with the scope that defines it.
    definitions: Vec<(&'a CompiledFunction, usize)>,
    visibilities: Vec<SubstitutionFunctionVisibility>,
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
                ak::slice_from_raw_parts(
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
                    self.visibilities.push(SubstitutionFunctionVisibility {
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
        let media_environment = media_environment.borrow();
        for &caller_scope in self.caller_scopes.values() {
            if self.scope_functions.contains_key(&caller_scope) {
                continue;
            }
            let visible = self.visible_from_scope(caller_scope);
            let definitions: Vec<_> = visible
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
            let visibilities = visible
                .visibilities
                .iter()
                .filter_map(|visibility| {
                    let index = definitions
                        .iter()
                        .position(|(function, _, _)| function.identity == visibility.function_identity);
                    debug_assert!(index.is_some(), "a visible function is published with its definition");
                    Some(CustomFunctionVisibility {
                        caller_scope_identity: visibility.caller_scope_identity,
                        definition_index: index?,
                    })
                })
                .collect();
            let scope_functions = ScopeFunctions {
                definitions,
                visibilities,
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
/// name.
pub(super) struct PreparedCustomFunctions<'a> {
    registry: CustomFunctionRegistry<'a>,
    /// Whether a parameter default or selected declaration substitutes `attr()`, which reads the
    /// node's attributes.
    pub(super) reads_attributes: bool,
    /// What the container conditions read of the node's containers, for the host to record with
    /// its record; `None` when no condition was evaluated.
    pub(super) container_effects: Option<super::container_queries::ContainerVerdict>,
}

/// The resolution context the engine substitutes under: the stores alone, with no callback into
/// C++ - what the engine cannot resolve without one is left to C++ before this is built.
#[expect(
    clippy::too_many_arguments,
    reason = "the context borrows each resolution input on its own"
)]
pub(super) fn engine_resolution_context<'a>(
    parse_context: &'a crate::css::parser::value_parser::ParseContext,
    store: Option<&'a CustomPropertyStore>,
    inheritance_store: Option<&'a CustomPropertyStore>,
    registry: &'a crate::css::custom_properties::CustomPropertyRegistry,
    attributes: Option<&'a SubstitutionAttributes<'a>>,
    media_environment: &'a crate::css::parser::query_parser::FfiMediaEnvironment<'a>,
    style_query: Option<&'a crate::css::cascaded_properties::StyleQueryInputs>,
    functions: Option<&'a PreparedCustomFunctions<'a>>,
) -> CascadeResolutionContext<'a> {
    CascadeResolutionContext {
        inputs: VarResolutionInputs {
            store,
            inheritance_store,
            registry: Some(registry),
            parse_context: Some(parse_context),
            media_environment: Some(media_environment),
            attribute_names_are_ascii_case_insensitive: attributes
                .is_some_and(|attributes| attributes.names_are_ascii_case_insensitive),
            style_query_inputs: style_query,
        },
        attributes: attributes.map_or(&[][..], |attributes| attributes.attributes.as_slice()),
        custom_functions: functions.map_or(&EMPTY_CUSTOM_FUNCTIONS, |functions| &functions.registry),
        style_query_dependencies: None,
    }
}

/// The token stream a written value substitutes: itself, or the shorthand a longhand pending its
/// substitution takes its part of.
fn substituted_tokens(value: &StyleValueData) -> &StyleValueData {
    match value {
        StyleValueData::PendingSubstitution {
            original_shorthand_value,
            ..
        } => substituted_tokens(original_shorthand_value.data()),
        value => value,
    }
}

/// Custom property names, sorted and without duplicates.
pub(crate) type CustomPropertyNames = Arc<[Arc<[u16]>]>;

/// The custom properties the longhands of a style substitute.
#[derive(Clone, Default, PartialEq)]
pub(crate) enum LonghandCustomPropertyReads {
    /// They substitute none.
    #[default]
    None,
    /// They substitute these, by name.
    Named(CustomPropertyNames),
    /// They substitute something that may read any of them.
    Any,
}

impl LonghandCustomPropertyReads {
    /// The reads of `names`, where `any` is whether a substitution may read others.
    pub(super) fn new(names: Vec<Arc<[u16]>>, any: bool) -> Self {
        match (any, names.is_empty()) {
            (true, _) => Self::Any,
            (false, true) => Self::None,
            (false, false) => Self::Named(sorted_custom_property_names(names)),
        }
    }

    /// Whether the longhands substitute one of the custom properties `moved` holds.
    pub(crate) fn read_any_of(&self, moved: impl Fn(&[u16]) -> bool) -> bool {
        match self {
            Self::None => false,
            Self::Named(names) => names.iter().any(|name| moved(name)),
            Self::Any => true,
        }
    }
}

/// Add the custom properties a written value reads by name to `names`, answering whether it reads
/// only those: an `if()` condition, a custom function or an attribute's value can read any.
pub(super) fn append_named_custom_property_reads(value: &StyleValueData, names: &mut Vec<Arc<[u16]>>) -> bool {
    let Some(reads) = named_custom_property_reads(value) else {
        return false;
    };
    names.extend(reads.map(Arc::from));
    true
}

/// The custom properties a written value reads by name, or `None` when it may read others: an
/// `if()` condition, a custom function or an attribute's value can read any, and `inherit()` reads
/// what the parent holds, which holds more than it hands down.
fn named_custom_property_reads(value: &StyleValueData) -> Option<impl Iterator<Item = &[u16]>> {
    let value = substituted_tokens(value);
    if let StyleValueData::Unresolved(unresolved) = value
        && (unresolved.presence().attr
            || unresolved.presence().dashed_function
            || unresolved.presence().if_
            || unresolved.presence().inherit)
    {
        return None;
    }
    let Some((references, all_references_visible)) = custom_property_references(value) else {
        return Some(None.into_iter().flatten());
    };
    all_references_visible.then(|| Some(references).into_iter().flatten())
}

/// Whether a written value holds a function that substitutes anything.
fn substitutes(value: &StyleValueData) -> bool {
    matches!(substituted_tokens(value), StyleValueData::Unresolved(unresolved) if unresolved.presence().has_any())
}

/// Which nodes of the graph `edges` describes lie on a cycle of it, one with an edge to itself among them. Those are the
/// nodes of a strongly connected component of more than one node, which Tarjan's algorithm finds in time linear in the
/// nodes and edges. It walks with a stack of its own, as a chain may be as long as a page likes.
fn nodes_on_a_cycle(edges: &[Vec<usize>]) -> Vec<bool> {
    // The order each node was first seen in, from 1, and the earliest such order on the walk's stack that it reaches.
    let mut order = vec![0usize; edges.len()];
    let mut lowest = vec![0usize; edges.len()];
    let mut on_stack = vec![false; edges.len()];
    let mut on_a_cycle = vec![false; edges.len()];
    let mut stack = Vec::new();
    let mut walk: Vec<(usize, usize)> = Vec::new();
    let mut nodes_seen = 0;
    for root in 0..edges.len() {
        if order[root] != 0 {
            continue;
        }
        walk.push((root, 0));
        while let Some((node, next_edge)) = walk.last_mut() {
            let node = *node;
            if order[node] == 0 {
                nodes_seen += 1;
                order[node] = nodes_seen;
                lowest[node] = nodes_seen;
                stack.push(node);
                on_stack[node] = true;
            }
            if let Some(&target) = edges[node].get(*next_edge) {
                *next_edge += 1;
                if target == node {
                    on_a_cycle[node] = true;
                } else if order[target] == 0 {
                    walk.push((target, 0));
                } else if on_stack[target] {
                    lowest[node] = lowest[node].min(order[target]);
                }
                continue;
            }
            walk.pop();
            if let Some((parent, _)) = walk.last() {
                lowest[*parent] = lowest[*parent].min(lowest[node]);
            }
            if lowest[node] == order[node] {
                let start = stack
                    .iter()
                    .rposition(|member| *member == node)
                    .expect("a node is on the stack");
                let is_cycle = stack.len() - start > 1;
                for member in stack.drain(start..) {
                    on_stack[member] = false;
                    on_a_cycle[member] |= is_cycle;
                }
            }
        }
    }
    on_a_cycle
}

/// Whether a written value may read one of the custom properties `moved` holds.
pub(super) fn value_reads_any_custom_property_of(value: &StyleValueData, moved: impl Fn(&[u16]) -> bool) -> bool {
    named_custom_property_reads(value).is_none_or(|mut names| names.any(moved))
}

pub(super) fn sorted_custom_property_names(mut names: Vec<Arc<[u16]>>) -> CustomPropertyNames {
    names.sort_unstable();
    names.dedup();
    names.into()
}

/// Whether a written value substitutes `attr()`, which reads the element's attributes.
pub(super) fn value_reads_attributes(value: &StyleValueData) -> bool {
    unresolved_reading_attributes(value).is_some()
}

/// The tokens a written value substitutes, where they substitute `attr()`.
pub(super) fn unresolved_reading_attributes(value: &StyleValueData) -> Option<&crate::css::style_value::UnresolvedBox> {
    match substituted_tokens(value) {
        StyleValueData::Unresolved(unresolved) if unresolved.presence().attr => Some(unresolved),
        _ => None,
    }
}

/// Whether a written value substitutes `inherit()`, which reads the custom-property environment
/// of the parent rather than the element's own.
pub(super) fn value_reads_inherited_values(value: &StyleValueData) -> bool {
    matches!(
        substituted_tokens(value),
        StyleValueData::Unresolved(unresolved) if unresolved.presence().inherit
    )
}

/// Whether a written value may substitute `if()`, directly or through an attribute's value.
/// Its conditions read the document's media features and the element's lengths beside its
/// custom-property environment.
pub(super) fn value_reads_conditions(value: &StyleValueData) -> bool {
    matches!(
        substituted_tokens(value),
        StyleValueData::Unresolved(unresolved) if unresolved.presence().if_ || unresolved.presence().attr
    )
}

/// Whether a written value calls a custom function, whose result depends on the `@function`
/// definitions its scope sees and on the conditions inside them.
pub(super) fn value_calls_custom_functions(value: &StyleValueData) -> bool {
    matches!(
        substituted_tokens(value),
        StyleValueData::Unresolved(unresolved) if unresolved.presence().dashed_function
    )
}

/// What a written value's substitutions read beyond the element's custom-property environment,
/// as `FfiNodeRecordReads` bits.
pub(super) fn substitution_reads(value: &StyleValueData) -> u8 {
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

/// What a node's winners substitute against beside their written values: the document's inputs
/// and media features, and what is read of the element itself,
/// where a winner reads it - its attributes for `attr()`, the lengths and colors a `style()` query
/// in `if()` resolves against, and the custom functions its calls reach.
pub(super) struct SubstitutionInputs<'a> {
    pub document: &'a bridge::FfiDocumentStyleComputationInputs,
    pub media: &'a DocumentMediaSnapshot,
    pub attributes: Option<&'a SubstitutionAttributes<'a>>,
    pub style_query: Option<&'a crate::css::cascaded_properties::StyleQueryInputs>,
    /// Where whether a `style()` query reads the sibling position is noted.
    pub style_query_references:
        Option<&'a std::cell::RefCell<Option<Box<crate::css::custom_properties::StyleQueryDependencies>>>>,
    pub functions: Option<&'a PreparedCustomFunctions<'a>>,
}

/// What an `attr()` reads of an element, as the resolution takes it: each of the element's
/// attributes in no namespace, by local name, with its value's text, and whether names match ASCII
/// case-insensitively, as an HTML element's do. It points into the facts it borrows.
pub(super) struct SubstitutionAttributes<'a> {
    attributes: Vec<SubstitutionAttribute<'a>>,
    names_are_ascii_case_insensitive: bool,
}

impl<'a> SubstitutionAttributes<'a> {
    pub(super) fn of(
        projection: &'a pipeline::projection::Projection,
        node: StyleNodeID,
        html_namespace: StyleAtomID,
    ) -> Self {
        let facts = projection.facts(node);
        Self {
            attributes: facts
                .map_or(&[][..], |facts| facts.attributes())
                .iter()
                .filter_map(|attribute| {
                    Some(SubstitutionAttribute {
                        name: projection.attribute_substitution_name(attribute.name)?,
                        value: projection.attribute_value_text(attribute.value)?,
                    })
                })
                .collect(),
            names_are_ascii_case_insensitive: !html_namespace.is_none()
                && facts.is_some_and(|facts| facts.namespace() == html_namespace),
        }
    }
}

/// Whether a value substitutes something of the element itself beyond its custom properties:
/// its attributes, conditions on its style and the document's media, or custom functions.
pub(super) fn value_reads_substitution_subject(value: &StyleValueData) -> bool {
    value_calls_custom_functions(value) || value_reads_attributes(value) || value_reads_conditions(value)
}

impl RetainedState {
    /// Whether anything in the document declares a custom property. Nothing declaring one means
    /// every environment is the inherited one, and no cascade need look.
    pub(super) fn any_custom_property_is_declared(&self) -> bool {
        self.rules.any_rule_declares_custom_properties() || self.element_declarations.any_declares_custom_properties()
    }

    /// Whether the element's animations sample custom properties, which its own values read.
    pub(super) fn element_samples_custom_properties(&self, node: StyleNodeID) -> bool {
        self.element_custom_property_data
            .get(&node)
            .is_some_and(|held| held.sampled_over.is_some())
    }

    /// What a `style()` query of an `if()` resolves against for a node or one of its
    /// pseudo-elements, as the host takes it: the style the target holds, else the one its parent
    /// holds, which for a pseudo-element is its originating element's, else the document's initial
    /// font, preferred color scheme and initial color.
    pub(super) fn style_query_inputs(
        &self,
        target: computed::ComputedStyleTarget,
    ) -> Option<crate::css::cascaded_properties::StyleQueryInputs> {
        let node = target.node();
        let parent_record = || {
            if target.pseudo_kind() == u8::MAX {
                self.computed_group_sets
                    .assigned_style_record(self.projection.inheritance_parent(node)?)
            } else {
                self.computed_group_sets.assigned_style_record(node)
            }
        };
        let held = self
            .computed_group_sets
            .assigned_final_style_record(target)
            .or_else(parent_record)
            .and_then(|record| self.computed_group_sets.style_record_view(record.raw()));
        self.style_query_inputs_of(held.as_ref().map(|view| (view.payloads, view.dependency_flags)), node)
    }

    /// What `style_query_inputs` says, against the style `view`, or no style at all.
    pub(super) fn style_query_inputs_of(
        &self,
        style: Option<(&crate::css::computed_values::StyleGroups, u8)>,
        node: StyleNodeID,
    ) -> Option<crate::css::cascaded_properties::StyleQueryInputs> {
        let tree_counting = self
            .element_sibling_position(node)
            .map(|(count, index)| (u64::from(count), u64::from(index)));
        if let Some((payloads, dependency_flags)) = style
            && let Some(length) = self.record_length_resolution_context(payloads, dependency_flags)
        {
            let values = crate::css::computed_value_views::ComputedValuesView::of(payloads);
            return Some(crate::css::cascaded_properties::StyleQueryInputs {
                length,
                tree_counting,
                color_scheme: values.inherited_ui().color_scheme,
                current_color: values.inherited_text().color,
            });
        }
        Some(crate::css::cascaded_properties::StyleQueryInputs {
            length: *self.document_media.length()?,
            tree_counting,
            color_scheme: self.document_style_computation_inputs.preferred_color_scheme,
            // Opaque black, the initial color.
            current_color: 0xff00_0000,
        })
    }

    /// Whether the custom property a declaration names is registered.
    pub(super) fn custom_property_name_is_registered(&self, name: StyleAtomID) -> bool {
        let registry = self.document_style_computation_inputs.custom_property_registry();
        registry.has_registrations()
            && self
                .declared_custom_property_name(name)
                .is_some_and(|name| name.with_fly_string(|name| registry.is_registered(name)))
    }

    /// What the custom function calls in the values of a node, or of one of its pseudo-elements,
    /// resolve against: the functions its tree scope's calls reach, each with the blocks of
    /// declarations whose `@media` conditions hold for the transaction and whose container
    /// conditions hold for the node. `None` when the engine cannot decide a container condition.
    pub(super) fn prepare_custom_functions<'a>(
        &self,
        snapshot: &'a DocumentFunctionSnapshot,
        node: StyleNodeID,
        pseudo: Option<u8>,
    ) -> Option<PreparedCustomFunctions<'a>> {
        self.prepare_custom_functions_with(snapshot, node, |containers| {
            self.container_conditions_verdict(containers, node, pseudo.is_some())
        })
    }

    /// What `prepare_custom_functions` says, with `verdict` deciding the container conditions.
    pub(super) fn prepare_custom_functions_with<'a>(
        &self,
        snapshot: &'a DocumentFunctionSnapshot,
        node: StyleNodeID,
        verdict: impl Fn(
            &[std::sync::Arc<crate::css::container_conditions::ContainerConditionsData>],
        ) -> Option<super::container_queries::ContainerVerdict>,
    ) -> Option<PreparedCustomFunctions<'a>> {
        let (caller_scope, visible) = snapshot.scope_functions(self.projection.tree_scope(node));
        let visible_definitions = visible.map_or(&[][..], |visible| visible.definitions.as_slice());
        let mut container_effects: Option<super::container_queries::ContainerVerdict> = None;
        let mut reads_attributes = false;
        let mut definitions = Vec::with_capacity(visible_definitions.len());
        for (function, scope, selected_inputs) in visible_definitions {
            reads_attributes |= function.parameter_defaults_read_attributes;
            let mut selected = Vec::new();
            for input in selected_inputs.iter().map(|&index| &function.inputs[index]) {
                if !input.containers.is_empty() {
                    let verdict = verdict(&input.containers)?;
                    let matches = verdict.matches;
                    container_effects.get_or_insert_default().add_reads(verdict);
                    if !matches {
                        continue;
                    }
                }
                reads_attributes |= input.reads_attributes;
                selected.extend(&input.declarations);
            }
            definitions.push(PreparedCustomFunction {
                identity: function.identity,
                scope_identity: *scope,
                signature: &function.signature,
                parameter_defaults: &function.parameter_defaults,
                declarations: selected,
            });
        }
        Some(PreparedCustomFunctions {
            registry: CustomFunctionRegistry {
                definitions,
                visible: visible.map_or(&[][..], |visible| visible.visibilities.as_slice()),
                caller_scope_identity: caller_scope,
            },
            reads_attributes,
            container_effects,
        })
    }

    /// The name a cascaded custom declaration names, as its store entry keys it. A block's
    /// publication notes every custom property name it declares before the block is set, so a
    /// live declaration's name is always known.
    pub(super) fn declared_custom_property_name(&self, name: StyleAtomID) -> Option<&CustomPropertyName> {
        let noted = self.custom_property_environments.name(name);
        debug_assert!(
            noted.is_some(),
            "a custom declaration's name is noted at its publication"
        );
        noted.filter(|noted| noted.raw() != 0)
    }

    /// The store of the environment the element's declarations resolved to last, where `resolved` holds the same values
    /// over the same stores, as [`Self::environment_resolving_to_the_same_values`] keeps it: a store computed again
    /// gives way to it, so that the element's record holds the store its environment is named by.
    pub(super) fn store_resolving_to_the_same_values(
        &self,
        node: StyleNodeID,
        pseudo: Option<u8>,
        resolved: &Arc<CustomPropertyStore>,
    ) -> Option<Arc<CustomPropertyStore>> {
        let registry = self.document_style_computation_inputs.custom_property_registry();
        let previous = self.environment_resolving_to_the_same_values(node, pseudo, resolved, registry)?;
        self.custom_property_environments.store(previous).cloned()
    }

    /// The environment the element's declarations resolved to last, beneath any animation overlay, where they resolve to
    /// its values again in `resolved`: an environment resolved anew, as one holding a registered value is, would move
    /// what the element's descendants inherit without moving any value they see. What an environment hands down was
    /// decided under the registrations it was resolved under, so only one handing down every value, then and now, is
    /// kept.
    pub(super) fn environment_resolving_to_the_same_values(
        &self,
        node: StyleNodeID,
        pseudo: Option<u8>,
        resolved: &Arc<CustomPropertyStore>,
        registry: &crate::css::custom_properties::CustomPropertyRegistry,
    ) -> Option<u64> {
        if pseudo.is_some() {
            return None;
        }
        // NB: An element holds an environment of its own only where one lies over its record's.
        let previous = match self.element_custom_property_data.get(&node) {
            Some(held) => held.sampled_over.unwrap_or(held.identity),
            None => self.computed_group_sets.custom_property_environment_identity(node)?,
        };
        if !self.custom_property_environments.is_engine_made(previous)
            || self.custom_property_environments.inheritable(previous) != previous
        {
            return None;
        }
        let previous_store = self.custom_property_environments.store(previous)?;
        (resolved.hands_down_every_value(registry)
            && CustomPropertyStore::hold_the_same_values(resolved, previous_store))
        .then_some(previous)
    }

    /// The custom properties in a dependency cycle through what an element's animations declare, as
    /// `--p: var(--q)` in a keyframe beside `--q: var(--p)` on the element. An animated value wins
    /// the cascade over a declared one, so a name it animates reads as its keyframes do. What a value
    /// reads is what substituting it looks up, over `base`, the element's environment beneath its
    /// animations, with their values `animated`, and `inheritance`, the environment it inherits:
    /// which branch of an `if()` or fallback that takes depends on the values, and what decides that
    /// is itself read, so that a cycle through it is found whichever way it went.
    pub(crate) fn custom_properties_in_an_animated_cycle(
        &self,
        node: StyleNodeID,
        pseudo: Option<u8>,
        base: *const c_void,
        inheritance: *const c_void,
        animated: &[(
            crate::css::retained_fly_string::RetainedUtf16FlyString,
            RetainedStyleValueData,
        )],
    ) -> Vec<crate::css::retained_fly_string::RetainedUtf16FlyString> {
        type Name = crate::css::retained_fly_string::RetainedUtf16FlyString;
        let slot = pseudo.map_or(0, |kind| kind + 1);
        let mut written: Vec<(Name, &StyleValueData)> = Vec::new();
        for effect in self.element_animation_effects(node, slot) {
            for keyframe in effect.keyframes() {
                for declaration in effect.custom_declarations_of(keyframe) {
                    let crate::css::style::effect_descriptions::PublishedValue::Declared(value) = &declaration.value
                    else {
                        continue;
                    };
                    written.push((declaration.name.clone(), value.data()));
                }
            }
        }
        if !written.iter().any(|(_, value)| substitutes(value)) {
            return Vec::new();
        }
        let written_names: HashSet<usize> = written.iter().map(|(name, _)| name.raw()).collect();
        let declarations = self
            .live_style
            .as_ref()
            .and_then(|live| live.custom_declarations_of(node, pseudo));
        if let Some(declarations) = &declarations {
            for (declared, value) in declarations
                .iter()
                .filter(|(declared, _)| declared.operator() == CascadeOperator::Declared)
            {
                let Some(name) = self.declared_custom_property_name(declared.name) else {
                    continue;
                };
                let name = Name::from_utf16(name.to_utf16().as_ref());
                if !written_names.contains(&name.raw()) {
                    written.push((name, value.data()));
                }
            }
        }
        // NB: A container condition the engine cannot decide leaves what a function reads unknown, as a read of no name.
        let functions = written
            .iter()
            .any(|(_, value)| value_calls_custom_functions(value))
            .then(|| self.prepare_custom_functions(&self.document_functions, node, pseudo))
            .flatten();
        let reads_attributes = functions.as_ref().is_some_and(|functions| functions.reads_attributes)
            || written.iter().any(|(_, value)| value_reads_attributes(value));
        let attribute_element = self.substitution_attribute_element(node, pseudo);
        let attributes = reads_attributes
            .then(|| SubstitutionAttributes::of(&self.projection, attribute_element, self.html_element_namespace));
        let reads_conditions = functions.is_some() || written.iter().any(|(_, value)| value_reads_conditions(value));
        let style_query = reads_conditions
            .then(|| self.style_query_inputs(computed::ComputedStyleTarget::new(node, pseudo.unwrap_or(u8::MAX))))
            .flatten();
        // SAFETY: The base store is live for the sample, and the values are live for the call.
        let base = unsafe { retained_store(base) };
        let values = animated
            .iter()
            .map(|(name, value)| (name.raw(), false, value.pointer().cast()))
            .collect();
        // SAFETY: The values are live for the call.
        let store = unsafe { CustomPropertyStore::cascaded_child(base, values) };
        let registry = self.document_style_computation_inputs.custom_property_registry();
        let random_function_index = crate::css::parser::value_parser::RandomFunctionIndex::default();
        let mut parse_context = registry.parse_context(&random_function_index);
        parse_context.in_quirks_mode = self.document_style_computation_inputs.in_quirks_mode;
        let media_environment = self.document_media.environment();
        // SAFETY: The store the element inherits from is live for the sample.
        let inheritance_store = unsafe { inheritance.cast::<CustomPropertyStore>().as_ref() };
        let resolution_context = engine_resolution_context(
            &parse_context,
            Some(&store),
            inheritance_store,
            registry,
            attributes.as_ref(),
            &media_environment,
            style_query.as_ref(),
            functions.as_ref(),
        );
        let mut environment = crate::css::custom_properties::prepare_var_resolution_environment(
            resolution_context.attributes,
            resolution_context.custom_functions,
        );
        let mut reads: Vec<(&Name, Vec<usize>)> = Vec::new();
        let mut reads_of_name: HashMap<usize, usize> = HashMap::default();
        for (name, value) in &written {
            let index = *reads_of_name.entry(name.raw()).or_insert_with(|| {
                reads.push((name, Vec::new()));
                reads.len() - 1
            });
            let StyleValueData::Unresolved(unresolved) = substituted_tokens(value) else {
                continue;
            };
            if !unresolved.presence().has_any() {
                continue;
            }
            let fly_name = ak::Utf16FlyString::from_utf16(name.to_utf16().as_ref());
            let names = crate::css::custom_properties::custom_properties_a_substitution_reads(
                &resolution_context.inputs,
                &fly_name,
                unresolved,
                &mut environment,
            );
            reads[index].1.extend(names.iter().map(|name| name.raw_identity()));
        }
        // NB: A name nothing writes reads nothing, so no cycle goes through it.
        let edges: Vec<Vec<usize>> = reads
            .iter()
            .map(|(_, read)| {
                read.iter()
                    .filter_map(|read| reads_of_name.get(read).copied())
                    .collect()
            })
            .collect();
        let on_a_cycle = nodes_on_a_cycle(&edges);
        reads
            .iter()
            .zip(on_a_cycle)
            .filter(|(_, on_a_cycle)| *on_a_cycle)
            .map(|((name, _), _)| (*name).clone())
            .collect()
    }

    /// What the custom properties an element declares resolve to once its animations sampled
    /// `animated` into its custom properties. An animated value wins the cascade over a declared
    /// one, so a declaration reading an animated name, or reading a declaration that does, resolves
    /// against the animated values. Answers each such declaration whose value moved from the one
    /// it holds in `base`, the element's environment beneath its animations. `inheritance` is the
    /// environment the element inherits, and `finalization` computes registered values against the
    /// element's lengths. Returns an error when the declarations or their functions cannot be prepared.
    pub(crate) fn resolve_custom_properties_reading_animated_values(
        &mut self,
        node: StyleNodeID,
        pseudo: Option<u8>,
        base: *const c_void,
        inheritance: *const c_void,
        animated: &[(
            crate::css::retained_fly_string::RetainedUtf16FlyString,
            RetainedStyleValueData,
        )],
        finalization: CustomPropertyFinalization<'_>,
    ) -> Result<
        Vec<(
            crate::css::retained_fly_string::RetainedUtf16FlyString,
            RetainedStyleValueData,
        )>,
        (),
    > {
        if animated.is_empty() || !self.any_custom_property_is_declared() {
            return Ok(Vec::new());
        }
        // What the element's cascade decided is the record the engine holds for it.
        let Some(cascaded) = self
            .live_style
            .as_ref()
            .and_then(|live| live.custom_declarations_of(node, pseudo))
        else {
            return Err(());
        };
        let mut moved: HashSet<Arc<[u16]>> = animated
            .iter()
            .map(|(name, _)| Arc::from(name.to_utf16().as_ref()))
            .collect();
        let mut candidates: Vec<_> = cascaded
            .iter()
            .filter(|(declared, _)| declared.operator() == CascadeOperator::Declared)
            .filter_map(|(declared, value)| {
                let name = self.declared_custom_property_name(declared.name)?;
                Some((
                    name,
                    declared,
                    value,
                    named_custom_property_reads(value.data()).map(Iterator::collect),
                ))
            })
            .filter(|(name, _, _, _)| !moved.contains(name.to_utf16().as_ref()))
            .collect();
        // What a declaration reads moves where any name it reads moved, and a read the scan
        // cannot name may read any.
        let mut readers = Vec::new();
        loop {
            let reads_moved = |reads: &Option<Vec<&[u16]>>| {
                reads
                    .as_ref()
                    .is_none_or(|reads| reads.iter().any(|read| moved.contains(*read)))
            };
            let Some(index) = candidates.iter().position(|(_, _, _, reads)| reads_moved(reads)) else {
                break;
            };
            let (name, declared, value, _) = candidates.swap_remove(index);
            moved.insert(Arc::from(name.to_utf16().as_ref()));
            readers.push((name.clone(), declared, value));
        }
        if readers.is_empty() {
            return Ok(Vec::new());
        }

        let functions = if readers
            .iter()
            .any(|(_, _, value)| value_calls_custom_functions(value.data()))
        {
            // A container condition the engine cannot decide leaves the declarations as they resolved.
            let Some(functions) = self.prepare_custom_functions(&self.document_functions, node, pseudo) else {
                return Err(());
            };
            Some(functions)
        } else {
            None
        };
        let reads_attributes = functions.as_ref().is_some_and(|functions| functions.reads_attributes)
            || readers.iter().any(|(_, _, value)| value_reads_attributes(value.data()));
        let attribute_element = self.substitution_attribute_element(node, pseudo);
        let attributes = reads_attributes
            .then(|| SubstitutionAttributes::of(&self.projection, attribute_element, self.html_element_namespace));
        let reads_conditions =
            functions.is_some() || readers.iter().any(|(_, _, value)| value_reads_conditions(value.data()));
        let style_query = reads_conditions
            .then(|| self.style_query_inputs(computed::ComputedStyleTarget::new(node, pseudo.unwrap_or(u8::MAX))))
            .flatten();

        // The readers resolve as declared, over the element's environment, beside the animated values.
        let values = readers
            .iter()
            .map(|(name, declared, value)| (name.raw(), declared.important(), value.pointer().cast()))
            .chain(
                animated
                    .iter()
                    .map(|(name, value)| (name.raw(), false, value.pointer().cast())),
            )
            .collect();
        // SAFETY: The base store is live for the sample, and the values are live for the call.
        let base = unsafe { retained_store(base) };
        // SAFETY: The values are live for the call.
        let cascaded_store = unsafe { CustomPropertyStore::cascaded_child(base.clone(), values) };
        let registry = self.document_style_computation_inputs.custom_property_registry();
        let random_function_index = crate::css::parser::value_parser::RandomFunctionIndex::default();
        let mut parse_context = registry.parse_context(&random_function_index);
        parse_context.in_quirks_mode = self.document_style_computation_inputs.in_quirks_mode;
        let media_environment = self.document_media.environment();
        // SAFETY: The store the element inherits from is live for the sample.
        let inheritance_store = unsafe { inheritance.cast::<CustomPropertyStore>().as_ref() };
        let mut resolution_context = engine_resolution_context(
            &parse_context,
            Some(&cascaded_store),
            inheritance_store,
            registry,
            attributes.as_ref(),
            &media_environment,
            style_query.as_ref(),
            functions.as_ref(),
        );
        // A registered reader's random() takes the base value drawn for the element as its
        // declarations resolved, not one the keyframes share.
        let random_base_values = std::cell::RefCell::new(&mut self.random_base_values);
        let draw_random_base_value = |name: &[u16], element_shared: bool| {
            random_base_values.borrow_mut().ensure(Some(node), name, element_shared)
        };
        let finalization = CustomPropertyFinalization {
            random_base_values: &[],
            draw_random_base_value: Some(&draw_random_base_value),
            ..finalization
        };
        let (resolved, _) = resolve_declared_custom_properties(
            &cascaded_store,
            base.as_ref(),
            true,
            &mut resolution_context,
            finalization,
        );
        let Some(resolved) = resolved else {
            return Ok(Vec::new());
        };
        let readers = readers
            .iter()
            .filter_map(|(name, _, _)| {
                let name_raw = name.raw();
                if !resolved.declares(name_raw) {
                    return None;
                }
                let value = resolved.own_values.get(&name_raw)?.value.clone();
                // SAFETY: The name is live while the engine notes it.
                Some((
                    unsafe { crate::css::retained_fly_string::RetainedUtf16FlyString::from_borrowed_raw(name_raw) },
                    value,
                ))
            })
            .collect();
        Ok(readers)
    }

    /// Whether what `written` substitutes to on an element depends only on the custom-property stores the element
    /// holds and inherits from, and the registrations: whether it reads no attribute, condition or custom function.
    pub(crate) fn keyframe_substitution_reads_only_stores(written: &StyleValueData) -> bool {
        !value_reads_attributes(written) && !value_reads_conditions(written) && !value_calls_custom_functions(written)
    }

    /// What a keyframe's written value substitutes to on the element being sampled, against the
    /// custom-property store the element holds and the one it inherits from, which the host hands
    /// over.
    /// `root_custom_property_name` names the custom property the value is written for, and is
    /// empty for a longhand. Like a cascaded declaration, a value that does not substitute or parse
    /// is guaranteed-invalid, and so is one calling a custom function whose container condition the
    /// engine cannot decide. Whether a `style()` query reads the sibling position is noted in
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
            .then(|| self.prepare_custom_functions(&self.document_functions, node, pseudo_kind))
            .flatten();
        let reads_attributes = value_reads_attributes(written.data())
            || functions.as_ref().is_some_and(|functions| functions.reads_attributes);
        let attributes = reads_attributes.then(|| {
            SubstitutionAttributes::of(
                &self.projection,
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
            attributes: attributes.as_ref(),
            style_query: style_query.as_ref(),
            style_query_references: Some(style_query_references),
            functions: functions.as_ref(),
        };
        // SAFETY: The stores are null or live for the sample.
        let (store, inheritance_store) = unsafe {
            (
                store.cast::<CustomPropertyStore>().as_ref(),
                inheritance_store.cast::<CustomPropertyStore>().as_ref(),
            )
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
}

/// What a written value substitutes to for a property against a custom-property store and the
/// store its element inherits from, parsed as the property's value: what the C++ cascade computes
/// for the declaration. `root_custom_property_name` names the custom property the value is written
/// for, and is empty for a longhand. A substitution the resolver cannot make, or a substituted
/// source no grammar accepts, is the guaranteed-invalid value: the declaration is invalid at
/// computed-value time.
pub(super) fn substitute_against_stores(
    store: Option<&CustomPropertyStore>,
    inheritance_store: Option<&CustomPropertyStore>,
    inputs: &SubstitutionInputs<'_>,
    property: u16,
    root_custom_property_name: &[u16],
    written: &RetainedStyleValueData,
) -> RetainedStyleValueData {
    let attributes = inputs.attributes;
    let functions = inputs.functions;
    let registry_ref = inputs.document.custom_property_registry();
    let random_function_index = crate::css::parser::value_parser::RandomFunctionIndex::default();
    let mut parse_context = registry_ref.parse_context(&random_function_index);
    parse_context.in_quirks_mode = inputs.document.in_quirks_mode;
    let media_environment = inputs.media.environment();
    let mut resolution_environment = prepare_var_resolution_environment(
        attributes.map_or(&[][..], |attributes| attributes.attributes.as_slice()),
        functions.map_or(&EMPTY_CUSTOM_FUNCTIONS, |functions| &functions.registry),
    );
    let mut style_query_references = inputs.style_query_references.map(std::cell::RefCell::borrow_mut);
    let resolution_inputs = VarResolutionInputs {
        store,
        inheritance_store,
        registry: Some(registry_ref),
        parse_context: Some(&parse_context),
        media_environment: Some(&media_environment),
        attribute_names_are_ascii_case_insensitive: attributes
            .is_some_and(|attributes| attributes.names_are_ascii_case_insensitive),
        style_query_inputs: inputs.style_query,
    };
    let resolution = crate::css::custom_properties::resolve_vars(
        &resolution_inputs,
        property,
        root_custom_property_name,
        written.data(),
        &mut resolution_environment,
        style_query_references.as_deref_mut(),
    );
    let invalid = || RetainedStyleValueData::from_owned(StyleValueData::GuaranteedInvalid);
    match resolution {
        NativeVarResolution::Resolved {
            components,
            contains_attr_tainted_values,
        } => parse_substituted_value(&parse_context, property, &components, contains_attr_tainted_values),
        NativeVarResolution::Invalid | NativeVarResolution::NotHandled => invalid(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nodes_on_a_cycle_are_those_of_a_cycle_and_not_those_between_cycles() {
        let on_a_cycle =
            |edges: &[&[usize]]| nodes_on_a_cycle(&edges.iter().map(|edges| edges.to_vec()).collect::<Vec<_>>());
        assert_eq!(on_a_cycle(&[]), Vec::<bool>::new());
        assert_eq!(on_a_cycle(&[&[]]), [false]);
        assert_eq!(on_a_cycle(&[&[0]]), [true]);
        // A chain, and a chain that reads a name twice.
        assert_eq!(on_a_cycle(&[&[], &[0], &[1, 1]]), [false, false, false]);
        assert_eq!(on_a_cycle(&[&[1], &[0]]), [true, true]);
        // A node reads into a cycle, and a cycle reads a node.
        assert_eq!(on_a_cycle(&[&[1], &[2], &[1]]), [false, true, true]);
        assert_eq!(on_a_cycle(&[&[1], &[0, 2], &[]]), [true, true, false]);
        // A node between two cycles lies on neither.
        assert_eq!(
            on_a_cycle(&[&[1], &[0, 2], &[3], &[4], &[3]]),
            [true, true, false, true, true]
        );
        // Two paths to one node, whose second finds it finished.
        assert_eq!(on_a_cycle(&[&[1, 2], &[3], &[3], &[]]), [false, false, false, false]);
        // A cycle reached back through a node the walk has finished with.
        assert_eq!(on_a_cycle(&[&[1, 2], &[], &[0, 1]]), [true, false, true]);
    }

    #[test]
    fn nodes_on_a_cycle_agrees_with_a_walk_from_each_node() {
        let mut state = 0x2545_f491_4f6c_dd1d_u64;
        let mut next = move |bound: usize| {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            state as usize % bound
        };
        for _ in 0..2000 {
            let nodes = 1 + next(12);
            let edge_count = next(2 * nodes);
            let mut edges = vec![Vec::new(); nodes];
            for _ in 0..edge_count {
                edges[next(nodes)].push(next(nodes));
            }
            let reaches_itself = |node: usize| {
                let mut seen = vec![false; nodes];
                let mut waiting = edges[node].clone();
                while let Some(target) = waiting.pop() {
                    if target == node {
                        return true;
                    }
                    if !std::mem::replace(&mut seen[target], true) {
                        waiting.extend(&edges[target]);
                    }
                }
                false
            };
            let expected: Vec<bool> = (0..nodes).map(reaches_itself).collect();
            assert_eq!(nodes_on_a_cycle(&edges), expected, "{edges:?}");
        }
    }

    #[test]
    fn nodes_on_a_cycle_walks_a_long_chain_and_ring_without_recursing() {
        let length = 1_000_000;
        let chain: Vec<Vec<usize>> = (0..length)
            .map(|node| if node == 0 { vec![] } else { vec![node - 1] })
            .collect();
        assert!(nodes_on_a_cycle(&chain).iter().all(|on_a_cycle| !on_a_cycle));
        let ring: Vec<Vec<usize>> = (0..length).map(|node| vec![(node + 1) % length]).collect();
        assert!(nodes_on_a_cycle(&ring).iter().all(|on_a_cycle| *on_a_cycle));
    }

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
