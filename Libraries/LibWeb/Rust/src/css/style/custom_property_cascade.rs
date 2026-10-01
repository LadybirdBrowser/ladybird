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
    CallbackFreeParseOutcome, FfiCascadeResolutionContext, FfiCustomPropertyDriveInput, FfiResolvedStyleValue,
    destroy_resolved_custom_properties, drive_custom_property_resolution, parse_substituted_source,
    parse_substituted_without_callbacks,
};
use crate::css::custom_properties::{CustomPropertyStore, NativeVarResolution, prepare_var_resolution_environment};
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::value_parser::ParseOutcome;
use crate::css::style_compute::keyword;
use crate::css::style_value::{RetainedStyleValueData, StyleValueData, release_style_value, retain_style_value};
use custom_property_environments::{CascadedCustomProperty, CustomPropertyName};

/// What the finalizer resolves a CSS-wide keyword against: the environment inherited.
struct EngineFinalizer {
    parent_store: *const c_void,
}

/// The tail of resolving one component of unregistered custom properties, as the C++ finalizer
/// does it for a name without a registration: `initial` is the guaranteed-invalid value, and
/// `inherit`, `unset`, `revert` and `revert-layer` are what the parent resolved the name to.
#[allow(clippy::arc_with_non_send_sync)]
unsafe extern "C" fn finalize_engine_custom_property_component(
    context: *mut c_void,
    names: *const usize,
    members: *const u32,
    member_count: usize,
    outputs: *mut FfiResolvedStyleValue,
) {
    let context = unsafe { &*context.cast::<EngineFinalizer>() };
    let parent = unsafe { context.parent_store.cast::<CustomPropertyStore>().as_ref() };
    let members = unsafe { std::slice::from_raw_parts(members, member_count) };
    for &member in members {
        let output = unsafe { &mut *outputs.add(member as usize) };
        let value = unsafe { &*output.data.cast::<StyleValueData>() };
        let StyleValueData::Keyword { keyword } = value else {
            continue;
        };
        let replacement: *const StyleValueData = match *keyword {
            keyword::INITIAL => Arc::into_raw(Arc::new(StyleValueData::GuaranteedInvalid)),
            keyword::INHERIT | keyword::UNSET | keyword::REVERT | keyword::REVERT_LAYER => {
                let name_raw = unsafe { *names.add(member as usize) };
                match parent.and_then(|parent| parent.get(name_raw)) {
                    Some(entry) => unsafe { retain_style_value(entry.value.pointer()) },
                    None => Arc::into_raw(Arc::new(StyleValueData::GuaranteedInvalid)),
                }
            }
            _ => continue,
        };
        unsafe { release_style_value(output.data.cast()) };
        output.data = replacement.cast();
    }
}

/// The resolution context the engine substitutes under: the stores alone, with no callback into
/// C++ - what the engine cannot resolve without one is left to C++ before this is built.
fn engine_resolution_context(
    parse_context: &crate::css::parser::value_parser::ParseContext,
    store: *const c_void,
    inheritance_store: *const c_void,
    registry: *const c_void,
    attributes: Option<&SubstitutionAttributes<'_>>,
) -> FfiCascadeResolutionContext {
    let substitution_attributes = attributes.map_or(&[][..], |attributes| attributes.attributes.as_slice());
    FfiCascadeResolutionContext {
        parse_context: std::ptr::from_ref(parse_context).cast(),
        media_environment: std::ptr::null(),
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
        custom_functions: std::ptr::null(),
        custom_function_count: 0,
        custom_function_scope_identity: 0,
        callback_context: std::ptr::null_mut(),
        install_custom_properties: None,
        resolve_custom_function: None,
        evaluate_style_query: None,
        note_substitution: None,
    }
}

/// Whether a token stream is a substitution the engine resolves itself: one whose only
/// substitution functions are `var()` references.
pub(super) fn value_is_engine_resolvable_substitution(value: &StyleValueData) -> bool {
    matches!(value, StyleValueData::Unresolved { .. }) && custom_property_value_is_engine_resolvable(value)
}

/// Whether a cascaded custom-property value is one the engine resolves: a plain value, or a
/// token stream whose only substitutions are `var()` references.
fn custom_property_value_is_engine_resolvable(value: &StyleValueData) -> bool {
    !value_reads_attributes(value) && substitutions_but_attr_are_engine_resolvable(value)
}

