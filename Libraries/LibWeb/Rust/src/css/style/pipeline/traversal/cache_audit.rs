/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The audit of what a pass reuses. With `LIBWEB_STYLE_AUDIT_CACHES` set, every element a pass
//! with its caches on gives a record, whether through style sharing, the matched declarations cache, a move of
//! its custom properties or a computation reading the group, absolutized value, custom property
//! store and substitution caches of its thread, is styled again from nothing: on a scratch of its
//! own, with every cache off, over the records the pass holds for the elements before it. The
//! element's record and its pseudo-elements' records, the ones from the matched declarations cache
//! included, must be the ones that recomputation gives, and the rules it matched the ones that
//! recomputation matches. `report` prints what differs at the end of the pass, and any other value
//! but `0` aborts there.

use super::super::super::*;
use super::super::records::{PseudoElementRecords, PseudoElementTarget, Record};
use super::super::self_check::{Mode, describe, pseudo_element_name, pseudo_element_target_name, record_differences};
use super::{PassContext, Scratch, is_document_element};
use std::sync::Arc;

/// What `LIBWEB_STYLE_AUDIT_CACHES` asks for, which the engine reads once as it starts.
pub(in super::super) fn mode_from_environment() -> Mode {
    match std::env::var("LIBWEB_STYLE_AUDIT_CACHES").as_deref() {
        Err(_) | Ok("" | "0") => Mode::Off,
        Ok("report") => Mode::Report,
        Ok(_) => Mode::Abort,
    }
}

/// What gave an element the record the audit checks.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Reuse {
    SharedStyle,
    MatchedDeclarations,
    MovedCustomProperties,
    /// A computation, reading the caches its thread kept of the computations before it.
    ComputationCaches,
    /// The matched declarations cache, for a pseudo-element.
    PseudoElementRecord,
}

impl Reuse {
    const COUNT: usize = 5;

    fn name(self) -> &'static str {
        match self {
            Self::SharedStyle => "style sharing",
            Self::MatchedDeclarations => "the matched declarations cache",
            Self::MovedCustomProperties => "a move of its custom properties",
            Self::ComputationCaches => "a computation with the caches of its thread",
            Self::PseudoElementRecord => "the matched declarations cache, for a pseudo-element",
        }
    }
}

#[derive(Clone, Default)]
pub(in super::super) struct CacheAudit {
    /// How many records the audit checked, by what gave them.
    checked: [u64; Reuse::COUNT],
    /// How many elements the audit checked nothing of, or nothing of the pseudo-elements of, as
    /// recomputation declined to style them or waited on a font or a random base value.
    declined: u64,
    failures: Vec<String>,
}

impl CacheAudit {
    /// The audit of a thread of a pass over `context`, where the context asks for one and the pass
    /// has caches to audit.
    pub(super) fn for_pass(context: &PassContext<'_>) -> Option<Box<Self>> {
        (context.uses_caches && context.cache_audit != Mode::Off).then(Box::default)
    }

    pub(in super::super) fn add(&mut self, other: Self) {
        for (checked, other) in self.checked.iter_mut().zip(other.checked) {
            *checked += other;
        }
        self.declined += other.declined;
        self.failures.extend(other.failures);
    }

    /// Print what differs from its recomputation to stderr, and abort where `mode` asks to.
    pub(super) fn conclude(&self, mode: Mode) {
        if self.failures.is_empty() {
            return;
        }
        eprintln!(
            "Style cache audit: what a pass reused differs from recomputing it without caches in {} places (live is what the pass holds, cold the recomputation):",
            self.failures.len()
        );
        for failure in &self.failures {
            eprintln!("    {failure}");
        }
        assert!(
            mode != Mode::Abort,
            "a cache gave a record recomputation does not; set LIBWEB_STYLE_AUDIT_CACHES=report to only print the differences"
        );
    }
}

