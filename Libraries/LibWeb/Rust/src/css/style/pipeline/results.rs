/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The cascade result cache: an element whose cascade picked the same declarations as one computed
//! shortly before it, under a parent whose inherited style is the same, takes that element's record
//! instead of computing its own.
//!
//! Where the style sharing cache finds siblings and cousins before matching, this finds elements
//! anywhere that end up with the same winners, such as the levels of a chain of identically styled
//! elements. What a computation reads beyond its winners is in the key: the parent's inherited groups
//! by identity, its color, zoom and dependency flags, the box the element would be a child of, the
//! element's custom-property store by identity, the facts its computation adjusts for and the
//! counter-style registry of its style scope. Winners from an element's own declarations match
//! where those declarations declare the same values.

use super::super::animations::DeclarationScope;
use super::super::computed::ENGINE_INHERITED_GROUP_COUNT;
use super::super::*;
use super::cascade::Winner;
use super::records::Record;
use crate::css::display::FfiDisplay;
use smallvec::SmallVec;
use std::collections::VecDeque;
use std::hash::{Hash, Hasher};
use std::sync::Arc;

/// How many computed elements each thread keeps to reuse.
const CAPACITY: usize = 32;

/// How many hashes of recently computed elements each thread remembers. An element whose winners
/// include its own declarations is kept to reuse only once its hash recurs, so the many elements
/// whose inline style is their own cost a hash rather than an entry.
const SEEN_CAPACITY: usize = 64;

/// What a computation reads besides its winners and the element's own declarations.
#[derive(Clone, Copy, PartialEq)]
pub(super) struct ResultContext {
    pub(super) parent_inherited_groups: [usize; ENGINE_INHERITED_GROUP_COUNT],
    pub(super) parent_color: u32,
    /// The inherited longhands the parent holds as bits, and its effective zoom.
    pub(super) parent_inherited_values: crate::css::computed_value_types::InheritedSetValues,
    pub(super) parent_dependency_flags: u8,
    /// The display of the parent, which is the box the element would be a child of.
    pub(super) parent_display: FfiDisplay,
    pub(super) custom_properties: usize,
    pub(super) adjustment_facts: u32,
    pub(super) language: StyleAtomID,
    pub(super) directionality: StyleAtomID,
    /// The identity of the counter-style registry of the element's style scope, which its
    /// `list-style-type` or `content` may resolve counter styles against.
    pub(super) counter_style_environment: u64,
    /// The hash of the context with the winners and the element's own declarations.
    pub(super) hash: u64,
}

impl ResultContext {
    /// Complete the context of `node` with the hash of everything its result depends on.
    pub(super) fn hashed(mut self, state: &RetainedState, node: StyleNodeID, winners: &[Winner]) -> Self {
        self.hash = self.hash_with(state, node, winners);
        self
    }

    fn hash_with(&self, state: &RetainedState, node: StyleNodeID, winners: &[Winner]) -> u64 {
        let mut hasher = crate::fast_hash::fast_hasher();
        winners.hash(&mut hasher);
        for kind in element_declaration_kinds(winners) {
            for (declared, written) in element_declarations(state, node, kind) {
                declared.hash(&mut hasher);
                written.pointer().hash(&mut hasher);
            }
        }
        self.parent_inherited_groups.hash(&mut hasher);
        self.parent_color.hash(&mut hasher);
        self.custom_properties.hash(&mut hasher);
        self.adjustment_facts.hash(&mut hasher);
        hasher.finish()
    }
}

struct Entry {
    node: StyleNodeID,
    winners: SmallVec<[Winner; 16]>,
    context: ResultContext,
    record: Arc<Record>,
    /// The custom-property store the context names by identity, which the entry keeps alive so
    /// that no other store takes its address. The record may hold another, computed from it.
    _custom_properties: Option<Arc<crate::css::custom_properties::CustomPropertyStore>>,
}

pub(super) struct ResultCache {
    /// Most recently computed last.
    entries: VecDeque<Entry>,
    seen: [u64; SEEN_CAPACITY],
    next_seen: usize,
    /// How often an element with declarations of its own was found and missed since the counts
    /// last started over. While such elements rarely repeat, a thread computes them without
    /// looking for them.
    hits: u32,
    misses: u32,
    lookups: u32,
}

