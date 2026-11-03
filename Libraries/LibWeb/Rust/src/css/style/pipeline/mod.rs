/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The style engine; see
//! `Documentation/Style/StyleEngine.md`.
//!
#[cfg(test)]
pub(crate) mod allocation_count;
#[cfg(test)]
mod allocation_tests;
mod bloom;
mod cascade;
mod cold_pass;
pub(in crate::css::style) mod compute;
pub(super) mod counters;
pub(in crate::css::style) mod custom_properties;
mod custom_property_moves;
mod declared_only;
mod demand;
#[cfg(test)]
mod differential_tests;
pub(in crate::css::style) mod drive;
mod extent_map;
#[cfg(test)]
mod fork_tests;
pub(crate) mod highlight_observers;
pub(crate) mod install;
mod install_scratch;
mod invalidation;
mod live;
pub(crate) mod main_effects;
mod matched_declarations;
mod matched_rules;
mod matching;
mod pending_inputs;
pub(super) use pending_inputs::HoverSealMemo;
pub(crate) mod per_thread;
pub(in crate::css::style) mod projection;
mod records;
mod rule_changes;
pub(crate) mod rule_store;
mod rules;
mod self_check;
mod shared_pages;
mod shared_prefix;
mod sharing;
#[cfg(test)]
mod test_support;
mod traversal;
pub(in crate::css::style) mod winner_store;
mod write_once;

use super::*;
use bloom::{attribute_hash, class_hash, id_hash, tag_hash};
pub(super) use cascade::DeclaredOnly;
pub(super) use live::Live;
#[cfg(test)]
pub(super) use records::PseudoElementStyles;
pub(super) use records::PseudoElementTarget;
pub(crate) use records::Record;
pub(crate) use records::{CustomDeclarations, RareSetFacts, RareStyleFacts};
pub(super) use records::{RecordFacts, Sample};

pub(super) use cold_pass::{Report, ReportField};
pub(super) use rules::ProcessRuleIndexes;
#[cfg(test)]
pub(super) use rules::entries_of;
pub(super) use self_check::SelfCheckSettings;
use smallvec::SmallVec;

/// The pseudo-elements a pass styles beside their element, in the order the host installs them.
pub(super) const PSEUDO_ELEMENT_KINDS: [u8; 9] = {
    use super::publication::pseudo_kind::{
        AFTER, BACKDROP, BEFORE, FIRST_LETTER, MARKER, SEARCH_TEXT, SEARCH_TEXT_CURRENT, SELECTION, SPELLING_ERROR,
    };
    [
        BEFORE,
        AFTER,
        FIRST_LETTER,
        SELECTION,
        SEARCH_TEXT,
        SEARCH_TEXT_CURRENT,
        SPELLING_ERROR,
        BACKDROP,
        MARKER,
    ]
};

/// The highlight pseudo-elements, which inherit from the same pseudo-element of the nearest
/// ancestor that has one.
pub(super) const HIGHLIGHT_KINDS: [u8; 4] = {
    use super::publication::pseudo_kind::{SEARCH_TEXT, SEARCH_TEXT_CURRENT, SELECTION, SPELLING_ERROR};
    [SELECTION, SEARCH_TEXT, SEARCH_TEXT_CURRENT, SPELLING_ERROR]
};

/// The keys an element contributes to its descendants' ancestor filter.
pub(super) fn ancestor_keys_of(facts: projection::ProjectionRow<'_>) -> SmallVec<[u32; 8]> {
    let mut keys = SmallVec::new();
    let id = facts.id();
    if !id.is_none() {
        keys.push(id_hash(id));
    }
    keys.extend(facts.classes().iter().map(|&class| class_hash(class)));
    for tag in [facts.tag(), facts.folded_tag()] {
        if !tag.is_none() {
            keys.push(tag_hash(tag));
        }
    }
    keys.extend(
        facts
            .attributes()
            .iter()
            .map(|attribute| attribute_hash(attribute.name)),
    );
    keys
}

/// What a computation waits on before it can answer.
#[derive(Default)]
pub(super) struct Waits {
    pub(super) fonts: Vec<font_resolution::FontRequest>,
    pub(super) random_bases: Vec<super::random_bases::RandomBaseRequest>,
}

/// How many rounds a computation may wait on fonts and random base values: a declined element hides
/// what its descendants wait on, so each round can find more.
const MAX_RESOLVE_ROUNDS: usize = 32;

/// How many more rounds a computation may wait for each element it inherits through, each of which it may compute with
/// a few fonts of its own and the random base values it draws in one round.
const MAX_RESOLVE_ROUNDS_PER_ANCESTOR: usize = 4;

impl StyleEngine {
    /// Runs `body` with the fonts of the document found by the content of their requests: a round
    /// computes the values its font requests name anew, so the fonts the round before asked for are
    /// found by the content of those values.
    fn matching_fonts_by_content<T>(&mut self, body: impl FnOnce(&mut Self) -> T) -> T {
        let set = |engine: &mut Self, matches_by_content| {
            let Some(cache) = &mut engine.retained.font_resolution else {
                return false;
            };
            let before = cache.matches_by_content();
            cache.set_matches_by_content(matches_by_content);
            before
        };
        let before = set(self, true);
        let answer = body(self);
        set(self, before);
        answer
    }