impl Scratch<'_> {
    /// Check the record `context` holds for `node`, which `reuse` gave it, and the records of its
    /// pseudo-elements, against styling it again with every cache off, where the audit is on.
    pub(super) fn audit_caches(&mut self, context: &PassContext<'_>, node: StyleNodeID, reuse: Reuse) {
        let Some(audit) = self.cache_audit.as_deref_mut() else {
            return;
        };
        let Some(live) = context.records.get_shared(node) else {
            return;
        };
        let Some(recomputed) = recompute(context, node) else {
            audit.declined += 1;
            return;
        };
        let cold = &recomputed.record;
        let state = context.state;
        let name_of = |atom: StyleAtomID| state.atoms.name_of(atom).unwrap_or_else(|| format!("atom{}", atom.0));
        let element = || describe(state, node, &name_of);
        audit.checked[reuse as usize] += 1;
        // NB: What an element matched decides its style, so a record differing for other matches
        //     than recomputation finds differs for those, whatever the caches gave.
        if let Some(live_matches) = context.records.matches_of(node).filter(|_| context.keeps_matches)
            && live_matches[..] != recomputed.matches[..]
        {
            let only_in = |first: &[u32], second: &[u32]| {
                first
                    .iter()
                    .filter(|entry| !second.contains(entry))
                    .map(|&entry| match context.index.entries.entry(entry as usize).pseudo_kind {
                        u8::MAX => entry.to_string(),
                        kind => format!("{entry} ({})", pseudo_element_name(kind)),
                    })
                    .collect::<Vec<_>>()
                    .join(", ")
            };
            audit.failures.push(format!(
                "{}, from {}: live matched other rule entries than cold, live alone [{}], cold alone [{}]",
                element(),
                reuse.name(),
                only_in(live_matches, &recomputed.matches),
                only_in(&recomputed.matches, live_matches)
            ));
        }
        let differences = record_differences(live, cold);
        if !differences.is_empty() {
            audit.failures.push(format!(
                "{}, from {}: live differs from cold in {}",
                element(),
                reuse.name(),
                differences.join(", ")
            ));
        }
        // NB: An element whose pseudo-element waits on a font waits in the pass too, which styles
        //     it again once the font resolved.
        let Some(cold_pseudo_elements) = &recomputed.pseudo_elements else {
            audit.declined += 1;
            return;
        };
        let live_pseudo_elements = context.records.pseudo_element_records(node);
        let targets = |records: &[(PseudoElementTarget, Arc<Record>)]| {
            records.iter().map(|(target, _)| *target).collect::<Vec<_>>()
        };
        if targets(live_pseudo_elements) != targets(cold_pseudo_elements) {
            let names = |targets: Vec<PseudoElementTarget>| {
                targets.into_iter().map(pseudo_element_target_name).collect::<Vec<_>>()
            };
            audit.failures.push(format!(
                "{}, from {}: live holds the pseudo-elements {:?}, cold computes {:?}",
                element(),
                reuse.name(),
                names(targets(live_pseudo_elements)),
                names(targets(cold_pseudo_elements))
            ));
            return;
        }
        for ((target, live), (_, cold)) in live_pseudo_elements.iter().zip(cold_pseudo_elements) {
            let reuse = match self.pseudo_element_cache_hits.contains(target) {
                true => Reuse::PseudoElementRecord,
                false => reuse,
            };
            audit.checked[reuse as usize] += u64::from(reuse == Reuse::PseudoElementRecord);
            let differences = record_differences(live, cold);
            if !differences.is_empty() {
                audit.failures.push(format!(
                    "{} of {}, from {}: live differs from cold in {}",
                    pseudo_element_target_name(*target),
                    element(),
                    reuse.name(),
                    differences.join(", ")
                ));
            }
        }
    }
}

/// What an element styled from nothing comes to.
struct Recomputed {
    record: Arc<Record>,
    /// None where one of them waits on a font or a random base value.
    pseudo_elements: Option<PseudoElementRecords>,
    /// The entries of the rule index the element matched.
    matches: Vec<u32>,
}