/// Whether a written value substitutes `attr()`: itself, or the shorthand a longhand pending its
/// substitution takes its part of.
pub(super) fn value_reads_attributes(value: &StyleValueData) -> bool {
    match value {
        StyleValueData::Unresolved { presence_attr, .. } => *presence_attr,
        StyleValueData::PendingSubstitution {
            original_shorthand_value,
        } => value_reads_attributes(original_shorthand_value.data()),
        _ => false,
    }
}

/// Whether the engine resolves every substitution a value holds but `attr()`, which it resolves
/// only where it has the element's attributes.
fn substitutions_but_attr_are_engine_resolvable(value: &StyleValueData) -> bool {
    !matches!(
        value,
        StyleValueData::Unresolved {
            presence_dashed_function: true,
            ..
        } | StyleValueData::Unresolved { presence_env: true, .. }
            | StyleValueData::Unresolved { presence_if: true, .. }
            | StyleValueData::Unresolved {
                presence_inherit: true,
                ..
            }
    )
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
    /// `None` when the facts lack the text of an attribute `attr()` may read: the host publishes
    /// every one, and an `attr()` that took its fallback for a missing one would be wrong.
    pub(super) fn of(
        facts: &'a index::ElementFactStore,
        node: StyleNodeID,
        html_namespace: StyleAtomID,
    ) -> Option<Self> {
        let view = |text: &[u16]| FfiUtf16View {
            ascii: std::ptr::null(),
            utf16: text.as_ptr(),
            length: text.len(),
        };
        let mut attributes = Vec::new();
        for (name, value) in facts.substitution_attributes(node) {
            let Some(value) = value else {
                debug_assert!(false, "the host publishes the text of every attribute attr() reads");
                return None;
            };
            attributes.push(crate::css::custom_properties::FfiSubstitutionAttribute {
                name: view(name),
                value: view(value),
            });
        }
        Some(Self {
            attributes,
            names_are_ascii_case_insensitive: !html_namespace.is_none() && facts.namespace_of(node) == html_namespace,
            facts: std::marker::PhantomData,
        })
    }
}