    /// The answer of `attempt`, run again until it no longer waits on anything, which this resolves
    /// for `node` between its runs, or none where it keeps waiting.
    pub(super) fn until_resolved<T>(
        &mut self,
        node: StyleNodeID,
        mut attempt: impl FnMut(&mut Self) -> Result<T, Waits>,
    ) -> Option<T> {
        self.matching_fonts_by_content(|engine| {
            let mut rounds = 0;
            let mut max_rounds = MAX_RESOLVE_ROUNDS;
            while rounds < max_rounds {
                match attempt(engine) {
                    Ok(answer) => return Some(answer),
                    Err(waits) => {
                        for request in waits.fonts {
                            engine.refill_font_request(node, request);
                        }
                        for request in waits.random_bases {
                            engine.retained.draw_random_base_value(request);
                        }
                    }
                }
                rounds += 1;
                if rounds == MAX_RESOLVE_ROUNDS {
                    let projection = &engine.retained.projection;
                    let ancestors = std::iter::successors(projection.inheritance_parent(node), |&parent| {
                        projection.inheritance_parent(parent)
                    });
                    max_rounds += MAX_RESOLVE_ROUNDS_PER_ANCESTOR * ancestors.count();
                }
            }
            None
        })
    }

    /// The computed initial values of the document under `root`.
    fn computed_initial(&mut self, root: StyleNodeID) -> Option<traversal::InitialStyle> {
        // The initial values compute against the document's initial font, which no element may
        // have asked the resolver for yet.
        self.until_resolved(root, |engine| {
            match engine.retained.initial_style(root) {
                Ok(mut computed) => {
                    // SAFETY: The initial style takes over the references the drive made.
                    let mut payloads = unsafe {
                        crate::css::computed_values::StyleGroups::adopt(
                            computed.payloads,
                            crate::css::computed_value_types::StyleBits {
                                inherited: crate::css::computed_value_types::InheritedSetValues {
                                    bits: crate::css::computed_value_types::InheritedBits::INITIAL,
                                    effective_zoom_bits: computed.value_facts.effective_zoom().to_bits(),
                                },
                                reset: crate::css::computed_value_types::ResetBits::INITIAL,
                            },
                        )
                    };
                    // The documents of the process share the initial groups but the font group.
                    let shared = computed.shared.take().unwrap_or_else(|| {
                        let value_groups = computed
                            .value_groups
                            .take()
                            .expect("a drive made the groups of its values");
                        let shared = engine.retained.process_style.share_initial_groups(
                            super::process_style::InitialGroupsInputs::of(
                                &engine.retained.document_style_computation_inputs,
                            ),
                            &payloads,
                            &value_groups,
                            super::process_style::InitialGroupsValues {
                                flags: computed.flags.clone(),
                                dependency_flags: computed.dependency_flags,
                                color: computed.color,
                                value_facts: computed.value_facts.clone(),
                            },
                        );
                        for index in crate::css::computed_values::StyleGroupIndex::all() {
                            if index.usize() != crate::css::table_group_builder::group_index::FONT {
                                payloads.share_group(shared.groups(), index);
                            }
                        }
                        shared
                    });
                    Ok(Some(traversal::InitialStyle::new(computed, payloads, shared)))
                }
                // NB: An engine no host published its fonts to yet, such as a test's bare engine, has no resolver
                //     to ask, and its initial style waits on fonts it cannot resolve yet.
                Err(Some(_)) if engine.host.font_resolver.is_none() => Ok(None),
                Err(Some(request)) => Err(Waits {
                    fonts: vec![*request],
                    random_bases: Vec::new(),
                }),
                Err(None) => Ok(None),
            }
        })
        .flatten()
    }

    /// Has style passes run on the threads of `pool`, which the process shares.
    pub(crate) fn share_style_layout_pool(&mut self, pool: std::sync::Arc<crate::style_layout_pool::StyleLayoutPool>) {
        self.retained.style_layout_pool = Some(pool);
    }

    /// The pool a pass on `threads` threads runs on besides the calling thread, or on the default
    /// number for the machine for none.
    fn style_layout_pool(&mut self, threads: u32) -> Option<std::sync::Arc<rayon::ThreadPool>> {
        // NB: An engine no render state holds, such as a test's, starts threads of its own.
        self.retained
            .style_layout_pool
            .get_or_insert_with(|| std::sync::Arc::new(crate::style_layout_pool::StyleLayoutPool::of_engine()))
            .for_threads(threads)
    }
}

impl RetainedState {
    /// What the audit of what passes reuse asks for: what the engine read as it started, or off
    /// without it.
    fn cache_audit_mode(&self) -> self_check::Mode {
        self.live_style
            .as_ref()
            .map_or(self_check::Mode::Off, |live| live.cache_audit)
    }
}