/// The record of `node` and those of its pseudo-elements, styled from nothing on a scratch of
/// their own with every cache off, over the records `context` holds for the elements before it,
/// the pseudo-elements over the one it holds for `node`. None where that declines to style the
/// element, or waits on a font or a random base value.
fn recompute(context: &PassContext<'_>, node: StyleNodeID) -> Option<Recomputed> {
    let state = context.state;
    // NB: The pass computes the document element against the root font it held before, and its
    //     pseudo-elements and every element after it against the one it computes.
    let is_document_element = is_document_element(state, node);
    let reference = PassContext {
        uses_caches: false,
        ..PassContext::new(
            state,
            context.index,
            context.initial,
            context.records,
            context.held_root_font,
        )
    };
    let note_root_font = || {
        if let Some(&root_font) = context.root_font.get() {
            let _ = reference.root_font.set(root_font);
        }
    };
    if !is_document_element {
        note_root_font();
    }
    let mut scratch = Scratch::new(&reference);
    let facts = state.projection.facts(node)?;
    scratch.enter_parent(&reference, state.projection.parent(node));
    scratch
        .matcher
        .match_element(node, facts, &scratch.filter, &mut scratch.matches)
        .ok()?;
    scratch.apply_container_gates(&reference, node, false);
    if !scratch.cascade_element(&reference, node, state.backed_host_pseudo_element(node)) {
        return None;
    }
    let (record, _) = scratch.compute(&reference, node, None).ok()?;
    if is_document_element {
        note_root_font();
    }
    let pseudo_elements = scratch
        .styled_pseudo_elements(&reference, node)
        .map(|(pseudo_elements, _)| pseudo_elements);
    Some(Recomputed {
        record,
        pseudo_elements: pseudo_elements.filter(|_| scratch.requested() == (0, 0)),
        matches: scratch.matches.iter().map(|matched| matched.entry).collect(),
    })
}

#[cfg(test)]
pub(super) mod tests {
    use super::super::super::test_support::*;
    use super::*;

    /// `root -> [a, b, c, d -> e -> f -> g]`, every element but the root an `.item`, whose style
    /// and `::before` every one shares or reuses from another but the first.
    fn document_of_items() -> (StyleEngine, Vec<StyleNodeID>) {
        let mut engine = hosted_engine();
        let nodes = allocate_nodes(&mut engine, 8);
        engine.record_tree_delta(nodes[0], None, Some(relations(None, None, None)));
        for index in 1..5 {
            let previous = (index > 1).then(|| nodes[index - 1]);
            engine.record_tree_delta(nodes[index], None, Some(relations(Some(nodes[0]), previous, None)));
        }
        for index in 5..8 {
            engine.record_tree_delta(nodes[index], None, Some(relations(Some(nodes[index - 1]), None, None)));
        }
        engine.set_element_adjustment_facts(nodes[0], bridge::element_adjustment_fact::IS_DOCUMENT_ELEMENT);
        for &node in &nodes {
            set_tag(&mut engine, node, "div");
        }
        for &node in &nodes[1..] {
            add_class(&mut engine, node, "item");
        }
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(
            &mut engine,
            sheet,
            ".item { color: rgb(0, 128, 0); width: 10px; --name: 1; } .item::before { content: \"x\"; }",
        );
        (engine, nodes)
    }

    #[test]
    fn what_live_style_reuses_is_what_it_recomputes() {
        let (mut engine, nodes) = document_of_items();
        start_live(&mut engine, nodes[0]);
        // NB: The engine starting again keeps the audit it started with, here one reporting
        //     what differs.
        engine
            .retained
            .live_style
            .as_mut()
            .expect("the engine runs")
            .cache_audit = Mode::Report;
        start_live(&mut engine, nodes[0]);
        add_class(&mut engine, nodes[2], "other");
        remove_class(&mut engine, nodes[6], "item");
        run_style_update(&mut engine, nodes[0]);
        let audit = &engine
            .retained
            .live_style
            .as_ref()
            .expect("the engine runs")
            .cache_audits;
        assert_eq!(audit.failures, Vec::<String>::new());
        for reuse in [
            Reuse::SharedStyle,
            Reuse::MatchedDeclarations,
            Reuse::ComputationCaches,
            Reuse::PseudoElementRecord,
        ] {
            assert_ne!(
                audit.checked[reuse as usize],
                0,
                "the audit checks records from {}",
                reuse.name()
            );
        }
    }