impl RetainedState {
    /// Hand each of a node's element-target matches, with the cascade inputs its priority is
    /// computed from, to `visit`, stopping when it breaks. `None` when the node has no answer to read.
    fn try_for_each_element_match(
        &self,
        node: StyleNodeID,
        mut visit: impl FnMut(RuleID, TreeScopeID, Specificity, u32) -> ControlFlow<()>,
    ) -> Option<ControlFlow<()>> {
        if let Some((published, answer)) = Self::published_answer_lookup(
            &self.published_match_answers,
            self.batch_matching_traversal.as_deref(),
            node,
        ) && let Some(matches) = published.matches_for(answer)
        {
            for entry in matches.iter().filter(|entry| entry.pseudo_element.is_none()) {
                if visit(entry.rule, entry.tree_scope, entry.specificity, entry.scope_proximity).is_break() {
                    return Some(ControlFlow::Break(()));
                }
            }
            return Some(ControlFlow::Continue(()));
        }
        let Lookup::Known(answer) = self.retained_match_answer(node) else {
            return None;
        };
        for rule_match in answer.iter() {
            let entry = &self.programs.get(rule_match.program).entries()[rule_match.entry as usize];
            if entry.pseudo_element.is_some() {
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
    fn any_custom_property_is_declared(&self) -> bool {
        self.program.any_rule_declares_custom_properties() || self.facts.any_element_declares_custom_properties()
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
        let parent = self.tree.inheritance_parent(node).map_or(Some(0), |parent| {
            self.computed_group_sets.custom_property_environment_identity(parent)
        });
        own != parent || self.node_declares_custom_properties(node)
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
    /// complete as any. A pseudo-element's rules keep the strict reading, since a pseudo-element's
    /// environment is still C++'s to compute.
    pub(super) fn cascade_winners_are_complete_but_for_custom_properties(&self, node: StyleNodeID) -> bool {
        if !ElementDeclarationKind::ALL.iter().all(|&kind| {
            self.facts
                .element_declarations_are_complete_but_for_custom_properties(node, kind)
        }) {
            return false;
        }
        // A rule deciding from another tree scope orders by its context like any other; the
        // record path reads the winners the cascade holds for it, whichever scope it decided from.
        let rule_is_complete = |rule: RuleID, _tree_scope: TreeScopeID, pseudo: bool| {
            !self.program.rule_is_gated_by_container_query(rule)
                && if pseudo {
                    self.program.declarations_are_complete_for(rule)
                } else {
                    self.program.declarations_are_complete_but_for_custom_properties(rule)
                }
        };
        if let Some((published, answer)) = Self::published_answer_lookup(
            &self.published_match_answers,
            self.batch_matching_traversal.as_deref(),
            node,
        ) && let Some(matches) = published.matches_for(answer)
        {
            return matches
                .iter()
                .all(|entry| rule_is_complete(entry.rule, entry.tree_scope, entry.pseudo_element.is_some()));
        }
        let Lookup::Known(answer) = self.retained_match_answer(node) else {
            return false;
        };
        answer.iter().all(|rule_match| {
            let entry = &self.programs.get(rule_match.program).entries()[rule_match.entry as usize];
            rule_is_complete(rule_match.rule, rule_match.tree_scope, entry.pseudo_element.is_some())
        })
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
        struct Candidate<'a> {
            priority: CascadePriority,
            stratum: CascadeStratum,
            declared: CustomDeclaration,
            written: &'a RetainedStyleValueData,
        }

        let mut candidates = Vec::new();
        let result = self.try_for_each_element_match(node, |rule, tree_scope, specificity, scope_proximity| {
            let declared = self.program.custom_declarations_of(rule);
            if declared.is_empty() {
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
                                rule,
                                tree_scope,
                                specificity,
                                scope_proximity,
                                declared.important,
                            ),
                            self.cascade_stratum_of(rule, tree_scope, declared.important),
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
        })?;
        if result.is_break() {
            return None;
        }
        let declared = self.facts.element_custom_declarations(node);
        let written = self.facts.element_custom_written_values(node);
        if written.len() != declared.len() {
            return None;
        }
        let mut priority_and_stratum_by_importance = [None; 2];
        for (&declared, written) in declared.iter().zip(written) {
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
        if cascaded
            .iter()
            .any(|(_, written)| !custom_property_value_is_engine_resolvable(written.data()))
        {
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

    /// The name a cascaded custom declaration names, as its store entry keys it. A block's
    /// publication notes every custom property name it declares before the block is set, so a
    /// live declaration's name is always known. A replay notes names without their fly strings,
    /// and a name without one keys no store entry: `None` there, and the declaration declares
    /// nothing.
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

    /// The environment of a node the engine computes a record for: the one it inherits when its
    /// cascade declares no custom property, else what its declarations resolve to over that one.
    /// Refused when the environment is C++'s to compute: a registered name or a substitution the
    /// engine does not resolve.
    ///
    /// A registration decides how its name computes, against the registered syntax and from its
    /// own initial value, which this resolution does not do, so an element declaring a registered
    /// name is the host's. A name registered as not inheriting keeps every declaring element with
    /// the host: the environment this builds over the parent's would hand that name to a
    /// descendant the registration keeps it from.
    pub(super) fn engine_custom_property_environment(
        &mut self,
        node: StyleNodeID,
        parent_environment: u64,
        inputs: &bridge::FfiDocumentStyleComputationInputs,
        counters: &mut Counters,
    ) -> Drive<u64> {
        if !self.any_custom_property_is_declared() {
            self.custom_declarations_reading_attributes.remove(&node);
            return Ok(parent_environment);
        }
        // A node the engine drives has the match answer its winners came from, and a published
        // block carries a written value for each custom declaration, so the cascade answers. One
        // that does not is refused rather than resolved without the node's own declarations.
        let cascaded = self.cascaded_custom_declarations(node);
        debug_assert!(cascaded.is_some(), "a driven node's custom declarations cascade");
        let Some(cascaded) = cascaded else {
            counters.bump(Counter::EngineCustomPropertyEnvironmentBails);
            return Err(Unanswered::Refused);
        };
        // Whether the declarations read the node's attributes, whatever environment they resolve
        // to: the host notes it beside the node's record.
        let reads_attributes = cascaded.iter().any(|(_, value)| value_reads_attributes(value.data()));
        if reads_attributes {
            self.custom_declarations_reading_attributes.insert(node);
        } else {
            self.custom_declarations_reading_attributes.remove(&node);
        }
        if cascaded.is_empty() {
            return Ok(parent_environment);
        }
        let registry_ref = inputs.custom_property_registry();
        // Before the memo: what C++ resolved for a registered name may depend on the element.
        if registry_ref.has_registrations()
            && (registry_ref.has_non_inheriting_registrations()
                || cascaded.iter().any(|(declared, _)| {
                    self.declared_custom_property_name(declared.name)
                        .is_some_and(|name| registry_ref.name_is_registered(&name.text))
                }))
        {
            counters.bump(Counter::EngineCustomPropertyEnvironmentBails);
            return Err(Unanswered::Refused);
        }
        // An `attr()` among the declarations reads the element's attributes, so what they resolve
        // to is the element's alone and takes no memo.
        let key = Self::environment_inputs(
            parent_environment,
            inputs.custom_property_registration_generation,
            &cascaded,
        );
        if !reads_attributes && let Some(identity) = self.custom_property_environments.memoized(&key) {
            counters.bump(Counter::EngineCustomPropertyEnvironmentMemoHits);
            return Ok(identity);
        }
        let Some(parent_store) = self.inherited_environment_store(parent_environment) else {
            counters.bump(Counter::EngineCustomPropertyEnvironmentBails);
            return Err(Unanswered::Refused);
        };
        let parent = unsafe { parent_store.cast::<CustomPropertyStore>().as_ref() };
        let mut values = Vec::with_capacity(cascaded.len());
        for (declared, value) in &cascaded {
            let Some(name) = self.declared_custom_property_name(declared.name) else {
                continue;
            };
            if !substitutions_but_attr_are_engine_resolvable(value.data()) {
                counters.bump(Counter::EngineCustomPropertyEnvironmentBails);
                return Err(Unanswered::Refused);
            }
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
            if !reads_attributes {
                let written_values = cascaded.into_iter().map(|(_, written)| written).collect();
                self.custom_property_environments
                    .remember(key, parent_environment, written_values);
            }
            return Ok(parent_environment);
        }
        // The attributes an `attr()` among the declarations reads.
        let attributes = if reads_attributes {
            let attributes = SubstitutionAttributes::of(&self.facts, node, self.html_element_namespace);
            if attributes.is_none() {
                counters.bump(Counter::EngineCustomPropertyEnvironmentBails);
                return Err(Unanswered::Refused);
            }
            attributes
        } else {
            None
        };
        // SAFETY: The parent store is live for as long as a record names its environment, and the
        // values are the program's interned values, live for the call.
        let cascaded_store = unsafe { CustomPropertyStore::cascaded_child(parent_store, values) };
        let mut random_function_index = 0_usize;
        let parse_context = registry_ref.parse_context(&mut random_function_index);
        let resolution_context = engine_resolution_context(
            &parse_context,
            cascaded_store,
            parent_store,
            std::ptr::from_ref(registry_ref).cast(),
            attributes.as_ref(),
        );
        let mut finalizer = EngineFinalizer { parent_store };
        let drive = FfiCustomPropertyDriveInput {
            store: cascaded_store,
            resolved_parent_store: parent_store,
            reuse_resolved_parent_if_empty: !parent_store.is_null(),
            resolution_context: &raw const resolution_context,
            finalizer_context: std::ptr::from_mut(&mut finalizer).cast(),
            finalize_component: Some(finalize_engine_custom_property_component),
        };
        // SAFETY: Every pointer the drive reads is live for the call, and the finalizer replaces
        // each output with one transferred reference.
        let resolved = unsafe { drive_custom_property_resolution(&drive) };
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
        counters.bump(Counter::EngineCustomPropertyEnvironmentsResolved);
        let identity = if resolved.rust_store.is_null() {
            parent_environment
        } else {
            unsafe {
                self.custom_property_environments
                    .adopt_engine_environment(resolved.rust_store, parent_environment)
            }
        };
        if !reads_attributes {
            let written_values = cascaded.into_iter().map(|(_, written)| written).collect();
            self.custom_property_environments
                .remember(key, identity, written_values);
        }
        Ok(identity)
    }

    /// What a node's records read beyond their cascade, as `FfiNodeRecordReads` bits, for the row
    /// that installs them: an `attr()` in the element's winners, in its pseudo-elements' (which
    /// read the originating element's attributes), or in the custom properties it declares, as
    /// their resolution found.
    pub(super) fn node_record_reads(&self, node: StyleNodeID) -> u8 {
        let groups = self.current_winner_groups();
        let reads_attributes = matches!(
            groups.token_for(WinnerGroupKey::current(node, self.program.version())),
            Lookup::Known((_, state)) if self.state_reads_attributes(node, state)
        ) || groups
            .pseudo_states(node)
            .any(|(_, _, state, _)| self.state_reads_attributes(node, state))
            || self.custom_declarations_reading_attributes.contains(&node);
        if reads_attributes {
            bridge::FfiNodeRecordReads::Attributes as u8
        } else {
            0
        }
    }

    /// What a written value with `var()` references substitutes to for a property under an
    /// environment, parsed as the property's value: what the C++ cascade computes for the
    /// declaration, memoized by the written value. An `attr()` reads the element's `attributes`,
    /// so its value is the element's alone and takes no memo. Refused when the value holds a
    /// substitution the engine does not resolve, or the environment is one the engine holds no
    /// store for.
    pub(super) fn substitute_written_value(
        environments: &mut custom_property_environments::CustomPropertyEnvironments,
        inputs: &bridge::FfiDocumentStyleComputationInputs,
        environment: u64,
        property: u16,
        written: RetainedStyleValueData,
        attributes: Option<&SubstitutionAttributes<'_>>,
        counters: &mut Counters,
    ) -> Drive<RetainedStyleValueData> {
        let attributes = attributes.filter(|_| value_reads_attributes(written.data()));
        if (attributes.is_none() && !custom_property_value_is_engine_resolvable(written.data()))
            || !substitutions_but_attr_are_engine_resolvable(written.data())
        {
            counters.bump(Counter::EngineComputedRecordBailSubstitution);
            return Err(Unanswered::Refused);
        }
        if attributes.is_none()
            && let Some(value) = environments.substitution(&written, property, environment)
        {
            counters.bump(Counter::EngineComputedRecordSubstitutionMemoHits);
            return Ok(value);
        }
        let store = match environment {
            0 => std::ptr::null(),
            identity => {
                let Some(store) = environments.store(identity) else {
                    counters.bump(Counter::EngineComputedRecordBailSubstitution);
                    return Err(Unanswered::Refused);
                };
                store
            }
        };
        let registry_ref = inputs.custom_property_registry();
        let mut random_function_index = 0_usize;
        let mut parse_context = registry_ref.parse_context(&mut random_function_index);
        parse_context.in_quirks_mode = inputs.in_quirks_mode;
        let substitution_attributes = attributes.map_or(&[][..], |attributes| attributes.attributes.as_slice());
        let Some(mut resolution_environment) = (unsafe {
            prepare_var_resolution_environment(
                substitution_attributes.as_ptr(),
                substitution_attributes.len(),
                std::ptr::null(),
                0,
                0,
            )
        }) else {
            counters.bump(Counter::EngineComputedRecordBailSubstitution);
            return Err(Unanswered::Refused);
        };
        // SAFETY: The store is live while a record names its environment, and the written value
        // is retained by the declaration that carries it.
        let resolution = unsafe {
            crate::css::custom_properties::resolve_vars(
                store,
                std::ptr::null(),
                std::ptr::from_ref(registry_ref).cast(),
                Some(&parse_context),
                None,
                None,
                property,
                FfiUtf16View {
                    ascii: std::ptr::null(),
                    utf16: std::ptr::null(),
                    length: 0,
                },
                written.pointer().cast(),
                &mut resolution_environment,
                attributes.is_some_and(|attributes| attributes.names_are_ascii_case_insensitive),
                None,
                std::ptr::null_mut(),
                None,
                None,
            )
        };
        // The substituted source parses as the C++ cascade parses it: without callbacks first,
        // then with the parse context's. A grammar the Rust parser does not handle parses in C++;
        // the value is C++'s to compute.
        let value = match resolution {
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
                    ParseOutcome::Invalid => RetainedStyleValueData::from_owned(StyleValueData::GuaranteedInvalid),
                    ParseOutcome::NotHandled => {
                        counters.bump(Counter::EngineComputedRecordBailSubstitution);
                        return Err(Unanswered::Refused);
                    }
                }
            }
            NativeVarResolution::Invalid => RetainedStyleValueData::from_owned(StyleValueData::GuaranteedInvalid),
            NativeVarResolution::NotHandled => {
                counters.bump(Counter::EngineComputedRecordBailSubstitution);
                return Err(Unanswered::Refused);
            }
        };
        counters.bump(Counter::EngineComputedRecordSubstitutions);
        if attributes.is_none() {
            environments.remember_substitution(written, property, environment, value.clone_retained());
        }
        Ok(value)
    }
}