impl Default for ResultCache {
    fn default() -> Self {
        Self {
            entries: VecDeque::new(),
            seen: [0; SEEN_CAPACITY],
            next_seen: 0,
            hits: 0,
            misses: 0,
            lookups: 0,
        }
    }
}

/// How many lookups the hit counts cover before they start over.
const COUNT_PERIOD: u32 = 1024;

/// The declarations of `kind` of `node`, each with the value it was written with. Equal values
/// elements declare are one canonical value, so two elements declaring the same declare the same
/// values.
fn element_declarations(
    state: &RetainedState,
    node: StyleNodeID,
    kind: ElementDeclarationKind,
) -> impl Iterator<Item = (&DeclarationEntry, &RetainedStyleValueData)> {
    state
        .element_declarations
        .declarations(node, kind)
        .unwrap_or_default()
        .iter()
}

/// The kinds of an element's own declarations its winners came from.
fn element_declaration_kinds(winners: &[Winner]) -> impl Iterator<Item = ElementDeclarationKind> + '_ {
    let mut seen = 0_u8;
    winners.iter().filter_map(move |winner| match winner.source {
        WinnerSource::Element(kind) if seen & (1 << kind.index()) == 0 => {
            seen |= 1 << kind.index();
            Some(kind)
        }
        _ => None,
    })
}

impl ResultCache {
    /// Whether looking for the next element with declarations of its own is worth its cost.
    pub(super) fn worth_looking(&mut self) -> bool {
        self.lookups += 1;
        if self.lookups == COUNT_PERIOD {
            self.lookups = 0;
            self.hits = 0;
            self.misses = 0;
        }
        // A hit saves a computation, which costs some twenty lookups that miss.
        self.misses < 64 || self.hits * 20 >= self.misses
    }

    pub(super) fn find(
        &mut self,
        state: &RetainedState,
        node: StyleNodeID,
        winners: &[Winner],
        animation_name_scope: DeclarationScope,
        context: &ResultContext,
    ) -> Option<Arc<Record>> {
        // NB: The winners of a sheet several shadow roots adopt are the same in each, but not the
        //     scope their `animation-name` looks for `@keyframes` in.
        let position = self.entries.iter().rposition(|entry| {
            entry.context == *context
                && entry.winners[..] == *winners
                && entry.record.rare().animation_name_scope == animation_name_scope
                && element_declaration_kinds(winners).all(|kind| {
                    let identities = |node| {
                        element_declarations(state, node, kind)
                            .map(|(declared, written)| (*declared, written.pointer()))
                    };
                    identities(node).eq(identities(entry.node))
                })
        });
        let declares_own = element_declaration_kinds(winners).next().is_some();
        let Some(position) = position else {
            self.misses += u32::from(declares_own);
            return None;
        };
        self.hits += u32::from(declares_own);
        let entry = self.entries.remove(position).expect("the position is in the cache");
        let record = entry.record.clone();
        self.entries.push_back(entry);
        Some(record)
    }

    pub(super) fn insert(
        &mut self,
        node: StyleNodeID,
        winners: &[Winner],
        context: ResultContext,
        custom_properties: Option<Arc<crate::css::custom_properties::CustomPropertyStore>>,
        record: Arc<Record>,
    ) {
        debug_assert_eq!(
            context.custom_properties,
            custom_properties
                .as_ref()
                .map_or(0, |store| Arc::as_ptr(store) as usize)
        );
        if element_declaration_kinds(winners).next().is_some() && !self.seen.contains(&context.hash) {
            self.seen[self.next_seen] = context.hash;
            self.next_seen = (self.next_seen + 1) % SEEN_CAPACITY;
            return;
        }
        if self.entries.len() == CAPACITY {
            self.entries.pop_front();
        }
        self.entries.push_back(Entry {
            node,
            winners: winners.into(),
            context,
            record,
            _custom_properties: custom_properties,
        });
    }
}
