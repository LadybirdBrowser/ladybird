/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The style sharing cache: an element that agrees with one styled shortly before it on everything
//! its style could depend on takes that element's record instead of matching, cascading and
//! computing its own.
//!
//! Two elements agree when their parents are the same element or shared a style through this cache, which by
//! induction makes their ancestor chains indistinguishable to selectors, and they themselves have
//! the same tag, namespace, classes and states, no id a selector tests for, and no declarations of
//! their own. Rule entries that read anything beyond that, such as attributes or siblings,
//! revalidate: they are evaluated for the element and must match exactly as they matched the
//! candidate.

use super::super::index::StateSet;
use super::super::*;
use super::matching::ElementMatch;
use super::projection::ElementFacts;
use super::records::Record;
use super::rules::RuleIndex;
use smallvec::SmallVec;
use std::sync::Arc;

/// How many recently styled elements each thread keeps to share with.
const CAPACITY: usize = 16;

/// What two elements must agree on, besides their classes, custom states and parts, to share.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) struct SharingKey {
    /// The sharing identity of the parent.
    parent: StyleNodeID,
    /// The sharing identity of the inheritance parent, or the inheritance parent itself where the
    /// host composes its animations over its record: elements sharing a record do not share a
    /// composition.
    inheritance_parent: StyleNodeID,
    inheritance_parent_composes: bool,
    tag: StyleAtomID,
    namespace: StyleAtomID,
    states: StateSet,
    directionality: StyleAtomID,
    language: StyleAtomID,
    heading_level: u8,
    part_exposure: StyleAtomID,
    adjustment_facts: u32,
    tree_scope: TreeScopeID,
    assigned_slot: Option<StyleNodeID>,
    /// The records of the highlight pseudo-elements of the inheritance parent, which those of the
    /// element inherit from.
    highlight_parents: [usize; 4],
    /// The highlights the host shows in the element, whose pseudo-elements a pass styles.
    highlight_observers: u8,
}

/// The matched entries that revalidate, with the scope proximity of each.
pub(super) type Revalidation = SmallVec<[(u32, u32); 4]>;

pub(super) fn revalidation_of(index: &RuleIndex, matches: &[ElementMatch]) -> Revalidation {
    matches
        .iter()
        .filter(|matched| index.entries.view(matched.entry as usize).revalidates())
        .map(|matched| (matched.entry, matched.scope_proximity))
        .collect()
}

/// The key `node` shares under, or none where it cannot share: it has an id a selector tests for,
/// declarations of its own, a shadow tree or parts, a composition of its animations over its
/// record, or a parent this pass did not style.
pub(super) fn key_of(
    state: &RetainedState,
    index: &RuleIndex,
    records: &super::records::Records<'_>,
    node: StyleNodeID,
    facts: &ElementFacts,
) -> Option<SharingKey> {
    let id = facts.id();
    if state.is_quirks_table(node)
        || (id != StyleAtomID::NONE && index.tests_id(id))
        || !state.element_declarations.custom_declarations(node).is_empty()
        || [
            ElementDeclarationKind::InlineStyle,
            ElementDeclarationKind::PresentationalHint,
            ElementDeclarationKind::SvgPresentationAttribute,
        ]
        .into_iter()
        .any(|kind| !state.element_declarations.declared_properties(node, kind).is_empty())
        || state.projection.shadow_root_of(node).is_some()
        || !state.projection.part_hosts_of(node).is_empty()
        // https://drafts.csswg.org/css-pseudo-4/#treelike
        // An element sharing a style takes the records of its pseudo-elements too, which inherit
        // the composition of the element's own animations, as no other element's style tells.
        || records.composition(node).is_some()
        // NB: A pseudo-element's record substitutes the custom properties its own animations sampled, which are not the
        //     same for two elements with the same rules: their animations can stand at different times.
        || state.pseudo_element_samples_custom_properties(node)
        || !state.highlight_observers.custom_highlights_of(node).is_empty()
    {
        return None;
    }
    let identity = |node: Option<StyleNodeID>| records.sharing_identity(node?);
    let inheritance_parent = state.projection.inheritance_parent(node);
    let inheritance_parent_composes = inheritance_parent.is_some_and(|parent| records.composition(parent).is_some());
    Some(SharingKey {
        parent: identity(state.projection.parent(node))?,
        inheritance_parent: match inheritance_parent_composes {
            true => inheritance_parent?,
            false => identity(inheritance_parent)?,
        },
        inheritance_parent_composes,
        tag: facts.tag(),
        namespace: facts.namespace(),
        states: facts.states(),
        directionality: facts.directionality(),
        language: facts.language(),
        heading_level: facts.heading_level(),
        part_exposure: facts.part_exposure(),
        adjustment_facts: state.projection.adjustment_facts(node),
        tree_scope: state.projection.tree_scope(node),
        assigned_slot: state.projection.assigned_slot_of(node),
        highlight_parents: {
            let parent_records = inheritance_parent.map_or(&[][..], |parent| records.pseudo_element_records(parent));
            super::HIGHLIGHT_KINDS.map(|kind| {
                parent_records
                    .iter()
                    .find(|(held, _)| *held == super::PseudoElementTarget::of_kind(kind))
                    .map_or(0, |(_, record)| Arc::as_ptr(record) as usize)
            })
        },
        highlight_observers: state.highlight_observers.kinds_of(node),
    })
}

struct Candidate {
    key: SharingKey,
    record: Arc<Record>,
    /// The element whose record the candidate holds, whose classes, custom states and parts the
    /// key leaves out.
    identity: StyleNodeID,
    revalidation: Revalidation,
}

#[derive(Default)]
pub(super) struct SharingCache {
    /// Most recently styled last.
    candidates: std::collections::VecDeque<Candidate>,
}

impl SharingCache {
    /// The record of a candidate an element with `facts` agrees with, asking `revalidate` for the
    /// element's matches among the entries that revalidate once some candidate agrees on
    /// everything else.
    pub(super) fn find(
        &mut self,
        projection: &super::projection::Projection,
        key: &SharingKey,
        facts: &ElementFacts,
        mut revalidate: impl FnMut() -> Option<Revalidation>,
    ) -> Option<(Arc<Record>, StyleNodeID)> {
        let mut revalidation = None;
        let position = self.candidates.iter().rposition(|candidate| {
            if candidate.key != *key {
                return false;
            }
            let Some(held) = projection.facts(candidate.identity) else {
                return false;
            };
            if held.classes() != facts.classes()
                || held.custom_states() != facts.custom_states()
                || held.parts() != facts.parts()
            {
                return false;
            }
            revalidation
                .get_or_insert_with(&mut revalidate)
                .as_ref()
                .is_some_and(|revalidation| *revalidation == candidate.revalidation)
        })?;
        let candidate = self.candidates.remove(position)?;
        let found = (candidate.record.clone(), candidate.identity);
        self.candidates.push_back(candidate);
        Some(found)
    }

    pub(super) fn insert(
        &mut self,
        key: SharingKey,
        record: Arc<Record>,
        identity: StyleNodeID,
        revalidation: Revalidation,
    ) {
        if self.candidates.len() == CAPACITY {
            self.candidates.pop_front();
        }
        self.candidates.push_back(Candidate {
            key,
            record,
            identity,
            revalidation,
        });
    }
}
