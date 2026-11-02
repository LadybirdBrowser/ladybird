/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! How many times a style update allocates once the engine owns a document and has seen
//! the same transactions before. A transaction that changes no style should allocate nothing, in
//! the engine (invalidating, restyling and installing) or anywhere else.
//!
//! The engine runs on the calling thread here, where every allocation is counted. Run with
//! LIBWEB_TEST_ALLOCATION_BACKTRACES set to see where a test's allocations come from.

use super::super::bridge::element_adjustment_fact;
use super::super::*;
use super::allocation_count::{Allocations, count_allocations};
use super::test_support::*;

/// How many times each transaction a test measures is taken before, for every buffer it grows
/// to reach its size.
const WARMUP_ROUNDS: usize = 4;

/// How many times a test measures each transaction.
const MEASURED_ROUNDS: usize = 3;

/// How many times std's `Mutex` allocates in a transaction that restyles one element. On Apple
/// platforms it wraps a pthread mutex, which it boxes the first time it is locked: here the
/// scratch slot of the calling thread, made for each pass, and the longhand table recipe of the
/// element's new record. Elsewhere it is a futex, which allocates nothing.
const MUTEX_ALLOCATIONS_RESTYLING_ONE_ELEMENT: u64 = if cfg!(target_vendor = "apple") { 2 } else { 0 };

struct Document {
    engine: StyleEngine,
    root: StyleNodeID,
    leaf: StyleNodeID,
    /// A class one selector names.
    target: StyleAtomID,
    /// A class no selector names.
    unnamed: StyleAtomID,
}

impl Document {
    /// Builds `root -> [a, b, c]` with one rule for the class `target`, owned by the engine,
    /// and takes every transaction the tests measure `WARMUP_ROUNDS` times.
    fn warm() -> Self {
        let (mut engine, nodes) = linear_document();
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(&mut engine, sheet, ".target { color: rgb(0, 128, 0); }");
        let target = atom(&mut engine, "target");
        let unnamed = atom(&mut engine, "unnamed");
        start_live(&mut engine, nodes[0]);
        let mut document = Self {
            engine,
            root: nodes[0],
            leaf: nodes[3],
            target,
            unnamed,
        };
        for _ in 0..WARMUP_ROUNDS {
            document.toggle(target);
            document.toggle(unnamed);
            document.take_transaction();
        }
        document
    }

    fn take_transaction(&mut self) -> Allocations {
        let (engine, root) = (&mut self.engine, self.root);
        count_allocations(|| run_style_update(engine, root)).1
    }

    /// Adds `class` to the leaf in one transaction and removes it in another, and answers what
    /// each transaction allocated.
    fn toggle(&mut self, class: StyleAtomID) -> [Allocations; 2] {
        add_feature(&mut self.engine, self.leaf, LocalFeatureKey::Class(class));
        let added = self.take_transaction();
        remove_feature(&mut self.engine, self.leaf, LocalFeatureKey::Class(class));
        let removed = self.take_transaction();
        [added, removed]
    }

    /// What each of `MEASURED_ROUNDS` rounds of `transactions` allocated.
    fn measure(&mut self, mut transactions: impl FnMut(&mut Self) -> Vec<Allocations>) -> Vec<Allocations> {
        (0..MEASURED_ROUNDS).flat_map(|_| transactions(self)).collect()
    }

    fn empty_transactions(&mut self) -> Vec<Allocations> {
        self.measure(|document| vec![document.take_transaction()])
    }

    fn unnamed_class_toggles(&mut self) -> Vec<Allocations> {
        let unnamed = self.unnamed;
        self.measure(|document| document.toggle(unnamed).to_vec())
    }

    fn target_class_toggles(&mut self) -> Vec<Allocations> {
        let target = self.target;
        self.measure(|document| document.toggle(target).to_vec())
    }
}

fn in_style(allocations: &[Allocations]) -> Vec<u64> {
    allocations.iter().map(|allocations| allocations.in_style).collect()
}

fn everywhere(allocations: &[Allocations]) -> Vec<u64> {
    allocations.iter().map(|allocations| allocations.everywhere).collect()
}

fn assert_at_most(counts: Vec<u64>, bound: u64, what: &str) {
    assert!(
        counts.iter().all(|&count| count <= bound),
        "{what} allocate {counts:?} times, more than {bound}"
    );
}

#[test]
fn an_empty_transaction_allocates_nothing_in_style() {
    let allocations = Document::warm().empty_transactions();
    assert_at_most(in_style(&allocations), 0, "empty transactions");
}