    /// Style `nodes` in turn on one scratch with its caches on, having `poison` seed its caches
    /// before the last one, and answer what the audit found.
    fn poisoned_pass(
        engine: &mut StyleEngine,
        nodes: &[StyleNodeID],
        poison: impl FnOnce(&mut Scratch<'_>, &PassContext<'_>),
    ) -> CacheAudit {
        let initial = engine
            .computed_initial(nodes[0])
            .expect("the document has initial values");
        let state = &engine.retained;
        let index = super::super::super::rules::RuleIndex::build(&state.rules, state.process_rule_indexes());
        let mut cells = super::super::super::records::RecordCells::default();
        cells.reserve(state.projection.element_slot_count());
        let records = super::super::super::records::Records::new(state, &cells, None);
        let context = PassContext {
            cache_audit: Mode::Report,
            ..PassContext::new(state, &index, &initial, &records, None)
        };
        let mut scratch = Scratch::new(&context);
        let (last, first) = nodes.split_last().expect("there are elements to style");
        for &node in first {
            scratch.style_element(&context, node, None, super::super::PARENT_CHANGED);
        }
        poison(&mut scratch, &context);
        scratch.style_element(&context, *last, None, super::super::PARENT_CHANGED);
        *scratch.cache_audit.take().expect("the scratch audits")
    }

    #[test]
    fn a_shared_style_that_differs_from_recomputation_is_reported() {
        let (mut engine, nodes) = document_of_items();
        start_live(&mut engine, nodes[0]);
        let audit = poisoned_pass(&mut engine, &nodes[..3], |scratch, context| {
            // Seed a candidate the second item agrees with, holding the root's record.
            let state = context.state;
            let facts = state.projection.facts(nodes[2]).unwrap();
            let key = super::super::super::sharing::key_of(state, context.index, context.records, nodes[2], facts)
                .expect("the item can share");
            let wrong = context.records.get_shared(nodes[0]).unwrap().clone();
            scratch.sharing = Default::default();
            scratch.sharing.insert(key, wrong, nodes[1], Default::default());
        });
        assert_eq!(audit.checked[Reuse::SharedStyle as usize], 1);
        // NB: The `::before` the item shares recomputes over the record the item holds.
        assert_eq!(audit.failures.len(), 2, "{:?}", audit.failures);
        assert!(audit.failures[1].starts_with("::before of "), "{:?}", audit.failures);
        let failure = &audit.failures[0];
        for part in [
            &format!(
                "(node {}), from style sharing: live differs from cold in ",
                nodes[2].raw()
            )[..],
            "width: live auto, cold 10px",
            "custom properties",
        ] {
            assert!(failure.contains(part), "{failure} names {part}");
        }
    }

    #[test]
    fn a_matched_declarations_record_that_differs_from_recomputation_is_reported() {
        let (mut engine, nodes) = document_of_items();
        start_live(&mut engine, nodes[0]);
        let audit = poisoned_pass(&mut engine, &nodes[..3], |scratch, context| {
            // Have every element record the pass keeps be the root's record.
            let wrong = context.records.get_shared(nodes[0]).unwrap();
            context.matched_declarations().poison_with(wrong, false);
            scratch.sharing = Default::default();
        });
        assert_eq!(audit.checked[Reuse::MatchedDeclarations as usize], 1);
        assert!(
            audit.failures[0].contains(&format!(
                "(node {}), from the matched declarations cache: live differs from cold in ",
                nodes[2].raw()
            )),
            "{:?}",
            audit.failures
        );
    }

    #[test]
    fn a_pseudo_element_record_that_differs_from_recomputation_is_reported() {
        let (mut engine, nodes) = document_of_items();
        start_live(&mut engine, nodes[0]);
        let audit = poisoned_pass(&mut engine, &nodes[..3], |scratch, context| {
            // Have every pseudo-element record the pass keeps be the root's record.
            let wrong = context.records.get_shared(nodes[0]).unwrap();
            context.matched_declarations().poison_with(wrong, true);
            scratch.sharing = Default::default();
        });
        assert_eq!(audit.checked[Reuse::PseudoElementRecord as usize], 1);
        assert_eq!(audit.failures.len(), 1, "{:?}", audit.failures);
        let failure = &audit.failures[0];
        // NB: The atoms of a test's engine name no strings the audit could show.
        assert!(failure.starts_with("::before of <"), "{failure}");
        assert!(
            failure.contains(&format!(
                "(node {}), from the matched declarations cache, for a pseudo-element: live differs from cold in ",
                nodes[2].raw()
            )),
            "{failure}"
        );
    }
}