#[test]
fn a_class_no_selector_names_allocates_nothing_in_style() {
    let allocations = Document::warm().unnamed_class_toggles();
    assert_at_most(in_style(&allocations), 0, "toggles of a class no selector names");
}

#[test]
fn a_class_one_selector_names_on_a_leaf_allocates_at_most_as_today() {
    let allocations = Document::warm().target_class_toggles();
    assert_at_most(
        in_style(&allocations),
        91 + MUTEX_ALLOCATIONS_RESTYLING_ONE_ELEMENT,
        "toggles of a class one selector names",
    );
    // FIXME: Lower this as the allocations an empty transaction makes outside
    //        the engine go away.
    assert_at_most(
        everywhere(&allocations),
        114 + MUTEX_ALLOCATIONS_RESTYLING_ONE_ELEMENT,
        "toggles of a class one selector names",
    );
}

/// What taking an element out of a list allocates in style, under `depth` ancestors, where the only
/// selector reading siblings tests nothing of the element or of the ancestors.
fn allocations_of_an_element_leaving_at_depth(depth: usize) -> Vec<u64> {
    let mut engine = hosted_engine();
    let nodes = allocate_nodes(&mut engine, depth + 2 + WARMUP_ROUNDS + MEASURED_ROUNDS);
    let (ancestors, others) = nodes.split_at(depth + 1);
    let (kept, leaving) = others.split_first().expect("the document holds the element kept");
    let (root, parent, kept) = (ancestors[0], ancestors[depth], *kept);
    engine.record_tree_delta(root, None, Some(relations(None, None, None)));
    for pair in ancestors.windows(2) {
        engine.record_tree_delta(pair[1], None, Some(relations(Some(pair[0]), None, None)));
    }
    engine.record_tree_delta(kept, None, Some(relations(Some(parent), None, None)));
    engine.set_element_adjustment_facts(root, element_adjustment_fact::IS_DOCUMENT_ELEMENT);
    for &node in ancestors.iter().chain([&kept]) {
        set_tag(&mut engine, node, "div");
    }
    for &node in ancestors {
        add_class(&mut engine, node, "ancestor");
        add_class(&mut engine, node, "other-ancestor");
    }
    add_class(&mut engine, kept, "y");
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(&mut engine, sheet, ".x ~ .y { width: 10px; }");
    start_live(&mut engine, root);

    let in_the_list = relations(Some(parent), None, Some(kept));
    let allocations: Vec<u64> = leaving
        .iter()
        .map(|&node| {
            engine.record_tree_delta(node, None, Some(in_the_list));
            set_tag(&mut engine, node, "div");
            add_class(&mut engine, node, "leaving");
            run_style_update(&mut engine, root);
            engine.record_tree_delta(node, Some(in_the_list), None);
            count_allocations(|| run_style_update(&mut engine, root)).1.in_style
        })
        .collect();
    allocations[WARMUP_ROUNDS..].to_vec()
}

#[test]
fn an_element_leaving_reads_no_ancestors_where_no_selector_asks() {
    assert_eq!(
        allocations_of_an_element_leaving_at_depth(64),
        allocations_of_an_element_leaving_at_depth(2)
    );
}

#[test]
fn a_class_flip_of_a_rule_that_loses_computes_nothing() {
    let (mut engine, nodes) = linear_document();
    let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
    add_rules(
        &mut engine,
        sheet,
        ".loser { color: rgb(2, 2, 2); } .winner { color: rgb(1, 1, 1); }",
    );
    add_class(&mut engine, nodes[1], "winner");
    start_live(&mut engine, nodes[0]);
    let (root, element) = (nodes[0], nodes[1]);
    let mut round = || {
        add_class(&mut engine, element, "loser");
        let (_, added) = count_allocations(|| run_style_update(&mut engine, root));
        remove_class(&mut engine, element, "loser");
        let (_, removed) = count_allocations(|| run_style_update(&mut engine, root));
        [added, removed]
    };
    for _ in 0..WARMUP_ROUNDS {
        round();
    }
    let allocations: Vec<Allocations> = (0..MEASURED_ROUNDS).flat_map(|_| round()).collect();
    // NB: Computing the record again allocates about twice this.
    assert_at_most(
        in_style(&allocations),
        20 + MUTEX_ALLOCATIONS_RESTYLING_ONE_ELEMENT,
        "flips of a rule that loses",
    );
}
